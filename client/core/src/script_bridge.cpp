#include "core/script_bridge.hpp"
#include "core/engine_hooks.hpp"
#include "core/native_invoker.hpp"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

namespace Frontier::Core {

namespace {

using ScriptRegisterFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn = void (*)(DWORD);
using ScriptManagerMaintenanceFn = void (*)();
using ScriptStartByIdFn = void (*)(uint32_t);
using ScriptHookRunDetourFn = uint64_t (*)(uintptr_t, uintptr_t);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptWaitFn s_scriptWait = nullptr;
ScriptManagerMaintenanceFn s_scriptManagerMaintenance = nullptr;
ScriptStartByIdFn s_scriptStartById = nullptr;
ScriptHookRunDetourFn s_originalScriptHookRunDetour = nullptr;
std::atomic<bool> s_scriptPreparationLogged{false};

// ScriptHookRDR 1.5.2 internal scheduler entry points.
// These RVAs come from the supplied 1.5.2 binary decompilation:
//   FUN_180023540 -> ScriptHook's rage::scrThread::Run detour.
//   FUN_180031970 -> ScriptManager::prepare/start registered script fibers.
//   FUN_180031a20 -> enter a registered script fiber by Script ID.
//   DAT_18020e0a0 -> ScriptManager object.
constexpr uintptr_t kScriptHookRunDetourRva = 0x23540;
constexpr uintptr_t kScriptManagerRva = 0x20e0a0;
constexpr uintptr_t kScriptManagerMaintenanceRva = 0x31970;
constexpr uintptr_t kScriptStartByIdRva = 0x31a20;

template <typename T>
T getExportByExactName(HMODULE module, const char* name) {
    if (!module || !name) {
        return nullptr;
    }

    const FARPROC proc = GetProcAddress(module, name);
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

        const FARPROC proc = GetProcAddress(module, exportedName);
        if (proc) {
            return reinterpret_cast<T>(proc);
        }
    }

    return nullptr;
}

template <typename T>
bool readScriptHookValue(HMODULE module, uintptr_t rva, T& value) {
    if (!module) {
        return false;
    }

    const auto address =
        reinterpret_cast<const uint8_t*>(module) + rva;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(
            reinterpret_cast<const void*>(address),
            &mbi,
            sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }

    if (mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_GUARD) ||
        (mbi.Protect & 0xff) == PAGE_NOACCESS) {
        return false;
    }

    __try {
        value = *reinterpret_cast<const T*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return true;
}

bool readScriptHookGlobal(HMODULE module, uintptr_t rva, uintptr_t& value) {
    return readScriptHookValue(module, rva, value) && value != 0;
}

struct RegisteredScriptState {
    size_t recordCount{0};
    uint32_t scriptId{0};
    uintptr_t scriptFiber{0};
};

bool readRegisteredScriptState(HMODULE hookModule, RegisteredScriptState& state) {
    if (!hookModule) {
        return false;
    }

    const auto base = reinterpret_cast<uintptr_t>(hookModule);
    const auto manager = base + kScriptManagerRva;

    uintptr_t begin = 0;
    uintptr_t end = 0;

    if (!readScriptHookValue(
            hookModule,
            kScriptManagerRva + 0x50,
            begin) ||
        !readScriptHookValue(
            hookModule,
            kScriptManagerRva + 0x58,
            end)) {
        return false;
    }

    if (!begin || !end || end < begin || ((end - begin) % sizeof(uintptr_t)) != 0) {
        return false;
    }

    const size_t count =
        static_cast<size_t>((end - begin) / sizeof(uintptr_t));

    // Frontier registers exactly one script. Refuse to walk an obviously
    // corrupt/unexpected vector so the diagnostics cannot dereference garbage.
    if (count > 64) {
        return false;
    }

    state.recordCount = count;
    if (count == 0) {
        return true;
    }

    uintptr_t record = 0;
    __try {
        record = *reinterpret_cast<const uintptr_t*>(begin);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (!record) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(
            reinterpret_cast<const void*>(record),
            &mbi,
            sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_GUARD) ||
        (mbi.Protect & 0xff) == PAGE_NOACCESS) {
        return false;
    }

    __try {
        state.scriptId = *reinterpret_cast<const uint32_t*>(record + 0x8c);
        state.scriptFiber = *reinterpret_cast<const uintptr_t*>(record + 0x68);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        state.scriptId = 0;
        state.scriptFiber = 0;
        return false;
    }

    (void)manager;
    return true;
}

bool isScriptHookRunSchedulerReady(HMODULE hookModule) {
    // ScriptHookRDR 1.5.2 decompilation:
    //   DAT_18020e4a8 -> resolved rage::scrThread::Run target
    //   DAT_18020e3f0 -> original Run pointer saved by hook manager
    // Their RVAs are fixed in this 1.5.2 binary.
    uintptr_t runTarget = 0;
    uintptr_t originalRun = 0;

    return readScriptHookGlobal(hookModule, 0x20e4a8, runTarget) &&
           readScriptHookGlobal(hookModule, 0x20e3f0, originalRun);
}

void resolveScriptHook(HMODULE frontierModule) {
    (void)frontierModule;

    // The launcher owns ScriptHookRDR loading. Frontier only observes an
    // already-loaded module and binds to its SDK/native exports.
    const HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

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

    static std::atomic<bool> nativeApiBound{false};
    if (nativeInit && nativePush64 && nativeCall &&
        !nativeApiBound.exchange(true, std::memory_order_acq_rel)) {
        NativeInvoker::setScriptHookApi(
            nativeInit,
            nativePush64,
            nativeCall);
    }
}

} // namespace

void ScriptBridge::registerScriptEarly(HMODULE module) {
    // Never enter ScriptHookRDR from DllMain. Its own worker initializes
    // asynchronously, and MinHook is initialized by EngineHooks.
    (void)module;
}

bool ScriptBridge::installRunInterceptor(HMODULE hookModule) {
    if (!hookModule) {
        return false;
    }

    if (s_runInterceptorInstalled.load(std::memory_order_acquire)) {
        return true;
    }

    const auto target = reinterpret_cast<LPVOID>(
        reinterpret_cast<uintptr_t>(hookModule) + kScriptHookRunDetourRva);

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(target, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_GUARD) ||
        (mbi.Protect & 0xff) == PAGE_NOACCESS) {
        return false;
    }

    const MH_STATUS createStatus = MH_CreateHook(
        target,
        reinterpret_cast<LPVOID>(&ScriptBridge::hookedScriptHookRun),
        reinterpret_cast<LPVOID*>(&s_originalScriptHookRunDetour));

    if (createStatus != MH_OK &&
        createStatus != MH_ERROR_ALREADY_CREATED) {
        if (!s_runInterceptorFailureLogged.exchange(
                true, std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] No se pudo interceptar FUN_180023540 "
                   "(rage::scrThread::Run) de ScriptHookRDR. MH_STATUS="
                << static_cast<int>(createStatus)
                << std::endl;
        }
        return false;
    }

    // With our own target this is normally MH_OK. If MinHook reports that
    // the hook already exists, the saved trampoline must still be usable.
    if (!s_originalScriptHookRunDetour) {
        if (!s_runInterceptorFailureLogged.exchange(
                true, std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] FUN_180023540 ya tenía un hook y no "
                   "se obtuvo su trampoline."
                << std::endl;
        }
        return false;
    }

    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK &&
        enableStatus != MH_ERROR_ENABLED) {
        if (!s_runInterceptorFailureLogged.exchange(
                true, std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] No se pudo activar el interceptor de "
                   "rage::scrThread::Run. MH_STATUS="
                << static_cast<int>(enableStatus)
                << std::endl;
        }
        return false;
    }

    s_runInterceptorInstalled.store(true, std::memory_order_release);

    std::cout
        << "[ScriptBridge] Interceptor ScriptHookRDR 1.5.2 instalado sobre "
           "FUN_180023540 (rage::scrThread::Run)."
        << std::endl;

    return true;
}

uint64_t ScriptBridge::hookedScriptHookRun(
    uintptr_t scriptThread,
    uintptr_t param2) {

    const uint64_t result =
        s_originalScriptHookRunDetour
        ? s_originalScriptHookRunDetour(scriptThread, param2)
        : 0;

    // The callback runs on the actual RAGE script thread, after ScriptHook's
    // own Run detour has finished. This is the safe context for FiberData,
    // ScriptHook TLS and nativeInit/nativePush64/nativeCall.
    if (!s_registrationRequested.load(std::memory_order_acquire) ||
        s_registered.load(std::memory_order_acquire)) {
        return result;
    }

    const HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (hookModule) {
        pumpRegisteredScript(
            reinterpret_cast<uintptr_t>(hookModule));
    }

    return result;
}

void ScriptBridge::pumpRegisteredScript(uintptr_t hookModuleBase) {
    if (!hookModuleBase) {
        return;
    }

    if (!s_scriptManagerMaintenance) {
        s_scriptManagerMaintenance =
            reinterpret_cast<ScriptManagerMaintenanceFn>(
                hookModuleBase + kScriptManagerMaintenanceRva);
    }

    if (!s_scriptStartById) {
        s_scriptStartById =
            reinterpret_cast<ScriptStartByIdFn>(
                hookModuleBase + kScriptStartByIdRva);
    }

    const HMODULE hookModule =
        reinterpret_cast<HMODULE>(hookModuleBase);

    RegisteredScriptState state{};
    if (!readRegisteredScriptState(hookModule, state) ||
        state.recordCount == 0) {
        return;
    }

    // ScriptHookRDR's first Run can happen before Frontier registers. In that
    // case DAT_18020e3f8 is already set and the normal Run state machine may
    // skip the manager's fiber creation path. Re-run FUN_180031970 here on the
    // correct RAGE thread until Frontier has both a Script ID and fiber.
    for (int pass = 0;
         pass < 3 &&
         (state.scriptId == 0 || state.scriptFiber == 0);
         ++pass) {

        if (!s_scriptManagerMaintenance) {
            return;
        }

        __try {
            s_scriptManagerMaintenance();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (!s_fiberPreparationFailureLogged.exchange(
                    true, std::memory_order_acq_rel)) {
                std::cerr
                    << "[ScriptBridge] Excepción al preparar el fiber "
                       "Frontier mediante ScriptHookRDR 1.5.2."
                    << std::endl;
            }
            return;
        }

        if (!readRegisteredScriptState(hookModule, state)) {
            return;
        }
    }

    if (state.scriptId == 0 || state.scriptFiber == 0) {
        return;
    }

    if (!s_scriptPreparationLogged.exchange(
            true, std::memory_order_acq_rel)) {
        std::cout
            << "[ScriptBridge] ScriptHookRDR preparó el script Frontier: "
               "ScriptId="
            << state.scriptId
            << " fiber=0x" << std::hex << state.scriptFiber
            << std::dec
            << ". Iniciando desde el contexto de rage::scrThread::Run."
            << std::endl;
    }

    if (!s_scriptStartById) {
        return;
    }

    __try {
        s_scriptStartById(state.scriptId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!s_scriptStartFailureLogged.exchange(
                true, std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] Excepción al entrar en el fiber Frontier "
                   "mediante FUN_180031a20."
                << std::endl;
        }
    }
}

void ScriptBridge::update(HMODULE module) {
    if (!module) {
        return;
    }

    resolveScriptHook(module);

    const HMODULE hookModule =
        GetModuleHandleA("ScriptHookRDR.dll");

    if (!hookModule) {
        return;
    }

    // EngineHooks initializes MinHook before the bridge runs. Waiting for
    // ScriptHook's own Run hook prevents us from intercepting an incomplete
    // module.
    if (!isScriptHookRunSchedulerReady(hookModule)) {
        tryRegister(module);
        return;
    }

    installRunInterceptor(hookModule);
    tryRegister(module);
}

void ScriptBridge::tryRegister(HMODULE module) {
    if (!module ||
        s_registered.load(std::memory_order_acquire) ||
        s_registrationRequested.load(std::memory_order_acquire)) {
        return;
    }

    const HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    resolveScriptHook(module);

    if (!s_scriptRegister) {
        return;
    }

    if (!isScriptHookRunSchedulerReady(hookModule)) {
        static std::atomic<bool> schedulerWaitLogged{false};
        if (!schedulerWaitLogged.exchange(
                true, std::memory_order_acq_rel)) {
            std::cout
                << "[ScriptBridge] ScriptHookRDR está cargado; esperando que "
                   "termine de instalar rage::scrThread::Run antes de registrar."
                << std::endl;
        }
        return;
    }

    if (!s_schedulerReadyLogged.exchange(
            true, std::memory_order_acq_rel)) {
        uintptr_t runTarget = 0;
        uintptr_t originalRun = 0;
        readScriptHookGlobal(hookModule, 0x20e4a8, runTarget);
        readScriptHookGlobal(hookModule, 0x20e3f0, originalRun);

        std::cout
            << "[ScriptBridge] Scheduler ScriptHookRDR listo: Run target=0x"
            << std::hex << runTarget
            << " original=0x" << originalRun
            << std::dec << std::endl;
    }

    // Install our interceptor before registering the script. If RDR reaches
    // Run immediately after this call, we must already be in the path that
    // can recover a late registration.
    if (!installRunInterceptor(hookModule)) {
        return;
    }

    if (s_registrationRequested.exchange(
            true, std::memory_order_acq_rel)) {
        return;
    }

    s_scriptRegister(module, &ScriptBridge::scriptMain);

    std::cout
        << "[ScriptBridge] scriptRegister enviado después de instalar el "
           "interceptor de rage::scrThread::Run."
        << std::endl;
}

void ScriptBridge::initialize(HMODULE module) {
    resolveScriptHook(module);
    tryRegister(module);

    const HMODULE hookModule =
        GetModuleHandleA("ScriptHookRDR.dll");

    if (!hookModule) {
        return;
    }

    if (!s_scriptWait) {
        if (!s_warnedUnavailable.exchange(
                true, std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] ScriptHookRDR está cargado, pero scriptWait "
                   "no pudo resolverse."
                << std::endl;
        }
    }
}

bool ScriptBridge::isRegistered() {
    return s_registered.load(std::memory_order_acquire);
}

void __cdecl ScriptBridge::scriptMain() {
    bool expected = false;
    if (!s_registered.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }

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
