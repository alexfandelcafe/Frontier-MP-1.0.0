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

ScriptRegisterFn resolveScriptRegister(HMODULE hookModule) {
    if (!hookModule) {
        return nullptr;
    }

    // Exact export used by the public RDR1 ScriptHook SDK.
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

    s_scriptRegister = resolveScriptRegister(hookModule);
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

    // Keep DLL_PROCESS_ATTACH deliberately minimal and match the public
    // RDR1 example project's registration mechanism exactly.
    const ScriptRegisterFn registerFn =
        resolveScriptRegister(hookModule);

    if (!registerFn) {
        OutputDebugStringA(
            "[FrontierClient] scriptRegister no pudo resolverse durante DLL attach.\n");
        return;
    }

    registerFn(
        module,
        &ScriptBridge::scriptMain);

    s_registrationRequested.store(
        true,
        std::memory_order_release);

    s_lastRetryTick.store(
        GetTickCount64(),
        std::memory_order_release);

    OutputDebugStringA(
        "[FrontierClient] scriptRegister llamado durante DLL_PROCESS_ATTACH.\n");
}

void ScriptBridge::registerScript(HMODULE module) {
    if (!module ||
        s_registered.load(std::memory_order_acquire) ||
        s_registrationRequested.load(std::memory_order_acquire)) {
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

    const uint64_t now = GetTickCount64();
    const uint64_t last =
        s_lastRetryTick.load(std::memory_order_acquire);

    if (last != 0 && now - last < 3000) {
        return;
    }

    // This path only exists if DLL attach happened before ScriptHookRDR was
    // visible. It mirrors the same public API; no private ScriptHook internals
    // are touched.
    s_scriptRegister(
        module,
        &ScriptBridge::scriptMain);

    s_registrationRequested.store(
        true,
        std::memory_order_release);

    s_lastRetryTick.store(
        now,
        std::memory_order_release);

    std::cout
        << "[ScriptBridge] scriptRegister enviado desde el worker; "
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
