#include "core/script_bridge.hpp"
#include "core/engine_hooks.hpp"
#include "core/native_invoker.hpp"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace Frontier::Core {

namespace {

using ScriptRegisterFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn = void (*)(DWORD);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptWaitFn s_scriptWait = nullptr;


template <typename T>
T getExportByExactName(HMODULE module, const char* name) {
    if (!module || !name) {
        return nullptr;
    }

    const FARPROC proc = GetProcAddress(module, name);
    return proc ? reinterpret_cast<T>(proc) : nullptr;
}

template <typename T>
T resolveMangledExport(HMODULE module, const char* token) {
    if (!module || !token || !*token) {
        return nullptr;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }

    const auto& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || !directory.Size) {
        return nullptr;
    }

    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        reinterpret_cast<const uint8_t*>(module) + directory.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(
        reinterpret_cast<const uint8_t*>(module) + exports->AddressOfNames);

    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const char* exportedName = reinterpret_cast<const char*>(
            reinterpret_cast<const uint8_t*>(module) + names[i]);

        if (!std::strstr(exportedName, token)) {
            continue;
        }

        const FARPROC proc = GetProcAddress(module, exportedName);
        if (proc) {
            return reinterpret_cast<T>(proc);
        }
    }

    return nullptr;
}
void resolveScriptHook() {

    // The launcher loads ScriptHookRDR.dll before Frontier. We intentionally
    // bind only to its public SDK/native exports and leave its internal
    // rage::scrThread::Run/Reset/fiber lifecycle untouched.
    const HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    s_scriptRegister = getExportByExactName<ScriptRegisterFn>(
        hookModule,
        "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    if (!s_scriptRegister) {
        s_scriptRegister = resolveMangledExport<ScriptRegisterFn>(
            hookModule,
            "scriptRegister");
    }

    s_scriptWait = getExportByExactName<ScriptWaitFn>(
        hookModule,
        "?scriptWait@@YAXK@Z");

    if (!s_scriptWait) {
        s_scriptWait = resolveMangledExport<ScriptWaitFn>(
            hookModule,
            "scriptWait");
    }

    const auto nativeInit =
        getExportByExactName<ScriptNativeInitFn>(
            hookModule,
            "?nativeInit@@YAX_K@Z");
    const auto nativePush64 =
        getExportByExactName<ScriptNativePush64Fn>(
            hookModule,
            "?nativePush64@@YAX_K@Z");
    const auto nativeCall =
        getExportByExactName<ScriptNativeCallFn>(
            hookModule,
            "?nativeCall@@YAPEA_KXZ");

    static std::atomic<bool> nativeApiBound{false};
    if (nativeInit && nativePush64 && nativeCall &&
        !nativeApiBound.exchange(true, std::memory_order_acq_rel)) {
        NativeInvoker::setScriptHookApi(
            nativeInit,
            nativePush64,
            nativeCall);
    }
}

} // namespace

void ScriptBridge::registerScriptEarly(HMODULE module) {
    if (!module ||
        s_registered.load(std::memory_order_acquire) ||
        s_registrationRequested.load(std::memory_order_acquire)) {
        return;
    }

    // The launcher deliberately maps ScriptHookRDR.dll before frontier_core.dll.
    // Register from DLL_PROCESS_ATTACH as the SDK expects, before ScriptHook's
    // worker has a chance to perform its initial script scan.
    const HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    ScriptRegisterFn registerFn =
        getExportByExactName<ScriptRegisterFn>(
            hookModule,
            "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    if (!registerFn) {
        registerFn =
            resolveMangledExport<ScriptRegisterFn>(
                hookModule,
                "scriptRegister");
    }

    if (!registerFn) {
        return;
    }

    if (s_registrationRequested.exchange(
            true, std::memory_order_acq_rel)) {
        return;
    }

    registerFn(module, &ScriptBridge::scriptMain);

    OutputDebugStringA(
        "[ScriptBridge] scriptRegister enviado desde DLL_PROCESS_ATTACH.\n");
}

void ScriptBridge::tryRegister(HMODULE module) {
    if (!module ||
        s_registered.load(std::memory_order_acquire) ||
        s_registrationRequested.load(std::memory_order_acquire)) {
        return;
    }

    // Fallback only when early registration was impossible (for example,
    // ScriptHookRDR was not visible yet). No internal ScriptHook functions
    // or scheduler hooks are touched here.
    const HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    resolveScriptHook(module);
    if (!s_scriptRegister) {
        return;
    }

    if (s_registrationRequested.exchange(
            true, std::memory_order_acq_rel)) {
        return;
    }

    std::cout
        << "[ScriptBridge] Registrando Frontier mediante scriptRegister "
           "desde el worker (fallback)."
        << std::endl;

    s_scriptRegister(module, &ScriptBridge::scriptMain);

    std::cout
        << "[ScriptBridge] scriptRegister enviado; ScriptHookRDR manejará "
           "el Run/Reset/fiber lifecycle."
        << std::endl;
}

void ScriptBridge::initialize(HMODULE module) {
    resolveScriptHook();
    tryRegister(module);

    const HMODULE hookModule =
        GetModuleHandleA("ScriptHookRDR.dll");

    if (!hookModule) {
        return;
    }

    if (!s_scriptWait) {
        if (!s_warnedUnavailable.exchange(
                true, std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] ScriptHookRDR está cargado, pero "
                   "scriptWait no pudo resolverse."
                << std::endl;
        }
    }
}

void ScriptBridge::update(HMODULE module) {
    // Fallback only when ScriptHookRDR was not visible during DLL attach.
    // From this point onward ScriptHookRDR owns Run/Reset/fiber creation,
    // and Frontier only uses the public registration API.
    tryRegister(module);
}

bool ScriptBridge::isRegistered() {
    return s_registered.load(std::memory_order_acquire);
}

void __cdecl ScriptBridge::scriptMain() {
    // ScriptHookRDR invokes this callback as the fiber entrypoint. Do not
    // suppress subsequent invocations: Reset/load may recreate the script
    // fiber and invoke the registered callback again.
    s_registered.store(true, std::memory_order_release);

    std::cout
        << "[ScriptBridge] ScriptMain invocado por el scheduler normal de "
           "ScriptHookRDR."
        << std::endl;

    for (;;) {
        __try {
            runFrame();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::cerr
                << "[ScriptBridge] ACCESS VIOLATION/excepción SEH en "
                   "runFrame. code=0x"
                << std::hex << GetExceptionCode()
                << std::dec
                << ". Se aborta únicamente el script Frontier."
                << std::endl;
            return;
        }

        if (s_scriptWait) {
            __try {
                s_scriptWait(0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                std::cerr
                    << "[ScriptBridge] ACCESS VIOLATION/excepción SEH en "
                       "scriptWait. code=0x"
                    << std::hex << GetExceptionCode()
                    << std::dec
                    << ". Se aborta únicamente el script Frontier."
                    << std::endl;
                return;
            }
        } else {
            return;
        }
    }
}

void ScriptBridge::runFrame() {
    if (!NativeInvoker::isReady()) {
        NativeInvoker::initialize();
    }

    if (!NativeInvoker::isReady()) {
        return;
    }

    EngineHooks::processMultiplayerWorldLoad();
}

} // namespace Frontier::Core
