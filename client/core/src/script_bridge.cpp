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

template <typename T>
bool readScriptHookValue(HMODULE module, uintptr_t rva, T& value) {
    if (!module) {
        return false;
    }

    const auto address =
        reinterpret_cast<const uint8_t*>(module) + rva;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(
            reinterpret_cast<const void*>(address),
            &mbi,
            sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }

    if (mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_GUARD) ||
        (mbi.Protect & 0xff) == PAGE_NOACCESS) {
        return false;
    }

    __try {
        value = *reinterpret_cast<const T*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return true;
}

bool isScriptHookSchedulerReady(HMODULE hookModule) {
    // ScriptHookRDR 1.5.2 decompilation:
    //   DAT_18020e4a8 = resolved rage::scrThread::Run target
    //   DAT_18020e3f0 = original Run trampoline after hook installation
    //   DAT_18020e400 = original Reset trampoline
    //   DAT_18020e408 = original ConvertThreadToFiber trampoline
    // We only READ these globals as a passive initialization signal.
    uintptr_t runTarget = 0;
    uintptr_t originalRun = 0;
    uintptr_t originalReset = 0;
    uintptr_t originalConvertFiber = 0;

    return readScriptHookValue(hookModule, 0x20e4a8, runTarget) &&
           readScriptHookValue(hookModule, 0x20e3f0, originalRun) &&
           readScriptHookValue(hookModule, 0x20e400, originalReset) &&
           readScriptHookValue(hookModule, 0x20e408, originalConvertFiber) &&
           runTarget != 0 &&
           originalRun != 0 &&
           originalReset != 0 &&
           originalConvertFiber != 0;
}


void resolveScriptHook(HMODULE frontierModule) {
    (void)frontierModule;

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
    // DllMain must remain free of ScriptHook calls. The worker thread calls
    // initialize() immediately after startup so registration can be queued
    // before ScriptHook's own startup scan advances too far.
    (void)module;
}

void ScriptBridge::tryRegister(HMODULE module) {
    if (!module ||
        s_registered.load(std::memory_order_acquire) ||
        s_registrationRequested.load(std::memory_order_acquire)) {
        return;
    }

    const HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    resolveScriptHook(module);

    if (!s_scriptRegister) {
        return;
    }

    // Do not register while ScriptHookRDR is still inside its asynchronous
    // initialization. Its script scheduler becomes safe once all four
    // scheduler hooks have published their original pointers.
    if (!isScriptHookSchedulerReady(hookModule)) {
        static std::atomic<bool> waitLogged{false};
        if (!waitLogged.exchange(true, std::memory_order_acq_rel)) {
            std::cout
                << "[ScriptBridge] ScriptHookRDR cargado; esperando a que "
                   "termine la inicialización de Run/Reset/ConvertThreadToFiber."
                << std::endl;
        }
        return;
    }

    static std::atomic<bool> readyLogged{false};
    if (!readyLogged.exchange(true, std::memory_order_acq_rel)) {
        uintptr_t runTarget = 0;
        uintptr_t originalRun = 0;
        readScriptHookValue(hookModule, 0x20e4a8, runTarget);
        readScriptHookValue(hookModule, 0x20e3f0, originalRun);

        std::cout
            << "[ScriptBridge] Scheduler ScriptHookRDR listo: Run target=0x"
            << std::hex << runTarget
            << " original=0x" << originalRun
            << std::dec << std::endl;
    }

    if (s_registrationRequested.exchange(
            true, std::memory_order_acq_rel)) {
        return;
    }

    std::cout
        << "[ScriptBridge] Registrando Frontier mediante scriptRegister "
           "después de la inicialización de ScriptHookRDR."
        << std::endl;

    s_scriptRegister(module, &ScriptBridge::scriptMain);

    std::cout
        << "[ScriptBridge] scriptRegister enviado; ScriptHookRDR manejará "
           "el Run/Reset/fiber lifecycle."
        << std::endl;
}

void ScriptBridge::initialize(HMODULE module) {
    resolveScriptHook(module);
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
    if (!module) {
        return;
    }

    // Normally registration succeeds during initialize(). This remains a
    // cheap retry path in case ScriptHookRDR was not yet mapped when the
    // Frontier worker first started.
    tryRegister(module);
}

bool ScriptBridge::isRegistered() {
    return s_registered.load(std::memory_order_acquire);
}

void __cdecl ScriptBridge::scriptMain() {
    bool expected = false;
    if (!s_registered.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

    std::cout
        << "[ScriptBridge] ScriptMain iniciado dentro del scheduler normal "
           "de ScriptHookRDR."
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
