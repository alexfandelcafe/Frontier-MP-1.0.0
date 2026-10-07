#include "core/script_bridge.hpp"
#include "core/engine_hooks.hpp"
#include "core/native_invoker.hpp"
#include <windows.h>
#include <iostream>
#include <cstring>

namespace Frontier::Core {

namespace {

using ScriptRegisterFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn = void (*)(DWORD);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptWaitFn s_scriptWait = nullptr;

template <typename T>
T resolveMangledExport(HMODULE module, const char* token) {
    if (!module || !token || !*token) {
        return nullptr;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERSA*>(
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

        const char* match = std::strstr(exportedName, token);
        if (!match) {
            continue;
        }

        const char after = match[std::strlen(token)];
        if (after != '\0' && after != '@') {
            continue;
        }

        FARPROC proc = GetProcAddress(module, exportedName);
        if (proc) {
            return reinterpret_cast<T>(proc);
        }
    }

    return nullptr;
}

void resolveScriptHook() {
    HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    s_scriptRegister = resolveMangledExport<ScriptRegisterFn>(
        hookModule, "scriptRegister");
    s_scriptWait = resolveMangledExport<ScriptWaitFn>(
        hookModule, "scriptWait");

    const auto nativeInit = resolveMangledExport<ScriptNativeInitFn>(
        hookModule, "nativeInit");
    const auto nativePush64 = resolveMangledExport<ScriptNativePush64Fn>(
        hookModule, "nativePush64");
    const auto nativeCall = resolveMangledExport<ScriptNativeCallFn>(
        hookModule, "nativeCall");
    const auto getCommandFromHash = resolveMangledExport<ScriptGetCommandFn>(
        hookModule, "getCommandFromHash");

    NativeInvoker::setScriptHookApi(
        nativeInit,
        nativePush64,
        nativeCall,
        getCommandFromHash);
}

} // namespace

void ScriptBridge::registerScript(HMODULE module) {
    resolveScriptHook();

    if (!s_scriptRegister || !s_scriptWait) {
        if (!s_warnedUnavailable.exchange(true, std::memory_order_acq_rel)) {
            std::cout << "[ScriptBridge] No se pudieron resolver los exports scriptRegister/scriptWait de ScriptHookRDR." << std::endl;
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
