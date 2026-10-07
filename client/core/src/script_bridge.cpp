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
T resolveExportCandidates(HMODULE module, const char* const* names, size_t count) {
    if (!module || !names) {
        return nullptr;
    }

    for (size_t i = 0; i < count; ++i) {
        if (!names[i] || !*names[i]) {
            continue;
        }

        FARPROC proc = GetProcAddress(module, names[i]);
        if (proc) {
            std::cout << "[ScriptBridge] Export encontrado: " << names[i]
                      << " @0x" << std::hex
                      << reinterpret_cast<uintptr_t>(proc)
                      << std::dec << std::endl;
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

    const char* scriptRegisterNames[] = {
        "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z",
        "scriptRegister"
    };
    const char* scriptWaitNames[] = {
        "?scriptWait@@YAXK@Z",
        "scriptWait"
    };
    const char* nativeInitNames[] = {
        "?nativeInit@@YAX_K@Z",
        "nativeInit"
    };
    const char* nativePushNames[] = {
        "?nativePush64@@YAX_K@Z",
        "?nativePush@@YAX_K@Z",
        "nativePush64",
        "nativePush"
    };
    const char* nativeCallNames[] = {
        "?nativeCall@@YAPEA_KXZ",
        "?nativeCall@@YAPEA_K@Z",
        "nativeCall"
    };

    s_scriptRegister = resolveExportCandidates<ScriptRegisterFn>(
        hookModule, scriptRegisterNames, sizeof(scriptRegisterNames) / sizeof(scriptRegisterNames[0]));
    s_scriptWait = resolveExportCandidates<ScriptWaitFn>(
        hookModule, scriptWaitNames, sizeof(scriptWaitNames) / sizeof(scriptWaitNames[0]));

    const auto nativeInit = resolveExportCandidates<ScriptNativeInitFn>(
        hookModule, nativeInitNames, sizeof(nativeInitNames) / sizeof(nativeInitNames[0]));
    const auto nativePush64 = resolveExportCandidates<ScriptNativePush64Fn>(
        hookModule, nativePushNames, sizeof(nativePushNames) / sizeof(nativePushNames[0]));
    const auto nativeCall = resolveExportCandidates<ScriptNativeCallFn>(
        hookModule, nativeCallNames, sizeof(nativeCallNames) / sizeof(nativeCallNames[0]));

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
