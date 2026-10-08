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
using ScriptUnregisterFn = void (*)(HMODULE);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptWaitFn s_scriptWait = nullptr;
ScriptUnregisterFn s_scriptUnregister = nullptr;

std::atomic<uint64_t> s_lastRetryTick{0};
constexpr uint64_t kRegistrationRetryMs = 1500;

ScriptRegisterFn resolveScriptRegisterAdditionalThread(HMODULE hookModule) {
    if (!hookModule) {
        return nullptr;
    }

    // Public RDR1 ScriptHook SDK entry point for an independently scheduled
    // script thread. This avoids depending on the primary script queue.
    const FARPROC proc = GetProcAddress(
        hookModule,
        "?scriptRegisterAdditionalThread@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

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

ScriptUnregisterFn resolveScriptUnregister(HMODULE hookModule) {
    if (!hookModule) {
        return nullptr;
    }

    const FARPROC proc = GetProcAddress(
        hookModule,
        "?scriptUnregister@@YAXPEAUHINSTANCE__@@@Z");

    return proc
        ? reinterpret_cast<ScriptUnregisterFn>(proc)
        : nullptr;
}

void resolveRuntimeApi() {
    const HMODULE hookModule =
        GetModuleHandleA("ScriptHookRDR.dll");

    if (!hookModule) {
        return;
    }

    s_scriptRegister = resolveScriptRegisterAdditionalThread(hookModule);
    s_scriptWait = resolveScriptWait(hookModule);
    s_scriptUnregister = resolveScriptUnregister(hookModule);
}

} // namespace

void ScriptBridge::registerScriptAtAttach(HMODULE module) {
    if (!module ||
        s_registered.load(std::memory_order_acquire)) {
        return;
    }

    const HMODULE hookModule =
        GetModuleHandleA("ScriptHookRDR.dll");

    if (!hookModule) {
        OutputDebugStringA(
            "[FrontierClient] ScriptHookRDR no estaba cargado durante DLL attach.\n");
        return;
    }

    // Keep DLL_PROCESS_ATTACH deliberately minimal. Register Frontier on
    // ScriptHook's public additional-script queue, which has its own
    // scheduler and does not require private ScriptHook internals.
    if (!s_scriptRegister) {
        s_scriptRegister =
            resolveScriptRegisterAdditionalThread(hookModule);
    }

    if (!s_scriptRegister) {
        OutputDebugStringA(
            "[FrontierClient] scriptRegisterAdditionalThread no pudo resolverse durante DLL attach.\n");
        return;
    }

    s_scriptRegister(
        module,
        &ScriptBridge::scriptMain);

    s_registrationRequested.store(
        true,
        std::memory_order_release);

    s_lastRetryTick.store(
        GetTickCount64(),
        std::memory_order_release);

    OutputDebugStringA(
        "[FrontierClient] scriptRegisterAdditionalThread llamado durante DLL_PROCESS_ATTACH.\n");
}

void ScriptBridge::registerScript(HMODULE module) {
    if (!module ||
        s_registered.load(std::memory_order_acquire)) {
        return;
    }

    resolveRuntimeApi();

    if (!s_scriptRegister || !s_scriptWait) {
        if (!s_warnedUnavailable.exchange(
                true,
                std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] ScriptHookRDR todavía no expone "
                   "scriptRegisterAdditionalThread/scriptWait."
                << std::endl;
        }
        return;
    }

    const uint64_t now = GetTickCount64();
    const uint64_t last =
        s_lastRetryTick.load(std::memory_order_acquire);

    if (last != 0 && now - last < kRegistrationRetryMs) {
        return;
    }

    // If the attach-time attempt happened before ScriptHook's scheduler was
    // ready, repeat the same public additional-thread registration call. No private scheduler
    // internals are touched, and retries stop immediately once ScriptMain runs.
    s_scriptRegister(module, &ScriptBridge::scriptMain);

    s_registrationRequested.store(
        true,
        std::memory_order_release);

    s_lastRetryTick.store(
        now,
        std::memory_order_release);

    std::cout
        << "[ScriptBridge] scriptRegisterAdditionalThread enviado/reintentado desde el worker; "
           "esperando ScriptMain."
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
    s_registrationRequested.store(
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
