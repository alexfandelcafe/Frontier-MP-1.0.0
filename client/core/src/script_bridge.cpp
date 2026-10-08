#include "core/script_bridge.hpp"
#include "core/engine_hooks.hpp"
#include "core/native_invoker.hpp"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace Frontier::Core {

namespace {

using ScriptRegisterFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn = void (*)(DWORD);
using ScriptStartByIdFn = void (*)(uint32_t);
using ScriptHookMaintenanceFn = void (*)();
using ScriptHookRunDetourFn = uint64_t (*)(uintptr_t, uintptr_t);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptWaitFn s_scriptWait = nullptr;
ScriptStartByIdFn s_scriptStartById = nullptr;
ScriptHookRunDetourFn s_originalScriptHookRunDetour = nullptr;

constexpr uintptr_t kScriptHookRunDetourRva = 0x23540;
constexpr uintptr_t kScriptManagerRecordsBeginRva = 0x20e0f0;
constexpr uintptr_t kScriptManagerRecordsEndRva = 0x20e0f8;
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


bool readFrontierScriptRecord(
    HMODULE hookModule,
    HMODULE frontierModule,
    uintptr_t& record,
    uint32_t& scriptId,
    uintptr_t& fiber,
    uintptr_t& callbackObject)
{
    record = 0;
    scriptId = 0;
    fiber = 0;
    callbackObject = 0;

    if (!hookModule || !frontierModule) {
        return false;
    }

    uintptr_t begin = 0;
    uintptr_t end = 0;

    if (!readScriptHookValue(
            hookModule, kScriptManagerRecordsBeginRva, begin) ||
        !readScriptHookValue(
            hookModule, kScriptManagerRecordsEndRva, end) ||
        !begin || !end || end <= begin) {
        return false;
    }

    const uintptr_t bytes = end - begin;
    if ((bytes % sizeof(uintptr_t)) != 0 ||
        bytes > sizeof(uintptr_t) * 64) {
        return false;
    }

    const uintptr_t frontierBase =
        reinterpret_cast<uintptr_t>(frontierModule);
    const uintptr_t scriptMain =
        reinterpret_cast<uintptr_t>(&ScriptBridge::scriptMain);

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

        if (!candidate) {
            continue;
        }

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
        uintptr_t recordCallback = 0;
        uintptr_t recordFiber = 0;
        uint32_t recordId = 0;

        __try {
            recordModule =
                *reinterpret_cast<HMODULE*>(candidate + 0x00);
            recordCallback =
                *reinterpret_cast<const uintptr_t*>(candidate + 0x60);
            recordFiber =
                *reinterpret_cast<const uintptr_t*>(candidate + 0x68);
            recordId =
                *reinterpret_cast<const uint32_t*>(candidate + 0x8c);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }

        bool moduleMatch =
            reinterpret_cast<uintptr_t>(recordModule) == frontierBase;
        bool callbackMatch = recordCallback == scriptMain;

        // The decompilation gives +0x00/+0x60 for these fields, but use a
        // value-based scan as a fallback so a minor structure-layout mismatch
        // cannot hide the registered Frontier record from the dispatcher.
        if (!moduleMatch || !callbackMatch) {
            __try {
                for (uintptr_t off = 0;
                     off <= 0xB8;
                     off += sizeof(uintptr_t)) {

                    const uintptr_t value =
                        *reinterpret_cast<const uintptr_t*>(candidate + off);

                    if (value == frontierBase) {
                        moduleMatch = true;
                    }
                    if (value == scriptMain) {
                        callbackMatch = true;
                        if (!recordCallback) {
                            recordCallback = value;
                        }
                    }

                    if (moduleMatch && callbackMatch) {
                        break;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // Keep the direct fields read above. A partially readable
                // record is still useful for diagnostics.
            }
        }

        if (!moduleMatch || !callbackMatch) {
            continue;
        }

        record = candidate;
        scriptId = recordId;
        fiber = recordFiber;
        callbackObject = recordCallback;
        return true;
    }

    return false;
}

void resolveScriptHook(HMODULE frontierModule) {
    (void)frontierModule;

    // The launcher loads ScriptHookRDR.dll before Frontier. We intentionally
    // bind only to its public SDK/native exports and leave its internal
    // rage::scrThread::Run/Reset/fiber lifecycle untouched.
    const HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    s_scriptRegister = getExportByExactName<ScriptRegisterFn>(
        hookModule,
        "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    if (!s_scriptRegister) {
        s_scriptRegister = resolveMangledExport<ScriptRegisterFn>(
            hookModule,
            "scriptRegister");
    }

    s_scriptWait = getExportByExactName<ScriptWaitFn>(
        hookModule,
        "?scriptWait@@YAXK@Z");

    if (!s_scriptWait) {
        s_scriptWait = resolveMangledExport<ScriptWaitFn>(
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
    if (!module ||
        s_registered.load(std::memory_order_acquire) ||
        s_registrationRequested.load(std::memory_order_acquire)) {
        return;
    }

    // The launcher deliberately maps ScriptHookRDR.dll before frontier_core.dll.
    // Register from DLL_PROCESS_ATTACH as the SDK expects, before ScriptHook's
    // worker has a chance to perform its initial script scan.
    const HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    ScriptRegisterFn registerFn =
        getExportByExactName<ScriptRegisterFn>(
            hookModule,
            "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    if (!registerFn) {
        registerFn =
            resolveMangledExport<ScriptRegisterFn>(
                hookModule,
                "scriptRegister");
    }

    if (!registerFn) {
        return;
    }

    if (s_registrationRequested.exchange(
            true, std::memory_order_acq_rel)) {
        return;
    }

    registerFn(module, &ScriptBridge::scriptMain);

    OutputDebugStringA(
        "[ScriptBridge] scriptRegister enviado desde DLL_PROCESS_ATTACH.\n");
}

void ScriptBridge::tryRegister(HMODULE module) {
    if (!module ||
        s_registered.load(std::memory_order_acquire) ||
        s_registrationRequested.load(std::memory_order_acquire)) {
        return;
    }

    // Fallback only when early registration was impossible (for example,
    // ScriptHookRDR was not visible yet). No internal ScriptHook functions
    // or scheduler hooks are touched here.
    const HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        return;
    }

    resolveScriptHook(module);
    if (!s_scriptRegister) {
        return;
    }

    if (s_registrationRequested.exchange(
            true, std::memory_order_acq_rel)) {
        return;
    }

    std::cout
        << "[ScriptBridge] Registrando Frontier mediante scriptRegister "
           "desde el worker (fallback)."
        << std::endl;

    s_scriptRegister(module, &ScriptBridge::scriptMain);

    std::cout
        << "[ScriptBridge] scriptRegister enviado; ScriptHookRDR manejará "
           "el Run/Reset/fiber lifecycle."
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
                << "[ScriptBridge] ScriptHookRDR está cargado, pero "
                   "scriptWait no pudo resolverse."
                << std::endl;
        }
    }
}

bool ScriptBridge::installRunDispatchHook(HMODULE hookModule) {
    if (!hookModule) {
        return false;
    }

    if (s_runDispatchHookInstalled.load(std::memory_order_acquire)) {
        return true;
    }

    const auto target = reinterpret_cast<LPVOID>(
        reinterpret_cast<uintptr_t>(hookModule) +
        kScriptHookRunDetourRva);

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(
            target, &mbi, sizeof(mbi)) != sizeof(mbi) ||
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
        if (!s_runDispatchFailureLogged.exchange(
                true, std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] No se pudo instalar el dispatcher "
                   "sobre rage::scrThread::Run de ScriptHookRDR. MH_STATUS="
                << static_cast<int>(createStatus)
                << std::endl;
        }
        return false;
    }

    if (!s_originalScriptHookRunDetour) {
        if (!s_runDispatchFailureLogged.exchange(
                true, std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] ScriptHookRDR::Run ya tenía un hook y "
                   "no se obtuvo un trampoline válido."
                << std::endl;
        }
        return false;
    }

    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK &&
        enableStatus != MH_ERROR_ENABLED) {
        if (!s_runDispatchFailureLogged.exchange(
                true, std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] No se pudo activar el dispatcher de "
                   "rage::scrThread::Run. MH_STATUS="
                << static_cast<int>(enableStatus)
                << std::endl;
        }
        return false;
    }

    s_scriptStartById =
        reinterpret_cast<ScriptStartByIdFn>(
            reinterpret_cast<uintptr_t>(hookModule) +
            kScriptStartByIdRva);

    s_runDispatchHookInstalled.store(
        true, std::memory_order_release);

    std::cout
        << "[ScriptBridge] Dispatcher ScriptHookRDR instalado: "
           "Run post-procesará el Script ID de Frontier."
        << std::endl;

    return true;
}

std::uint64_t __cdecl ScriptBridge::hookedScriptHookRun(
    uintptr_t scriptThread,
    uintptr_t param2)
{
    const uint64_t result =
        s_originalScriptHookRunDetour
        ? s_originalScriptHookRunDetour(scriptThread, param2)
        : 0;

    if (!s_registrationRequested.load(
            std::memory_order_acquire)) {
        return result;
    }

    // Re-entering the ScriptHook Run detour while the Frontier fiber is
    // currently executing would attempt a nested SwitchToFiber(). Keep the
    // dispatch one-at-a-time per RAGE script thread.
    thread_local bool dispatchInProgress = false;
    if (dispatchInProgress) {
        return result;
    }

    const HMODULE hookModule =
        GetModuleHandleA("ScriptHookRDR.dll");

    if (!hookModule) {
        return result;
    }

    dispatchInProgress = true;
    dispatchRegisteredScript(
        reinterpret_cast<uintptr_t>(hookModule));
    dispatchInProgress = false;

    return result;
}

void ScriptBridge::dispatchRegisteredScript(
    uintptr_t hookModuleBase)
{
    if (!hookModuleBase ||
        !s_registrationRequested.load(std::memory_order_acquire)) {
        return;
    }

    if (!s_scriptStartById) {
        s_scriptStartById =
            reinterpret_cast<ScriptStartByIdFn>(
                hookModuleBase + kScriptStartByIdRva);
    }

    const HMODULE hookModule =
        reinterpret_cast<HMODULE>(hookModuleBase);

    const HMODULE frontierModule =
        GetModuleHandleA("frontier_core.dll");

    uintptr_t record = 0;
    uint32_t scriptId = 0;
    uintptr_t fiber = 0;
    uintptr_t callbackObject = 0;

    if (!readFrontierScriptRecord(
            hookModule,
            frontierModule,
            record,
            scriptId,
            fiber,
            callbackObject)) {
        return;
    }

    // UINT32_MAX is the unassigned sentinel observed in ScriptHookRDR 1.5.2.
    constexpr uint32_t kUnassignedScriptId = UINT32_MAX;

    static std::atomic<bool> pendingStateLogged{false};
    if (!pendingStateLogged.exchange(
            true, std::memory_order_acq_rel)) {
        std::cout
            << "[ScriptBridge] Registro Frontier encontrado: record=0x"
            << std::hex << record
            << " ScriptId=0x" << scriptId
            << " fiber=0x" << fiber
            << " callback=0x" << callbackObject
            << std::dec
            << "."
            << std::endl;
    }

    // At this point the record exists, but ScriptHook may not have completed
    // its manager preparation yet. The original ScriptHook Run path normally
    // calls FUN_180031970 for this job. We invoke that same preparation routine
    // only from the post-Run script-thread context, never from the Frontier
    // worker and never from DllMain.
    if (scriptId == 0 ||
        scriptId == kUnassignedScriptId ||
        fiber == 0) {

        static ScriptHookMaintenanceFn scriptManagerPrepare = nullptr;
        if (!scriptManagerPrepare) {
            scriptManagerPrepare =
                reinterpret_cast<ScriptHookMaintenanceFn>(
                    hookModuleBase + kScriptManagerMaintenanceRva);
        }

        for (int pass = 0; pass < 3; ++pass) {
            if (!scriptManagerPrepare) {
                return;
            }

            __try {
                scriptManagerPrepare();
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                static std::atomic<bool> preparationFailureLogged{false};
                if (!preparationFailureLogged.exchange(
                        true, std::memory_order_acq_rel)) {
                    std::cerr
                        << "[ScriptBridge] Excepción preparando el fiber "
                           "Frontier mediante ScriptHookRDR::ScriptManager. "
                           "code=0x"
                        << std::hex << GetExceptionCode()
                        << std::dec << std::endl;
                }
                return;
            }

            if (!readFrontierScriptRecord(
                    hookModule,
                    frontierModule,
                    record,
                    scriptId,
                    fiber,
                    callbackObject)) {
                return;
            }

            if (scriptId != 0 &&
                scriptId != kUnassignedScriptId &&
                fiber != 0) {
                break;
            }
        }
    }

    if (scriptId == 0 ||
        scriptId == kUnassignedScriptId ||
        fiber == 0 ||
        !s_scriptStartById) {
        return;
    }

    if (!s_scriptDispatchLogged.exchange(
            true, std::memory_order_acq_rel)) {
        std::cout
            << "[ScriptBridge] ScriptHookRDR tiene listo el script "
               "Frontier: record=0x"
            << std::hex << record
            << " ScriptId=0x" << scriptId
            << " fiber=0x" << fiber
            << " callback=0x" << callbackObject
            << std::dec
            << ". Iniciando mediante FUN_180031a20."
            << std::endl;
    }

    __try {
        s_scriptStartById(scriptId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static std::atomic<bool> dispatchFailureLogged{false};
        if (!dispatchFailureLogged.exchange(
                true, std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] Excepción al iniciar el fiber "
                   "Frontier mediante FUN_180031a20. code=0x"
                << std::hex << GetExceptionCode()
                << std::dec << std::endl;
        }
    }
}

void ScriptBridge::update(HMODULE module) {
    if (!module) {
        return;
    }

    const HMODULE hookModule =
        GetModuleHandleA("ScriptHookRDR.dll");

    if (hookModule) {
        installRunDispatchHook(hookModule);
    }

    // Fallback only when early registration was impossible. No internal
    // ScriptHook function is invoked from the Frontier worker.
    tryRegister(module);
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
        << "[ScriptBridge] ScriptMain iniciado dentro del scheduler normal "
           "de ScriptHookRDR."
        << std::endl;

    std::cout
        << "[ScriptBridge] ScriptMain ejecutándose en el fiber gestionado por "
           "ScriptHookRDR."
        << std::endl;

    for (;;) {
        __try {
            runFrame();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::cerr
                << "[ScriptBridge] ACCESS VIOLATION/excepción SEH en "
                   "runFrame. code=0x"
                << std::hex << GetExceptionCode()
                << std::dec
                << ". Se aborta únicamente el script Frontier."
                << std::endl;
            return;
        }

        if (s_scriptWait) {
            __try {
                s_scriptWait(0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                std::cerr
                    << "[ScriptBridge] ACCESS VIOLATION/excepción SEH en "
                       "scriptWait. code=0x"
                    << std::hex << GetExceptionCode()
                    << std::dec
                    << ". Se aborta únicamente el script Frontier."
                    << std::endl;
                return;
            }
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
