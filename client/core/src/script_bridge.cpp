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

bool readScriptHookGlobal(HMODULE module, uintptr_t rva, uintptr_t& value) {
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

    value = *reinterpret_cast<const uintptr_t*>(address);
    return value != 0;
}

bool isScriptHookRunSchedulerReady(HMODULE hookModule) {
    // ScriptHookRDR 1.5.2 decompilation:
    //   DAT_18020e4a8 -> resolved rage::scrThread::Run target
    //   DAT_18020e3f0 -> original Run pointer saved by hook manager
    // Their RVAs are fixed in this 1.5.2 binary.
    uintptr_t runTarget = 0;
    uintptr_t originalRun = 0;

    return readScriptHookGlobal(hookModule, 0x20e4a8, runTarget) &&
           readScriptHookGlobal(hookModule, 0x20e3f0, originalRun);
}

void resolveScriptHook(HMODULE frontierModule) {
    (void)frontierModule;

    // The launcher owns ScriptHookRDR loading. Frontier only observes an
    // already-loaded module and binds to its SDK/native exports.
    const HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    s_scriptRegister = getExportByExactName<ScriptRegisterFn>(
        hookModule,
        "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    if (!s_scriptRegister) {
        s_scriptRegister =
            resolveMangledExport<ScriptRegisterFn>(
                hookModule,
                "scriptRegister");
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
    // Registration is deliberately deferred. ScriptHookRDR starts a worker
    // from its own DLL_PROCESS_ATTACH and installs its RAGE scheduler hooks
    // asynchronously, so registering before Run is hooked can leave the
    // request stranded until the next loader event.
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

    if (!isScriptHookRunSchedulerReady(hookModule)) {
        static std::atomic<bool> schedulerWaitLogged{false};
        if (!schedulerWaitLogged.exchange(
                true,
                std::memory_order_acq_rel)) {
            std::cout
                << "[ScriptBridge] ScriptHookRDR está cargado; esperando que "
                   "termine de instalar rage::scrThread::Run antes de registrar."
                << std::endl;
        }
        return;
    }

    if (!s_schedulerReadyLogged.exchange(
            true,
            std::memory_order_acq_rel)) {
        uintptr_t runTarget = 0;
        uintptr_t originalRun = 0;
        readScriptHookGlobal(hookModule, 0x20e4a8, runTarget);
        readScriptHookGlobal(hookModule, 0x20e3f0, originalRun);

        std::cout
            << "[ScriptBridge] Scheduler ScriptHookRDR listo: Run target=0x"
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
        << "[ScriptBridge] scriptRegister enviado después de que ScriptHookRDR "
           "instaló rage::scrThread::Run."
        << std::endl;
}

void ScriptBridge::initialize(HMODULE module) {
    resolveScriptHook(module);
    tryRegister(module);

    if (!GetModuleHandleA("ScriptHookRDR.dll")) {
        return;
    }

    if (!s_scriptWait) {
        if (!s_warnedUnavailable.exchange(
                true,
                std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] ScriptHookRDR está cargado, pero scriptWait "
                   "no pudo resolverse."
                << std::endl;
        }
    }
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
