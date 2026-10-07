#include "core/script_bridge.hpp"
#include "core/engine_hooks.hpp"
#include "core/native_invoker.hpp"
#include <windows.h>
#include <iostream>
#include <cstring>
#include <filesystem>
#include <fstream>

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
        std::cerr << "[ScriptBridge] PE header inválido en ScriptHookRDR." << std::endl;
        return;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);

    const auto& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

    if (!directory.VirtualAddress || !directory.Size) {
        std::cerr << "[ScriptBridge] ScriptHookRDR no tiene directorio de exports." << std::endl;
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
            std::cout << "[ScriptBridge] export[" << i << "] " << name << std::endl;
            ++relevant;
        }
    }

    std::cout << "[ScriptBridge] Relevant exports: "
              << relevant << std::endl;
}

HMODULE ensureScriptHookLoaded() {
    HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (hookModule) {
        return hookModule;
    }

    static std::atomic<bool> loadAttemptLogged{false};

    char gamePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, gamePath, MAX_PATH)) {
        std::string exePath(gamePath);
        const size_t slash = exePath.find_last_of("\\\\/");
        if (slash != std::string::npos) {
            exePath.resize(slash + 1);
            exePath += "ScriptHookRDR.dll";

            hookModule = LoadLibraryExA(
                exePath.c_str(),
                nullptr,
                LOAD_WITH_ALTERED_SEARCH_PATH);

            if (hookModule) {
                std::cout << "[ScriptBridge] ScriptHookRDR.dll cargado explícitamente desde: "
                          << exePath << std::endl;
                return hookModule;
            }
        }
    }

    char ownPath[MAX_PATH] = {};
    HMODULE ownModule = GetModuleHandleA("frontier_core.dll");
    if (ownModule && GetModuleFileNameA(ownModule, ownPath, MAX_PATH)) {
        std::string modulePath(ownPath);
        const size_t slash = modulePath.find_last_of("\\\\/");
        if (slash != std::string::npos) {
            modulePath.resize(slash + 1);
            modulePath += "ScriptHookRDR.dll";

            hookModule = LoadLibraryExA(
                modulePath.c_str(),
                nullptr,
                LOAD_WITH_ALTERED_SEARCH_PATH);

            if (hookModule) {
                std::cout << "[ScriptBridge] ScriptHookRDR.dll cargado explícitamente desde: "
                          << modulePath << std::endl;
                return hookModule;
            }
        }
    }

    if (!loadAttemptLogged.exchange(true, std::memory_order_acq_rel)) {
        std::cerr << "[ScriptBridge] No se pudo cargar ScriptHookRDR.dll. "
                  << "Asegúrate de que el DLL esté instalado junto a RDR.exe."
                  << " GetLastError=" << GetLastError() << std::endl;
    }

    return nullptr;
}

void resolveScriptHook() {
    HMODULE hookModule = ensureScriptHookLoaded();
    if (!hookModule) {
        return;
    }

    dumpRelevantExports(hookModule);

    // Nombres decorados del SDK de ScriptHookRDR para x64 MSVC.
    s_scriptRegister = getExportByExactName<ScriptRegisterFn>(
        hookModule, "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");
    if (!s_scriptRegister) {
        s_scriptRegister = resolveMangledExport<ScriptRegisterFn>(
            hookModule, "scriptRegister");
    }

    s_scriptWait = getExportByExactName<ScriptWaitFn>(
        hookModule, "?scriptWait@@YAXK@Z");
    if (!s_scriptWait) {
        s_scriptWait = resolveMangledExport<ScriptWaitFn>(
            hookModule, "scriptWait");
    }

    const auto nativeInit = getExportByExactName<ScriptNativeInitFn>(
        hookModule, "?nativeInit@@YAX_K@Z");
    const auto nativePush64 = getExportByExactName<ScriptNativePush64Fn>(
        hookModule, "?nativePush64@@YAX_K@Z");
    const auto nativeCall = getExportByExactName<ScriptNativeCallFn>(
        hookModule, "?nativeCall@@YAPEA_KXZ");

    NativeInvoker::setScriptHookApi(
        nativeInit,
        nativePush64,
        nativeCall);
}

} // namespace


void ScriptBridge::registerScript(HMODULE module) {
    resolveScriptHook();

    if (!s_scriptRegister || !s_scriptWait) {
        if (!s_warnedUnavailable.exchange(true, std::memory_order_acq_rel)) {
            std::cout << "[ScriptBridge] ScriptHookRDR cargado pero no se pudieron resolver "
                         "scriptRegister/scriptWait." << std::endl;
        }
        return;
    }

    if (s_registered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    s_scriptRegister(module, &ScriptBridge::scriptMain);
    std::cout << "[ScriptBridge] Hilo de script FrontierMP registrado en ScriptHookRDR." << std::endl;
}

bool ScriptBridge::isRegistered() {
    return s_registered.load(std::memory_order_acquire);
}

void __cdecl ScriptBridge::scriptMain() {
    std::cout << "[ScriptBridge] ScriptMain iniciado dentro del scheduler de RAGE." << std::endl;

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
