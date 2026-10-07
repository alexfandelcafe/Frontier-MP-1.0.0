#include "core/script_bridge.hpp"
#include "core/engine_hooks.hpp"
#include "core/native_invoker.hpp"
#include <iostream>

namespace Frontier::Core {

namespace {

using ScriptRegisterFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn = void (*)(DWORD);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptWaitFn s_scriptWait = nullptr;

void resolveScriptHook() {
    HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    s_scriptRegister = reinterpret_cast<ScriptRegisterFn>(
        GetProcAddress(hookModule, "scriptRegister"));
    s_scriptWait = reinterpret_cast<ScriptWaitFn>(
        GetProcAddress(hookModule, "scriptWait"));
}

} // namespace

void ScriptBridge::registerScript(HMODULE module) {
    resolveScriptHook();

    if (!s_scriptRegister || !s_scriptWait) {
        if (!s_warnedUnavailable.exchange(true, std::memory_order_acq_rel)) {
            std::cout << "[ScriptBridge] No se encontraron scriptRegister/scriptWait en ScriptHookRDR.dll." << std::endl;
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
