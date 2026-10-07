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
#include <fstream>
#include <filesystem>

namespace Frontier::Core {

namespace {

using ScriptRegisterFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn = void (*)(DWORD);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptWaitFn s_scriptWait = nullptr;
std::atomic<bool> s_exportsDumped{false};
std::atomic<bool> s_registrationRequested{false};
std::atomic<bool> s_registrationDelayLogged{false};
std::atomic<bool> s_scriptHookReadyLogged{false};
std::atomic<uintmax_t> s_scriptHookLogBaselineSize{0};
std::atomic<bool> s_scriptHookLogBaselineCaptured{false};

bool isScriptHookInitializationComplete() {
    char gamePath[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, gamePath, MAX_PATH)) {
        return false;
    }

    std::filesystem::path logPath(gamePath);
    logPath = logPath.parent_path() / "ScriptHookRDR.log";

    std::error_code ec;
    if (!std::filesystem::exists(logPath, ec)) {
        return false;
    }

    const uintmax_t currentSize = std::filesystem::file_size(logPath, ec);
    if (ec) {
        return false;
    }

    if (!s_scriptHookLogBaselineCaptured.load(std::memory_order_acquire)) {
        return false;
    }

    const uintmax_t baseline =
        s_scriptHookLogBaselineSize.load(std::memory_order_acquire);

    // ScriptHook rewrites/appends this log during the current startup. Do not
    // accept stale "Finished hooking functions" text from an older run.
    if (currentSize <= baseline) {
        return false;
    }

    std::ifstream log(logPath, std::ios::binary);
    if (!log.is_open()) {
        return false;
    }

    std::string contents(
        (std::istreambuf_iterator<char>(log)),
        std::istreambuf_iterator<char>());

    const size_t initPos =
        contents.rfind("[INIT] Initializing ScriptHook for Red Dead Redemption");
    const size_t finishPos =
        contents.rfind("[INIT] Finished hooking functions");

    if (initPos == std::string::npos ||
        finishPos == std::string::npos ||
        finishPos <= initPos) {
        return false;
    }

    // The current initialization sequence must extend beyond the baseline.
    return finishPos >= baseline;
}

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
        // If ScriptHook was loaded by the host before Frontier, there is no
        // earlier baseline to protect against stale log contents. In that
        // case, the presence of the completed current initialization is enough.
        if (!s_scriptHookLogBaselineCaptured.load(std::memory_order_acquire)) {
            s_scriptHookLogBaselineSize.store(0, std::memory_order_release);
            s_scriptHookLogBaselineCaptured.store(true, std::memory_order_release);
        }
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
                char gamePath[MAX_PATH] = {};
                if (GetModuleFileNameA(nullptr, gamePath, MAX_PATH)) {
                    std::filesystem::path logPath(gamePath);
                    logPath = logPath.parent_path() / "ScriptHookRDR.log";
                    std::error_code ec;
                    const uintmax_t baseline =
                        std::filesystem::exists(logPath, ec)
                            ? std::filesystem::file_size(logPath, ec)
                            : 0;
                    s_scriptHookLogBaselineSize.store(
                        ec ? 0 : baseline,
                        std::memory_order_release);
                    s_scriptHookLogBaselineCaptured.store(
                        true,
                        std::memory_order_release);
                }
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

    dumpRelevantExports(hookModule);

    s_scriptRegister = getExportByExactName<ScriptRegisterFn>(
        hookModule,
        "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    if (!s_scriptRegister) {
        s_scriptRegister =
            resolveMangledExport<ScriptRegisterFn>(
                hookModule,
                "scriptRegister");
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

void ScriptBridge::registerScript(HMODULE module) {
    resolveScriptHook(module);

    if (!s_scriptRegister || !s_scriptWait) {
        if (!s_warnedUnavailable.exchange(
                true,
                std::memory_order_acq_rel)) {
            std::cout
                << "[ScriptBridge] ScriptHookRDR cargado pero no se pudieron resolver "
                   "scriptRegister/scriptWait."
                << std::endl;
        }
        return;
    }

    if (s_registered.load(std::memory_order_acquire) ||
        s_registrationRequested.load(std::memory_order_acquire)) {
        return;
    }

    if (!isScriptHookInitializationComplete()) {
        if (!s_registrationDelayLogged.exchange(
                true,
                std::memory_order_acq_rel)) {
            std::cout
                << "[ScriptBridge] ScriptHookRDR cargado; esperando "
                   "\"[INIT] Finished hooking functions\" antes de registrar FrontierMP."
                << std::endl;
        }
        return;
    }

    if (!s_scriptHookReadyLogged.exchange(
            true,
            std::memory_order_acq_rel)) {
        std::cout
            << "[ScriptBridge] ScriptHookRDR terminó su inicialización de hooks; "
               "registrando FrontierMP ahora."
            << std::endl;
    }

    s_registrationRequested.store(true, std::memory_order_release);

    s_scriptRegister(
        module,
        &ScriptBridge::scriptMain);

    std::cout
        << "[ScriptBridge] Registro de FrontierMP enviado mediante scriptRegister "
           "después de la inicialización de ScriptHookRDR."
        << std::endl;
}

bool ScriptBridge::isRegistered() {
    return s_registered.load(std::memory_order_acquire);
}

void __cdecl ScriptBridge::scriptMain() {
    s_registered.store(true, std::memory_order_release);

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
