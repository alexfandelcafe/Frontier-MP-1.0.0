
#include "core/script_bridge.hpp"
#include "core/engine_hooks.hpp"
#include "core/native_invoker.hpp"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace Frontier::Core {

namespace {

using ScriptRegisterFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn = void (*)(DWORD);
using ScriptUnregisterFn = void (*)(HMODULE);
using ScriptStartByIdFn = void (*)(uint32_t);
using ScriptManagerMaintenanceFn = void (*)();
using ScriptHookRunDetourFn = uint64_t (*)(uintptr_t, uintptr_t);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptWaitFn s_scriptWait = nullptr;
ScriptUnregisterFn s_scriptUnregister = nullptr;
ScriptStartByIdFn s_scriptStartById = nullptr;
ScriptManagerMaintenanceFn s_scriptManagerMaintenance = nullptr;
ScriptHookRunDetourFn s_originalScriptHookRunDetour = nullptr;

// RVAs verified against the bundled ScriptHookRDR 1.5.2 binary.
constexpr uintptr_t kScriptHookRunDetourRva = 0x23540;
constexpr uintptr_t kScriptManagerRecordsBeginRva = 0x20e0f0;
constexpr uintptr_t kScriptManagerRecordsEndRva = 0x20e0f8;
constexpr uintptr_t kScriptManagerMaintenanceRva = 0x31970;
constexpr uintptr_t kScriptStartByIdRva = 0x31a20;
constexpr uintptr_t kScriptHookRunTargetRva = 0x20e4a8;
constexpr uintptr_t kScriptHookRunOriginalRva = 0x20e3f0;

std::atomic<uint64_t> s_lastRegistrationTick{0};

template <typename T>
T getExportByExactName(HMODULE module, const char* name) {
    if (!module || !name) return nullptr;
    const FARPROC proc = GetProcAddress(module, name);
    return proc ? reinterpret_cast<T>(proc) : nullptr;
}

template <typename T>
T resolveMangledExport(HMODULE module, const char* token) {
    if (!module || !token || !*token) return nullptr;

    const auto* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const auto& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return nullptr;

    const auto* exports =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
            reinterpret_cast<const uint8_t*>(module) + dir.VirtualAddress);
    const auto* names =
        reinterpret_cast<const DWORD*>(
            reinterpret_cast<const uint8_t*>(module) + exports->AddressOfNames);

    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const char* exportedName =
            reinterpret_cast<const char*>(
                reinterpret_cast<const uint8_t*>(module) + names[i]);

        if (!std::strstr(exportedName, token)) continue;

        const FARPROC proc = GetProcAddress(module, exportedName);
        if (proc) return reinterpret_cast<T>(proc);
    }

    return nullptr;
}

template <typename T>
bool readModuleValue(HMODULE module, uintptr_t rva, T& out) {
    if (!module) return false;

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
        out = *reinterpret_cast<const T*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return true;
}

bool schedulerLooksReady(HMODULE hookModule) {
    uintptr_t runTarget = 0;
    uintptr_t originalRun = 0;

    return readModuleValue(
               hookModule,
               kScriptHookRunTargetRva,
               runTarget) &&
           readModuleValue(
               hookModule,
               kScriptHookRunOriginalRva,
               originalRun) &&
           runTarget != 0 &&
           originalRun != 0;
}

struct RegisteredScriptState {
    uintptr_t record{0};
    uintptr_t fiber{0};
    uintptr_t callback{0};
    uint32_t id{0};
};

bool readRegisteredScriptState(
    HMODULE hookModule,
    HMODULE frontierModule,
    RegisteredScriptState& state)
{
    state = {};

    if (!hookModule || !frontierModule) return false;

    uintptr_t begin = 0;
    uintptr_t end = 0;

    if (!readModuleValue(
            hookModule,
            kScriptManagerRecordsBeginRva,
            begin) ||
        !readModuleValue(
            hookModule,
            kScriptManagerRecordsEndRva,
            end) ||
        !begin ||
        !end ||
        end <= begin) {
        return false;
    }

    const uintptr_t bytes = end - begin;
    if ((bytes % sizeof(uintptr_t)) != 0 ||
        bytes > sizeof(uintptr_t) * 64) {
        return false;
    }

    for (uintptr_t cursor = begin;
         cursor < end;
         cursor += sizeof(uintptr_t)) {

        uintptr_t candidate = 0;
        __try {
            candidate =
                *reinterpret_cast<const uintptr_t*>(cursor);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }

        if (!candidate) continue;

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(candidate),
                &mbi,
                sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & PAGE_GUARD) ||
            (mbi.Protect & 0xff) == PAGE_NOACCESS) {
            continue;
        }

        HMODULE recordModule = nullptr;
        uintptr_t callback = 0;
        uintptr_t fiber = 0;
        uint32_t id = 0;

        __try {
            recordModule =
                *reinterpret_cast<const HMODULE*>(candidate + 0x00);
            callback =
                *reinterpret_cast<const uintptr_t*>(candidate + 0x60);
            fiber =
                *reinterpret_cast<const uintptr_t*>(candidate + 0x68);
            id =
                *reinterpret_cast<const uint32_t*>(candidate + 0x8c);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }

        if (recordModule != frontierModule ||
            !callback) {
            continue;
        }

        state.record = candidate;
        state.callback = callback;
        state.fiber = fiber;
        state.id = id;
        return true;
    }

    return false;
}

void resolveScriptHook() {
    const HMODULE hookModule =
        GetModuleHandleA("ScriptHookRDR.dll");

    if (!hookModule) return;

    s_scriptRegister =
        getExportByExactName<ScriptRegisterFn>(
            hookModule,
            "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    if (!s_scriptRegister) {
        s_scriptRegister =
            resolveMangledExport<ScriptRegisterFn>(
                hookModule,
                "scriptRegister");
    }

    s_scriptWait =
        getExportByExactName<ScriptWaitFn>(
            hookModule,
            "?scriptWait@@YAXK@Z");

    if (!s_scriptWait) {
        s_scriptWait =
            resolveMangledExport<ScriptWaitFn>(
                hookModule,
                "scriptWait");
    }

    s_scriptUnregister =
        getExportByExactName<ScriptUnregisterFn>(
            hookModule,
            "?scriptUnregister@@YAXPEAUHINSTANCE__@@@Z");

    if (!s_scriptUnregister) {
        s_scriptUnregister =
            resolveMangledExport<ScriptUnregisterFn>(
                hookModule,
                "scriptUnregister");
    }

    NativeInvoker::initialize();

    std::cout
        << "[ScriptBridge] APIs ScriptHook: scriptRegister="
        << reinterpret_cast<void*>(s_scriptRegister)
        << " scriptWait="
        << reinterpret_cast<void*>(s_scriptWait)
        << " scriptUnregister="
        << reinterpret_cast<void*>(s_scriptUnregister)
        << std::endl;
}

} // namespace

void ScriptBridge::registerScriptAtAttach(HMODULE module) {
    if (!module ||
        s_registered.load(std::memory_order_acquire) ||
        s_registrationRequested.exchange(
            true,
            std::memory_order_acq_rel)) {
        return;
    }

    const HMODULE hookModule =
        GetModuleHandleA("ScriptHookRDR.dll");

    if (!hookModule) {
        s_registrationRequested.store(false, std::memory_order_release);
        OutputDebugStringA(
            "[FrontierClient] ScriptHookRDR no estaba cargado en DLL attach.\n");
        return;
    }

    resolveScriptHook();

    if (!s_scriptRegister) {
        s_registrationRequested.store(false, std::memory_order_release);
        OutputDebugStringA(
            "[FrontierClient] scriptRegister no pudo resolverse en DLL attach.\n");
        return;
    }

    s_scriptRegister(module, &ScriptBridge::scriptMain);
    s_lastRegistrationTick.store(
        GetTickCount64(),
        std::memory_order_release);

    OutputDebugStringA(
        "[FrontierClient] Frontier registrado mediante scriptRegister en DLL attach.\n");
}

void ScriptBridge::registerScript(HMODULE module) {
    if (!module ||
        s_registered.load(std::memory_order_acquire)) {
        return;
    }

    resolveScriptHook();

    const HMODULE hookModule =
        GetModuleHandleA("ScriptHookRDR.dll");

    if (!hookModule) return;

    if (!s_scriptWait || !s_scriptRegister) {
        if (!s_warnedUnavailable.exchange(
                true,
                std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] Falta scriptRegister/scriptWait en ScriptHookRDR."
                << std::endl;
        }
        return;
    }

    if (!s_runDispatchHookInstalled.load(
            std::memory_order_acquire)) {

        if (!schedulerLooksReady(hookModule)) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_acq_rel)) {
                std::cout
                    << "[ScriptBridge] Esperando scheduler interno de "
                       "ScriptHookRDR (rage::scrThread::Run)..."
                    << std::endl;
            }
            return;
        }

        if (!installRunDispatchHook(hookModule)) {
            return;
        }
    }

    const uint64_t now = GetTickCount64();
    const uint64_t last =
        s_lastRegistrationTick.load(
            std::memory_order_acquire);

    // If attach registration was not consumed by ScriptHook, allow one
    // controlled retry every 3 seconds.
    if (!s_registrationRequested.load(
            std::memory_order_acquire) ||
        (now - last >= 3000 &&
         !s_registered.load(std::memory_order_acquire))) {

        if (s_registrationRequested.load(
                std::memory_order_acquire) &&
            s_scriptUnregister) {
            s_scriptUnregister(module);
        }

        s_scriptRegister(module, &ScriptBridge::scriptMain);
        s_registrationRequested.store(
            true,
            std::memory_order_release);
        s_lastRegistrationTick.store(
            now,
            std::memory_order_release);

        std::cout
            << "[ScriptBridge] scriptRegister enviado; esperando "
               "asignación del ScriptId/Fiber."
            << std::endl;
    }

    dispatchRegisteredScript(
        reinterpret_cast<uintptr_t>(hookModule));
}

bool ScriptBridge::isRegistered() {
    return s_registered.load(std::memory_order_acquire);
}

bool ScriptBridge::installRunDispatchHook(HMODULE hookModule) {
    if (!hookModule) return false;

    if (s_runDispatchHookInstalled.load(
            std::memory_order_acquire)) {
        return true;
    }

    const auto target =
        reinterpret_cast<LPVOID>(
            reinterpret_cast<uintptr_t>(hookModule) +
            kScriptHookRunDetourRva);

    const MH_STATUS createStatus =
        MH_CreateHook(
            target,
            reinterpret_cast<LPVOID>(
                &ScriptBridge::hookedScriptHookRun),
            reinterpret_cast<LPVOID*>(
                &s_originalScriptHookRunDetour));

    if (createStatus != MH_OK &&
        createStatus != MH_ERROR_ALREADY_CREATED) {

        if (!s_runDispatchFailureLogged.exchange(
                true,
                std::memory_order_acq_rel)) {

            std::cerr
                << "[ScriptBridge] No se pudo instalar dispatcher de "
                   "rage::scrThread::Run. MH_STATUS="
                << static_cast<int>(createStatus)
                << " target=0x"
                << std::hex
                << reinterpret_cast<uintptr_t>(target)
                << std::dec
                << std::endl;
        }

        return false;
    }

    if (!s_originalScriptHookRunDetour) {
        if (!s_runDispatchFailureLogged.exchange(
                true,
                std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] Dispatcher de Run instalado previamente "
                   "pero no hay trampoline disponible."
                << std::endl;
        }
        return false;
    }

    const MH_STATUS enableStatus =
        MH_EnableHook(target);

    if (enableStatus != MH_OK &&
        enableStatus != MH_ERROR_ENABLED) {

        if (!s_runDispatchFailureLogged.exchange(
                true,
                std::memory_order_acq_rel)) {

            std::cerr
                << "[ScriptBridge] No se pudo activar dispatcher de "
                   "rage::scrThread::Run. MH_STATUS="
                << static_cast<int>(enableStatus)
                << std::endl;
        }

        return false;
    }

    s_scriptManagerMaintenance =
        reinterpret_cast<ScriptManagerMaintenanceFn>(
            reinterpret_cast<uintptr_t>(hookModule) +
            kScriptManagerMaintenanceRva);

    s_scriptStartById =
        reinterpret_cast<ScriptStartByIdFn>(
            reinterpret_cast<uintptr_t>(hookModule) +
            kScriptStartByIdRva);

    s_runDispatchHookInstalled.store(
        true,
        std::memory_order_release);

    uintptr_t runTarget = 0;
    uintptr_t originalRun = 0;
    readModuleValue(
        hookModule,
        kScriptHookRunTargetRva,
        runTarget);
    readModuleValue(
        hookModule,
        kScriptHookRunOriginalRva,
        originalRun);

    if (!s_schedulerReadyLogged.exchange(
            true,
            std::memory_order_acq_rel)) {

        std::cout
            << "[ScriptBridge] Scheduler ScriptHookRDR listo: "
               "RunTarget=0x"
            << std::hex << runTarget
            << " OriginalRun=0x"
            << originalRun
            << " Dispatcher=0x"
            << reinterpret_cast<uintptr_t>(target)
            << std::dec
            << std::endl;
    }

    return true;
}

std::uint64_t __cdecl ScriptBridge::hookedScriptHookRun(
    uintptr_t scriptThread,
    uintptr_t param2)
{
    const uint64_t result =
        s_originalScriptHookRunDetour
            ? s_originalScriptHookRunDetour(
                  scriptThread,
                  param2)
            : 0;

    if (!s_registrationRequested.load(
            std::memory_order_acquire) ||
        s_registered.load(
            std::memory_order_acquire)) {
        return result;
    }

    const HMODULE hookModule =
        GetModuleHandleA("ScriptHookRDR.dll");

    if (hookModule) {
        dispatchRegisteredScript(
            reinterpret_cast<uintptr_t>(hookModule));
    }

    return result;
}

void ScriptBridge::dispatchRegisteredScript(
    uintptr_t hookModuleBase)
{
    if (!hookModuleBase ||
        s_registered.load(std::memory_order_acquire) ||
        !s_registrationRequested.load(
            std::memory_order_acquire)) {
        return;
    }

    const HMODULE hookModule =
        reinterpret_cast<HMODULE>(hookModuleBase);

    const HMODULE frontierModule =
        GetModuleHandleA("frontier_core.dll");

    if (!frontierModule) return;

    RegisteredScriptState state{};
    if (!readRegisteredScriptState(
            hookModule,
            frontierModule,
            state)) {
        return;
    }

    // ScriptHook may have a registration record before it has prepared the
    // fiber. Use its own maintenance routine on the actual RAGE script thread.
    constexpr uint32_t kUnassignedScriptId = UINT32_MAX;

    if ((state.id == 0 ||
         state.id == kUnassignedScriptId ||
         state.fiber == 0) &&
        s_scriptManagerMaintenance) {

        for (int attempt = 0;
             attempt < 3 &&
             (state.id == 0 ||
              state.id == kUnassignedScriptId ||
              state.fiber == 0);
             ++attempt) {

            __try {
                s_scriptManagerMaintenance();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                if (!s_scriptDispatchLogged.exchange(
                        true,
                        std::memory_order_acq_rel)) {
                    std::cerr
                        << "[ScriptBridge] Excepción preparando el "
                           "fiber Frontier con ScriptHookRDR."
                        << std::endl;
                }
                return;
            }

            if (!readRegisteredScriptState(
                    hookModule,
                    frontierModule,
                    state)) {
                return;
            }
        }
    }

    if (state.id == 0 ||
        state.id == kUnassignedScriptId ||
        state.fiber == 0) {
        return;
    }

    if (!s_scriptDispatchLogged.exchange(
            true,
            std::memory_order_acq_rel)) {

        std::cout
            << "[ScriptBridge] Fiber Frontier listo: record=0x"
            << std::hex << state.record
            << " ScriptId=0x" << state.id
            << " fiber=0x" << state.fiber
            << " callback=0x" << state.callback
            << std::dec
            << ". Entrando mediante ScriptHookRDR."
            << std::endl;
    }

    if (!s_scriptStartById) return;

    __try {
        s_scriptStartById(state.id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!s_runDispatchFailureLogged.exchange(
                true,
                std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] Excepción iniciando el fiber Frontier "
                   "mediante ScriptStartById. code=0x"
                << std::hex << GetExceptionCode()
                << std::dec
                << std::endl;
        }
    }
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

    s_registrationRequested.store(
        true,
        std::memory_order_release);

    std::cout
        << "[ScriptBridge] ScriptMain iniciado dentro del scheduler de RAGE."
        << std::endl;

    for (;;) {
        __try {
            runFrame();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::cerr
                << "[ScriptBridge] Excepción en runFrame. code=0x"
                << std::hex << GetExceptionCode()
                << std::dec
                << std::endl;
            return;
        }

        if (!s_scriptWait) return;

        __try {
            s_scriptWait(0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::cerr
                << "[ScriptBridge] Excepción en scriptWait. code=0x"
                << std::hex << GetExceptionCode()
                << std::dec
                << std::endl;
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
