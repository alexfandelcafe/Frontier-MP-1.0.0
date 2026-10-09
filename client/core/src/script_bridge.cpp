#include "core/script_bridge.hpp"
#include "core/engine_hooks.hpp"
#include "core/native_invoker.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <iterator>

namespace Frontier::Core {

namespace {

using ScriptRegisterFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn = void (*)(DWORD);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptWaitFn s_scriptWait = nullptr;

std::atomic<bool> s_registrationIssued{false};

void appendScriptBootLog(const char* message);

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

std::filesystem::path getGameDirectory() {
    char exePath[MAX_PATH] = {};
    if (!GetModuleFileNameA(GetModuleHandleA(nullptr), exePath, MAX_PATH)) {
        return {};
    }
    return std::filesystem::path(exePath).parent_path();
}

std::string readSmallTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

bool containsAsiLoaderMarker(const std::string& log) {
    return log.find("Mod loader by kepmehz") != std::string::npos &&
           log.find("self loading ScriptHookRDR.dll") != std::string::npos;
}

bool detectExternalAsiLoader(
    const std::filesystem::path& gameDirectory,
    const std::string& existingLog) {
    if (gameDirectory.empty() ||
        !std::filesystem::is_regular_file(
            gameDirectory / "ScriptHookRDR.dll")) {
        return false;
    }

    if (containsAsiLoaderMarker(existingLog)) {
        return true;
    }

    const char* loaderNames[] = {
        "dinput8.dll", "version.dll", "winmm.dll",
        "dsound.dll", "xinput1_3.dll", "xinput9_1_0.dll"
    };

    for (const char* name : loaderNames) {
        if (std::filesystem::is_regular_file(gameDirectory / name)) {
            return true;
        }
    }
    return false;
}

bool loaderReportedReady(
    const std::string& currentLog,
    const std::string& baseline) {
    if (currentLog.empty() || currentLog == baseline) {
        return false;
    }

    std::string thisRun;
    if (!baseline.empty() &&
        currentLog.size() >= baseline.size() &&
        currentLog.compare(0, baseline.size(), baseline) == 0) {
        thisRun = currentLog.substr(baseline.size());
    } else {
        // The loader may truncate/recreate its log at process startup.
        thisRun = currentLog;
    }

    const std::string initMarker =
        "[INIT] Initializing ScriptHook for Red Dead Redemption";
    const size_t initPos = thisRun.rfind(initMarker);
    if (initPos == std::string::npos) {
        return false;
    }

    return thisRun.find("Finished hooking functions", initPos) !=
           std::string::npos;
}

bool prepareScriptHookReadiness() {
    static bool initialized = false;
    static bool externalLoader = false;
    static std::filesystem::path logPath;
    static std::string logBaseline;

    if (!initialized) {
        const auto gameDirectory = getGameDirectory();
        logPath = gameDirectory / "asiloader.log";
        logBaseline = readSmallTextFile(logPath);
        externalLoader = detectExternalAsiLoader(
            gameDirectory,
            logBaseline);
        initialized = true;

        if (externalLoader) {
            appendScriptBootLog(
                "Detected external ASI loader; waiting for a fresh ScriptHook ready marker.");
            OutputDebugStringA(
                "[ScriptBridge] ASI Loader externo detectado; esperando "
                "'Finished hooking functions'.\n");
        }
    }

    if (!externalLoader) {
        // Standalone mode: ScriptHook was explicitly injected by the launcher
        // before the Frontier core, so its public exports are already resident.
        return true;
    }

    const std::string currentLog = readSmallTextFile(logPath);
    if (loaderReportedReady(currentLog, logBaseline)) {
        return true;
    }

    // Avoid waiting forever if the loader redirects its log. The log supplied
    // by the user shows the hook scan takes about 20 seconds; 45 seconds is a
    // conservative fallback while still keeping graphics hooks installed early.
    static const ULONGLONG firstAttempt = GetTickCount64();
    if (GetTickCount64() - firstAttempt >= 45000) {
        appendScriptBootLog(
            "WARN: no fresh asiloader ready marker after 45 seconds; permitting ScriptMain registration.");
        OutputDebugStringA(
            "[ScriptBridge] ADVERTENCIA: log de readiness no detectado tras "
            "45 s; se permitirá el registro.\n");
        return true;
    }

    return false;
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

    if (!prepareScriptHookReadiness()) {
        return;
    }

    // For the external loader path, do not call a partially initialized
    // ScriptHook scheduler. For standalone mode the private copy was loaded
    // before Frontier and can queue the script before RDR resumes.
    if (!EngineHooks::isGameRenderReady() &&
        GetModuleHandleA("ScriptHookRDR.dll") == nullptr) {
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
