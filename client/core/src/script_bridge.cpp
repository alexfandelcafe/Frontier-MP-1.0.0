#include "core/script_bridge.hpp"
#include "core/engine_hooks.hpp"
#include "core/native_invoker.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <iostream>

namespace Frontier::Core {

namespace {

using ScriptRegisterFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn = void (*)(DWORD);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptWaitFn s_scriptWait = nullptr;

std::atomic<bool> s_registrationIssued{false};

ScriptRegisterFn resolveScriptRegister(HMODULE hookModule) {
    if (!hookModule) {
        return nullptr;
    }

    // Public RDR1 ScriptHook SDK registration entry point.
    const FARPROC proc = GetProcAddress(
        hookModule,
        "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    return proc
        ? reinterpret_cast<ScriptRegisterFn>(proc)
        : nullptr;
}

ScriptWaitFn resolveScriptWait(HMODULE hookModule) {
    if (!hookModule) {
        return nullptr;
    }

    const FARPROC proc = GetProcAddress(
        hookModule,
        "?scriptWait@@YAXK@Z");

    return proc
        ? reinterpret_cast<ScriptWaitFn>(proc)
        : nullptr;
}

void resolveRuntimeApi() {
    const HMODULE hookModule =
        GetModuleHandleA("ScriptHookRDR.dll");

    if (!hookModule) {
        return;
    }

    s_scriptRegister = resolveScriptRegister(hookModule);
    s_scriptWait = resolveScriptWait(hookModule);
}

} // namespace

void ScriptBridge::registerScript(HMODULE module) {
    if (!module ||
        s_registered.load(std::memory_order_acquire) ||
        s_registrationIssued.load(std::memory_order_acquire)) {
        return;
    }

    // The launcher intentionally keeps RDR's main thread suspended while both
    // DLLs are injected. Registering against ScriptHook before the game has
    // entered its render/script lifecycle is too early. FrontierMainThread
    // calls this only after EngineHooks reports a live RDR swap chain.
    if (!EngineHooks::isGameRenderReady()) {
        return;
    }

    resolveRuntimeApi();

    if (!s_scriptRegister || !s_scriptWait) {
        if (!s_warnedUnavailable.exchange(
                true,
                std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] ScriptHookRDR todavía no expone "
                   "scriptRegister/scriptWait."
                << std::endl;
        }
        return;
    }

    bool expected = false;
    if (!s_registrationIssued.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel)) {
        return;
    }

    // ScriptHook's public SDK expects a module/script pair to be registered
    // during DLL_PROCESS_ATTACH. The launcher loads ScriptHookRDR before this
    // DLL, so this call is made at the supported lifecycle point.
    s_scriptRegister(
        module,
        &ScriptBridge::scriptMain);

    std::cout
        << "[ScriptBridge] scriptRegister registrado; "
           "ScriptMain queda en manos del scheduler de ScriptHookRDR."
        << std::endl;
}

bool ScriptBridge::isRegistered() {
    return s_registered.load(
        std::memory_order_acquire);
}

void __cdecl ScriptBridge::scriptMain() {
    s_registered.store(
        true,
        std::memory_order_release);
    s_registrationIssued.store(
        false,
        std::memory_order_release);
    resolveRuntimeApi();

    // NativeInvoker is initialized only after ScriptHook has entered the
    // actual ScriptMain fiber. This keeps all native calls inside ScriptHook's
    // supported script context.
    NativeInvoker::initialize();

    std::cout
        << "[ScriptBridge] ScriptMain iniciado dentro del scheduler de RAGE."
        << std::endl;

    for (;;) {
        runFrame();

        if (!s_scriptWait) {
            resolveRuntimeApi();
        }

        if (!s_scriptWait) {
            std::cerr
                << "[ScriptBridge] scriptWait no está disponible; "
                   "se detiene el script Frontier."
                << std::endl;
            return;
        }

        s_scriptWait(0);
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
