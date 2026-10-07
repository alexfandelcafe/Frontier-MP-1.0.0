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

namespace Frontier::Core {

namespace {

using ScriptRegisterFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn = void (*)(DWORD);
using ScriptManagerMaintenanceFn = void (*)();
using ScriptStartByIdFn = void (*)(uint32_t);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptWaitFn s_scriptWait = nullptr;

// ScriptHookRDR 1.5.2 internal scheduler entry points.
// These RVAs come from the supplied 1.5.2 binary decompilation:
//   FUN_180031970 -> ScriptManager::prepare/start registered script fibers
//   FUN_180031a20 -> enter a registered script fiber by Script ID
constexpr uintptr_t kScriptManagerRva = 0x20e0a0;
constexpr uintptr_t kRunEnteredFlagRva = 0x20e3f8;
constexpr uintptr_t kScriptManagerMaintenanceRva = 0x31f970;
constexpr uintptr_t kScriptStartByIdRva = 0x31fa20;

ScriptManagerMaintenanceFn s_scriptManagerMaintenance = nullptr;
ScriptStartByIdFn s_scriptStartById = nullptr;

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

bool hasScriptHookRunEntered(HMODULE hookModule) {
    uint8_t entered = 0;
    return readScriptHookValue(hookModule, kRunEnteredFlagRva, entered) &&
           entered != 0;
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

bool initializeInternalSchedulerApi(HMODULE hookModule) {
    if (!hookModule) {
        return false;
    }

    if (!s_scriptManagerMaintenance) {
        s_scriptManagerMaintenance =
            reinterpret_cast<ScriptManagerMaintenanceFn>(
                reinterpret_cast<uintptr_t>(hookModule) +
                kScriptManagerMaintenanceRva);
    }

    if (!s_scriptStartById) {
        s_scriptStartById =
            reinterpret_cast<ScriptStartByIdFn>(
                reinterpret_cast<uintptr_t>(hookModule) +
                kScriptStartByIdRva);
    }

    return s_scriptManagerMaintenance && s_scriptStartById;
}

bool callScriptManagerMaintenance() {
    if (!s_scriptManagerMaintenance) {
        return false;
    }

    __try {
        s_scriptManagerMaintenance();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool callScriptStartById(uint32_t scriptId) {
    if (!s_scriptStartById || scriptId == 0) {
        return false;
    }

    __try {
        s_scriptStartById(scriptId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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
    // Registration is deliberately deferred. ScriptHookRDR starts a worker
    // from its own DLL_PROCESS_ATTACH and installs its RAGE scheduler hooks
    // asynchronously, so registering before Run is hooked can leave the
    // request stranded until the next loader event.
    (void)module;
}

void ScriptBridge::update(HMODULE module) {
    if (!module) {
        return;
    }

    tryRegister(module);

    const HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule || !s_registrationRequested.load(std::memory_order_acquire)) {
        return;
    }

    // Once ScriptMain has started through the normal ScriptHook path, do not
    // drive the same fiber from Frontier.
    if (!s_fallbackActive.load(std::memory_order_acquire)) {
        if (s_registered.load(std::memory_order_acquire)) {
            return;
        }

        const uint32_t ticks =
            s_fallbackPollTicks.fetch_add(1, std::memory_order_acq_rel) + 1;

        // Give the real rage::scrThread::Run hook time to encounter a RAGE
        // script thread. The fallback is only for the frontend/title path
        // where Run may not fire at all.
        if (ticks < 120) {
            return;
        }

        if (hasScriptHookRunEntered(hookModule)) {
            if (!s_fallbackPreparedLogged.exchange(
                    true,
                    std::memory_order_acq_rel)) {
                std::cout
                    << "[ScriptBridge] rage::scrThread::Run ya comenzó a "
                       "ejecutarse; se desactiva el fallback de scheduler."
                    << std::endl;
            }
            return;
        }

        if (prepareFallbackScript(hookModule)) {
            s_fallbackActive.store(true, std::memory_order_release);
        }
    }

    if (s_fallbackActive.load(std::memory_order_acquire)) {
        if (hasScriptHookRunEntered(hookModule)) {
            s_fallbackActive.store(false, std::memory_order_release);
            std::cout
                << "[ScriptBridge] ScriptHookRDR comenzó a ejecutar Run; "
                   "cediendo el fiber al scheduler normal."
                << std::endl;
            return;
        }

        pumpRegisteredScript(hookModule);
    }
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
                true,
                std::memory_order_acq_rel)) {
            std::cout
                << "[ScriptBridge] ScriptHookRDR está cargado; esperando que "
                   "termine de instalar rage::scrThread::Run antes de registrar."
                << std::endl;
        }
        return;
    }

    if (!s_schedulerReadyLogged.exchange(
            true,
            std::memory_order_acq_rel)) {
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

    if (s_registrationRequested.exchange(
            true,
            std::memory_order_acq_rel)) {
        return;
    }

    s_scriptRegister(module, &ScriptBridge::scriptMain);

    std::cout
        << "[ScriptBridge] scriptRegister enviado después de que ScriptHookRDR "
           "instaló rage::scrThread::Run."
        << std::endl;
}

void ScriptBridge::initialize(HMODULE module) {
    resolveScriptHook(module);
    tryRegister(module);

    if (!GetModuleHandleA("ScriptHookRDR.dll")) {
        return;
    }

    if (!s_scriptWait) {
        if (!s_warnedUnavailable.exchange(
                true,
                std::memory_order_acq_rel)) {
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
            expected,
            true,
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

bool ScriptBridge::prepareFallbackScript(HMODULE hookModule) {
    if (!initializeInternalSchedulerApi(hookModule)) {
        return false;
    }

    RegisteredScriptState state{};
    if (!readRegisteredScriptState(hookModule, state)) {
        return false;
    }

    if (state.recordCount == 0) {
        static std::atomic<bool> noRecordLogged{false};
        if (!noRecordLogged.exchange(true, std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] Fallback: ScriptHookRDR no muestra ningún "
                   "registro en ScriptManager después de scriptRegister."
                << std::endl;
        }
        return false;
    }

    // FUN_180031970 assigns the Script ID on its first pass and creates the
    // actual fiber on the next pass. Calling it only as needed mirrors the
    // state machine in ScriptHookRDR 1.5.2.
    for (int pass = 0; pass < 3; ++pass) {
        if (!readRegisteredScriptState(hookModule, state)) {
            return false;
        }

        if (state.scriptId != 0 && state.scriptFiber != 0) {
            break;
        }

        if (!callScriptManagerMaintenance()) {
            std::cerr
                << "[ScriptBridge] Fallback: FUN_180031970 falló durante la "
                   "preparación del script fiber."
                << std::endl;
            return false;
        }
    }

    if (!readRegisteredScriptState(hookModule, state) ||
        state.scriptId == 0 ||
        state.scriptFiber == 0) {
        static std::atomic<bool> fiberNotReadyLogged{false};
        if (!fiberNotReadyLogged.exchange(
                true,
                std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] Fallback: registro encontrado, pero "
                   "ScriptHookRDR todavía no creó su fiber."
                << std::endl;
        }
        return false;
    }

    s_fallbackScriptId.store(state.scriptId, std::memory_order_release);

    if (!s_fallbackPreparedLogged.exchange(
            true,
            std::memory_order_acq_rel)) {
        std::cout
            << "[ScriptBridge] Fallback de scheduler preparado: ScriptId="
            << state.scriptId
            << " fiber=0x" << std::hex << state.scriptFiber
            << std::dec
            << ". Se usará la entrada interna de ScriptHookRDR 1.5.2."
            << std::endl;
    }

    return true;
}

void ScriptBridge::pumpRegisteredScript(HMODULE hookModule) {
    (void)hookModule;

    const uint32_t scriptId =
        s_fallbackScriptId.load(std::memory_order_acquire);
    if (!scriptId || !s_scriptWait) {
        return;
    }

    if (!callScriptStartById(scriptId) &&
        !s_registered.load(std::memory_order_acquire)) {
        static std::atomic<bool> pumpFailureLogged{false};
        if (!pumpFailureLogged.exchange(true, std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] Fallback: no se pudo entrar al fiber "
                   "registrado mediante FUN_180031a20."
                << std::endl;
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
