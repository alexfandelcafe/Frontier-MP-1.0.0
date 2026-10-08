#include "core/script_bridge.hpp"
#include "core/engine_hooks.hpp"
#include "core/native_invoker.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace Frontier::Core {

namespace {

using ScriptRegisterFn = void (*)(HMODULE, void (*)());
using ScriptWaitFn = void (*)(DWORD);

ScriptRegisterFn s_scriptRegister = nullptr;
ScriptWaitFn s_scriptWait = nullptr;

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

size_t readScriptHookVectorCount(HMODULE hookModule, uintptr_t rva) {
    if (!hookModule) {
        return 0;
    }

    uintptr_t begin = 0;
    uintptr_t end = 0;

    if (!readScriptHookValue(
            hookModule, rva, begin) ||
        !readScriptHookValue(
            hookModule, rva + sizeof(uintptr_t), end) ||
        !begin || !end || end < begin) {
        return 0;
    }

    const uintptr_t bytes = end - begin;
    if ((bytes % sizeof(uintptr_t)) != 0 ||
        bytes > (sizeof(uintptr_t) * 1024)) {
        return 0;
    }

    return static_cast<size_t>(
        bytes / sizeof(uintptr_t));
}

bool readScriptRecordPointer(
    HMODULE hookModule,
    uintptr_t vectorRva,
    uintptr_t& record)
{
    record = 0;
    if (!hookModule) {
        return false;
    }

    uintptr_t begin = 0;
    uintptr_t end = 0;

    if (!readScriptHookValue(hookModule, vectorRva, begin) ||
        !readScriptHookValue(
            hookModule, vectorRva + sizeof(uintptr_t), end) ||
        !begin || end <= begin ||
        (end - begin) < sizeof(uintptr_t)) {
        return false;
    }

    __try {
        record = *reinterpret_cast<const uintptr_t*>(begin);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        record = 0;
        return false;
    }

    return record != 0;
}

void logRegisteredScriptRecord(
    HMODULE hookModule,
    HMODULE frontierModule)
{
    uintptr_t record = 0;
    if (!readScriptRecordPointer(
            hookModule, 0x20e108, record)) {
        std::cout
            << "[ScriptBridge] No se pudo leer el primer registro de "
               "ScriptHookRDR."
            << std::endl;
        return;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(
            reinterpret_cast<const void*>(record),
            &mbi,
            sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_GUARD) ||
        (mbi.Protect & 0xff) == PAGE_NOACCESS) {
        std::cout
            << "[ScriptBridge] El primer registro de ScriptHookRDR no apunta "
               "a memoria legible."
            << std::endl;
        return;
    }

    const uintptr_t frontierBase =
        reinterpret_cast<uintptr_t>(frontierModule);
    const uintptr_t scriptMain =
        reinterpret_cast<uintptr_t>(&ScriptBridge::scriptMain);

    uintptr_t moduleField = 0;
    uintptr_t fiberField = 0;
    uint32_t scriptId = 0;

    __try {
        // Known fields from the 1.5.2 record layout observed in the
        // decompilation: +0x68 = fiber, +0x8c = ScriptId.
        fiberField =
            *reinterpret_cast<const uintptr_t*>(record + 0x68);
        scriptId =
            *reinterpret_cast<const uint32_t*>(record + 0x8c);

        // Look for the Frontier module handle and exact ScriptMain pointer
        // without assuming their field offsets.
        for (uintptr_t off = 0;
             off <= 0xB8;
             off += sizeof(uintptr_t)) {
            uintptr_t value =
                *reinterpret_cast<const uintptr_t*>(record + off);

            if (value == frontierBase) {
                moduleField = off;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::cout
            << "[ScriptBridge] Excepción leyendo el registro de "
               "ScriptHookRDR; se omite el diagnóstico."
            << std::endl;
        return;
    }

    uintptr_t callbackField = 0;
    __try {
        for (uintptr_t off = 0;
             off <= 0xB8;
             off += sizeof(uintptr_t)) {
            const uintptr_t value =
                *reinterpret_cast<const uintptr_t*>(record + off);

            if (value == scriptMain) {
                callbackField = off;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        callbackField = 0;
    }

    std::cout
        << "[ScriptBridge] Registro ScriptHookRDR: record=0x"
        << std::hex << record
        << " moduleFieldOffset="
        << moduleField
        << " scriptMainFieldOffset="
        << callbackField
        << " fiber=0x"
        << fiberField
        << " scriptId=0x"
        << scriptId
        << std::dec
        << std::endl;
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

void ScriptBridge::update(HMODULE module) {
    if (!module) {
        return;
    }

    static std::atomic<bool> registrationStateLogged{false};
    if (s_registrationRequested.load(std::memory_order_acquire) &&
        !s_registered.load(std::memory_order_acquire) &&
        !registrationStateLogged.exchange(true, std::memory_order_acq_rel)) {

        const HMODULE hookModule =
            GetModuleHandleA("ScriptHookRDR.dll");

        const size_t pendingRegistrations =
            readScriptHookVectorCount(hookModule, 0x20e108);

        const size_t scriptStacks =
            readScriptHookVectorCount(hookModule, 0x20e0f0);

        std::cout
            << "[ScriptBridge] Diagnóstico post-scriptRegister: "
               "registrationRequested=1, "
               "pendingRegistrationEntries="
            << pendingRegistrations
            << ", scriptStackEntries="
            << scriptStacks
            << ", scriptMainStarted=0."
            << std::endl;
        logRegisteredScriptRecord(
            hookModule, module);
    }

    // Normally registration succeeds during initialize(). This remains a
    // cheap retry path in case ScriptHookRDR was not yet mapped when the
    // Frontier worker first started.
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
