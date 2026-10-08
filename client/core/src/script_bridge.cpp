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
using ScriptUnregisterFn = void (*)(HMODULE);

ScriptRegisterFn s_scriptRegisterAdditionalThread = nullptr;
ScriptRegisterFn s_scriptRegisterFallback = nullptr;
ScriptWaitFn s_scriptWait = nullptr;
ScriptUnregisterFn s_scriptUnregister = nullptr;

// 0 = todavía no registrado, 1 = probando scriptRegister, 2 = probando
// scriptRegisterAdditionalThread. Cambiamos de ruta solo después de que la
// anterior haya tenido tiempo de ser consumida por el scheduler de ScriptHook.
static std::atomic<uint32_t> s_registrationMode{0};

template <typename T>
T resolveExport(HMODULE module, const char* name) {
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

    const auto& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || !directory.Size) return nullptr;

    const auto* exports =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
            reinterpret_cast<const uint8_t*>(module) +
            directory.VirtualAddress);
    const auto* names =
        reinterpret_cast<const DWORD*>(
            reinterpret_cast<const uint8_t*>(module) +
            exports->AddressOfNames);

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

void resolveScriptHook() {
    HMODULE hookModule =
        GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) return;

    s_scriptRegisterAdditionalThread =
        resolveExport<ScriptRegisterFn>(
            hookModule,
            "?scriptRegisterAdditionalThread@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    if (!s_scriptRegisterAdditionalThread) {
        s_scriptRegisterAdditionalThread =
            resolveMangledExport<ScriptRegisterFn>(
                hookModule, "scriptRegisterAdditionalThread");
    }

    s_scriptRegisterFallback =
        resolveExport<ScriptRegisterFn>(
            hookModule,
            "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");

    if (!s_scriptRegisterFallback) {
        s_scriptRegisterFallback =
            resolveMangledExport<ScriptRegisterFn>(
                hookModule, "scriptRegister");
    }

    s_scriptWait =
        resolveExport<ScriptWaitFn>(
            hookModule, "?scriptWait@@YAXK@Z");

    if (!s_scriptWait) {
        s_scriptWait =
            resolveMangledExport<ScriptWaitFn>(
                hookModule, "scriptWait");
    }

    s_scriptUnregister =
        resolveExport<ScriptUnregisterFn>(
            hookModule,
            "?scriptUnregister@@YAXPEAUHINSTANCE__@@@Z");

    if (!s_scriptUnregister) {
        s_scriptUnregister =
            resolveMangledExport<ScriptUnregisterFn>(
                hookModule, "scriptUnregister");
    }

    NativeInvoker::initialize();

    std::cout << "[ScriptBridge] APIs de registro: scriptRegister="
              << reinterpret_cast<void*>(s_scriptRegisterFallback)
              << " scriptRegisterAdditionalThread="
              << reinterpret_cast<void*>(s_scriptRegisterAdditionalThread)
              << " scriptWait="
              << reinterpret_cast<void*>(s_scriptWait)
              << std::endl;
}

} // namespace

void ScriptBridge::registerScript(HMODULE module) {
    if (!module ||
        s_registered.load(std::memory_order_acquire)) {
        return;
    }

    resolveScriptHook();

    if (!s_scriptWait ||
        (!s_scriptRegisterAdditionalThread &&
         !s_scriptRegisterFallback)) {

        if (!s_warnedUnavailable.exchange(
                true, std::memory_order_acq_rel)) {
            std::cerr
                << "[ScriptBridge] ScriptHookRDR no expone la API "
                   "necesaria para crear el script Frontier."
                << std::endl;
        }

        return;
    }

    // ScriptHook puede tardar en iniciar su scheduler. Reintentar una solicitud
    // que no comenzó es seguro y evita el bloqueo que vimos con scriptRegister.
    const uint64_t now = GetTickCount64();
    const uint64_t last =
        s_lastRegistrationTick.load(std::memory_order_acquire);

    if (s_registrationInFlight.load(std::memory_order_acquire)) {
        if (now - last < 3000) return;

        const uint32_t previousMode =
            s_registrationMode.load(std::memory_order_acquire);

        if (s_scriptUnregister) {
            s_scriptUnregister(module);
            std::cout
                << "[ScriptBridge] Registro anterior retirado antes de "
                   "cambiar de ruta."
                << std::endl;
        }

        // La ruta normal es la API soportada por las versiones modernas de
        // ScriptHook. Solo usamos la API adicional como fallback real si el
        // callback registrado por scriptRegister no arrancó.
        if (previousMode == 1 && s_scriptRegisterAdditionalThread) {
            s_registrationMode.store(2, std::memory_order_release);
        } else if (s_scriptRegisterFallback) {
            s_registrationMode.store(1, std::memory_order_release);
        } else {
            s_registrationMode.store(2, std::memory_order_release);
        }

        s_registrationInFlight.store(false, std::memory_order_release);
    }

    s_lastRegistrationTick.store(
        now, std::memory_order_release);
    s_registrationInFlight.store(
        true, std::memory_order_release);

    uint32_t mode = s_registrationMode.load(std::memory_order_acquire);
    if (mode == 0) {
        // Primer intento: scriptRegister. Es la ruta compatible con el
        // ScriptHook actual y con plugins inyectados después del arranque.
        mode = s_scriptRegisterFallback ? 1u : 2u;
        s_registrationMode.store(mode, std::memory_order_release);
    }

    if (mode == 1 && s_scriptRegisterFallback) {
        s_scriptRegisterFallback(
            module, &ScriptBridge::scriptMain);

        std::cout
            << "[ScriptBridge] Registro solicitado mediante scriptRegister."
            << std::endl;
    } else if (mode == 2 && s_scriptRegisterAdditionalThread) {
        s_scriptRegisterAdditionalThread(
            module, &ScriptBridge::scriptMain);

        std::cout
            << "[ScriptBridge] Registro solicitado mediante "
               "scriptRegisterAdditionalThread (fallback)."
            << std::endl;
    } else {
        s_registrationInFlight.store(false, std::memory_order_release);
        std::cerr
            << "[ScriptBridge] No hay una ruta de registro válida disponible."
            << std::endl;
    }
}

bool ScriptBridge::isRegistered() {
    return s_registered.load(std::memory_order_acquire);
}

void __cdecl ScriptBridge::scriptMain() {
    s_registered.store(true, std::memory_order_release);
    s_registrationInFlight.store(false, std::memory_order_release);

    std::cout
        << "[ScriptBridge] ScriptMain iniciado dentro del scheduler de RAGE."
        << std::endl;

    for (;;) {
        runFrame();

        if (!s_scriptWait) return;
        s_scriptWait(0);
    }
}

void ScriptBridge::runFrame() {
    if (!NativeInvoker::isReady()) {
        NativeInvoker::initialize();
    }

    if (!NativeInvoker::isReady()) return;

    EngineHooks::processMultiplayerWorldLoad();
}

} // namespace Frontier::Core
