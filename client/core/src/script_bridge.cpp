#include "core/script_bridge.hpp"
#include "core/engine_hooks.hpp"
#include "core/native_invoker.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <filesystem>

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

void appendScriptBootLog(const char* message) {
    char modulePath[MAX_PATH] = {};
    HMODULE module = GetModuleHandleA("frontier_core.dll");
    if (!module ||
        !GetModuleFileNameA(module, modulePath, MAX_PATH)) {
        return;
    }

    try {
        const std::filesystem::path logPath =
            std::filesystem::path(modulePath).parent_path() /
            "frontier_script_boot.log";

        std::ofstream log(logPath, std::ios::app);
        if (log.is_open()) {
            log << message << std::endl;
        }
    } catch (...) {
        // Boot logging must never interfere with ScriptHook execution.
    }
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

    appendScriptBootLog(
        "scriptRegister registrado. Esperando ejecución de ScriptMain.");
    OutputDebugStringA(
        "[ScriptBridge] scriptRegister registrado; ScriptMain queda en manos del scheduler de ScriptHookRDR.\n");
}

bool ScriptBridge::isRegistered() {
    return s_registered.load(
        std::memory_order_acquire);
}

void __cdecl ScriptBridge::scriptMain() {
    appendScriptBootLog("ScriptMain ENTER.");
    OutputDebugStringA(
        "[ScriptBridge] ScriptMain ENTER.\n");

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
    appendScriptBootLog(
        NativeInvoker::isReady()
            ? "NativeInvoker listo dentro de ScriptMain."
            : "NativeInvoker NO listo dentro de ScriptMain.");

    OutputDebugStringA(
        "[ScriptBridge] ScriptMain iniciado dentro del scheduler de RAGE.\n");

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
