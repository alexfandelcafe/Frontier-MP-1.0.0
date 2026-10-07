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
using ScriptRegisterAdditionalThreadFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn = void (*)(DWORD);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptRegisterAdditionalThreadFn s_scriptRegisterAdditionalThread = nullptr;
ScriptWaitFn s_scriptWait = nullptr;
std::atomic<bool> s_exportsDumped{false};
std::atomic<uint64_t> s_registrationRequestedAt{0};

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

HMODULE ensureScriptHookLoaded(HMODULE frontierModule) {
    HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (hookModule) {
        std::cout << "[ScriptBridge] ScriptHookRDR.dll ya estaba cargado en 0x"
                  << std::hex
                  << reinterpret_cast<uintptr_t>(hookModule)
                  << std::dec << std::endl;
        return hookModule;
    }

    static std::atomic<bool> loadDiagnosticsPrinted{false};
    DWORD clientLoadError = ERROR_SUCCESS;

    // Frontier controls this runtime dependency. Never look in the game folder.
    char ownPath[MAX_PATH] = {};

    if (frontierModule &&
        GetModuleFileNameA(frontierModule, ownPath, MAX_PATH)) {

        std::string modulePath(ownPath);
        const size_t slash = modulePath.find_last_of("\\/");

        if (slash != std::string::npos) {
            modulePath.resize(slash + 1);
            modulePath += "ScriptHookRDR.dll";

            hookModule = LoadLibraryExA(
                modulePath.c_str(),
                nullptr,
                LOAD_WITH_ALTERED_SEARCH_PATH);

            if (hookModule) {
                std::cout
                    << "[ScriptBridge] ScriptHookRDR.dll cargado desde Frontier: "
                    << modulePath << std::endl;
                return hookModule;
            }

            clientLoadError = GetLastError();

            if (!loadDiagnosticsPrinted.exchange(
                    true,
                    std::memory_order_acq_rel)) {
                std::cerr
                    << "[ScriptBridge] Falló LoadLibraryEx de la dependencia "
                       "del cliente: "
                    << modulePath
                    << " | GetLastError=" << clientLoadError
                    << std::endl;
            }
        }
    }

    std::ostringstream diagnostic;
    diagnostic
        << "[ScriptBridge] ScriptHookRDR.dll no pudo cargarse desde el cliente"
        << " | clientError=" << clientLoadError;

    std::cerr << diagnostic.str() << std::endl;
    return nullptr;
}

void resolveScriptHook(HMODULE frontierModule) {
    HMODULE hookModule = ensureScriptHookLoaded(frontierModule);
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

    NativeInvoker::setScriptHookApi(
        nativeInit,
        nativePush64,
        nativeCall);
}

} // namespace

void ScriptBridge::registerScriptEarly(HMODULE module) {
    // Fast path for the normal SDK lifecycle: if ScriptHook is already loaded
    // when Frontier attaches, register immediately. No logging, filesystem I/O,
    // or native initialization is performed here.
    HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule || !module) {
        return;
    }

    const auto registerFn = getExportByExactName<ScriptRegisterFn>(
        hookModule,
        "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    if (!registerFn) {
        return;
    }

    registerFn(module, &ScriptBridge::scriptMain);
    s_registrationRequestedAt.store(GetTickCount64(), std::memory_order_release);
}

void ScriptBridge::tryRegister(HMODULE module) {
    if (!module || s_registered.load(std::memory_order_acquire)) {
        return;
    }

    HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    // Resolve the registration functions only after leaving DLL_PROCESS_ATTACH.
    s_scriptRegister = getExportByExactName<ScriptRegisterFn>(
        hookModule,
        "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");
    if (!s_scriptRegister) {
        s_scriptRegister =
            resolveMangledExport<ScriptRegisterFn>(hookModule, "scriptRegister");
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
            resolveMangledExport<ScriptWaitFn>(hookModule, "scriptWait");
    }

    if (!s_scriptRegister) {
        return;
    }

    const uint64_t now = GetTickCount64();
    const uint64_t requestedAt =
        s_registrationRequestedAt.load(std::memory_order_acquire);

    // First attempt happens once, as soon as ScriptHook's module exists. This
    // mirrors the ASI lifecycle without requiring the game installation.
    if (requestedAt == 0) {
        s_scriptRegister(module, &ScriptBridge::scriptMain);
        s_registrationRequestedAt.store(now, std::memory_order_release);
        std::cout
            << "[ScriptBridge] scriptRegister enviado en cuanto ScriptHookRDR "
               "apareció en el proceso."
            << std::endl;
        return;
    }

    // If the regular registration was not materialized into a script stack
    // after a few seconds, use the dedicated additional-thread API once. The
    // callback itself protects against executing two copies of our script.
    if (!s_additionalThreadRegistrationAttempted.load(std::memory_order_acquire) &&
        now >= requestedAt + 3000 &&
        s_scriptRegisterAdditionalThread) {

        bool expected = false;
        if (s_additionalThreadRegistrationAttempted.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            s_scriptRegisterAdditionalThread(module, &ScriptBridge::scriptMain);
            std::cout
                << "[ScriptBridge] scriptRegisterAdditionalThread enviado como "
                   "fallback después de 3 segundos."
                << std::endl;
        }
    }
}

void ScriptBridge::initialize(HMODULE module) {
    resolveScriptHook(module);
    tryRegister(module);

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
