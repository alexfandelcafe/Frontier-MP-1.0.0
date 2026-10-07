#include "core/script_bridge.hpp"
#include "core/engine_hooks.hpp"
#include "core/native_invoker.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

namespace Frontier::Core {

namespace {

using ScriptRegisterFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn = void (*)(DWORD);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptWaitFn s_scriptWait = nullptr;
std::atomic<bool> s_exportsDumped{false};

template <typename T>
T getExportByExactName(HMODULE module, const char* name) {
    if (!module || !name) {
        return nullptr;
    }
    FARPROC proc = GetProcAddress(module, name);
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
        FARPROC proc = GetProcAddress(module, exportedName);
        if (proc) {
            return reinterpret_cast<T>(proc);
        }
    }
    return nullptr;
}

void dumpRelevantExports(HMODULE module) {
    if (!module || s_exportsDumped.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(module, modulePath, MAX_PATH);

    std::cout << "[ScriptBridge] ScriptHookRDR cargado desde: "
              << modulePath << std::endl;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        std::cerr << "[ScriptBridge] PE header inválido en ScriptHookRDR."
                  << std::endl;
        return;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);

    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        std::cerr << "[ScriptBridge] NT header inválido en ScriptHookRDR."
                  << std::endl;
        return;
    }

    const auto& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

    if (!directory.VirtualAddress || !directory.Size) {
        std::cerr
            << "[ScriptBridge] ScriptHookRDR no tiene directorio de exports."
            << std::endl;
        return;
    }

    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        reinterpret_cast<const uint8_t*>(module) + directory.VirtualAddress);

    const auto* names = reinterpret_cast<const DWORD*>(
        reinterpret_cast<const uint8_t*>(module) + exports->AddressOfNames);

    std::cout << "[ScriptBridge] Export count: "
              << exports->NumberOfNames << std::endl;

    uint32_t relevant = 0;
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(
            reinterpret_cast<const uint8_t*>(module) + names[i]);

        if (std::strstr(name, "script") ||
            std::strstr(name, "native") ||
            std::strstr(name, "Command") ||
            std::strstr(name, "command")) {
            std::cout << "[ScriptBridge] export[" << i << "] "
                      << name << std::endl;
            ++relevant;
        }
    }

    std::cout << "[ScriptBridge] Relevant exports: "
              << relevant << std::endl;
}

void resolveScriptHook(HMODULE frontierModule) {
    (void)frontierModule;

    // The launcher owns ScriptHookRDR loading. Frontier only observes an
    // already-loaded module here so it cannot accidentally race or replace
    // the launch order.
    HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    // Resolve the registration API first. ScriptHookRDR starts its own
    // initialization asynchronously from DllMain, so any expensive diagnostic
    // work here can make us miss the only useful registration window.
    s_scriptRegister = getExportByExactName<ScriptRegisterFn>(
        hookModule,
        "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    if (!s_scriptRegister) {
        s_scriptRegister =
            resolveMangledExport<ScriptRegisterFn>(
                hookModule,
                "scriptRegister");
    }

    s_scriptRegisterAdditionalThread =
        getExportByExactName<ScriptRegisterAdditionalThreadFn>(
            hookModule,
            "?scriptRegisterAdditionalThread@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    if (!s_scriptRegisterAdditionalThread) {
        s_scriptRegisterAdditionalThread =
            resolveMangledExport<ScriptRegisterAdditionalThreadFn>(
                hookModule,
                "scriptRegisterAdditionalThread");
    }

    s_scriptWait = getExportByExactName<ScriptWaitFn>(
        hookModule,
        "?scriptWait@@YAXK@Z");

    if (!s_scriptWait) {
        s_scriptWait =
            resolveMangledExport<ScriptWaitFn>(
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

    Nvoid resolveScriptHook(HMODULE frontierModule) {
    (void)frontierModule;

    // The launcher owns ScriptHookRDR loading. Frontier only observes an
    // already-loaded module here so it cannot race the loader or alter the
    // game installation.
    HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    s_scriptRegister = getExportByExactName<ScriptRegisterFn>(
        hookModule,
        "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    if (!s_scriptRegister) {
        s_scriptRegister =
            resolveMangledExport<ScriptRegisterFn>(hookModule, "scriptRegister");
    }

    s_scriptWait = getExportByExactName<ScriptWaitFn>(
        hookModule,
        "?scriptWait@@YAXK@Z");

    if (!s_scriptWait) {
        s_scriptWait =
            resolveMangledExport<ScriptWaitFn>(hookModule, "scriptWait");
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
    if (!nativeApiBound.exchange(true, std::memory_order_acq_rel)) {
        NativeInvoker::setScriptHookApi(
            nativeInit,
            nativePush64,
            nativeCall);
    }
}

} // namespace

void ScriptBridge::registerScriptEarly(HMODULE module) {
    // Do not register from DLL_PROCESS_ATTACH. ScriptHookRDR creates its own
    // worker and installs the RAGE Run/Reset/Fiber hooks asynchronously.
    // Registration is submitted once that scheduler is demonstrably ready.
    (void)module;
}

void ScriptBridge::tryRegister(HMODULE module) {
    if (!module ||
        s_registered.load(std::memory_order_acquire) ||
        s_registrationRequested.load(std::memory_order_acquire)) {
        return;
    }

    HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    resolveScriptHook(module);
    if (!s_scriptRegister) {
        return;
    }

    const auto base = reinterpret_cast<const uint8_t*>(hookModule);

    // These are ScriptHookRDR 1.5.2's own globals from the decompilation:
    // +0x20e4a8 = resolved rage::scrThread::Run target
    // +0x20e3f0 = original Run pointer saved by the hook manager.
    // Both become non-zero only after ScriptHook has completed the Run hook
    // setup that drains the registration queue.
    const uintptr_t runTarget =
        *reinterpret_cast<const uintptr_t*>(base + 0x20e4a8);
    const uintptr_t originalRun =
        *reinterpret_cast<const uintptr_t*>(base + 0x20e3f0);

    if (runTarget == 0 || originalRun == 0) {
        if (!s_schedulerReadyLogged.exchange(
                true,
                std::memory_order_acq_rel)) {
            std::cout
                << "[ScriptBridge] ScriptHookRDR cargado; esperando a que "
                   "termine el hook de rage::scrThread::Run."
                << std::endl;
        }
        return;
    }

    if (!s_schedulerReadyLogged.exchange(
            true,
            std::memory_order_acq_rel)) {
        std::cout
            << "[ScriptBridge] Scheduler de ScriptHookRDR listo: Run target=0x"
            << std::hex << runTarget
            << " original=0x" << originalRun
            << std::dec << std::endl;
    }

    if (s_registrationRequested.exchange(
            true,
            std::memory_order_acq_rel)) {
        return;
    }

    s_scriptRegister(module, &ScriptBridge::scriptMain);

    std::cout
        << "[ScriptBridge] scriptRegister enviado después de instalar "
           "rage::scrThread::Run."
        << std::endl;
}

bool ScriptBridge::isRegistered() {
    return s_registered.load(std::memory_order_acquire);
}

void __cdecl ScriptBridge::scriptMain() {
    bool expected = false;
    if (!s_registered.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        // A second registration can exist transiently if the regular API was
        // followed by the additional-thread fallback. Do not run our game loop
        // twice.
        return;
    }

    std::cout
        << "[ScriptBridge] ScriptMain iniciado dentro del scheduler de RAGE."
        << std::endl;

    for (;;) {
        runFrame();

        if (s_scriptWait) {
            s_scriptWait(0);
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
