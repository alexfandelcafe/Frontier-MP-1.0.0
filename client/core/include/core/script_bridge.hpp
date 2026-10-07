#pragma once

#include <windows.h>
#include <cstdint>
#include <atomic>

namespace Frontier::Core {

class ScriptBridge {
public:
    // Kept for DLL lifecycle compatibility. Registration is deferred until
    // ScriptHookRDR has installed its rage::scrThread::Run hook.
    static void registerScriptEarly(HMODULE module);

    // Resolves ScriptHook exports and attempts registration from the client
    // worker thread. Safe to call repeatedly until registration succeeds.
    static void initialize(HMODULE module);
    static void tryRegister(HMODULE module);

    // Services the ScriptHook-backed registration and Run interceptor.
    static void update(HMODULE module);

    static bool isRegistered();

private:
    static void __cdecl scriptMain();
    static void runFrame();

    static bool installRunInterceptor(HMODULE hookModule);
    static uint64_t hookedScriptHookRun(uintptr_t scriptThread, uintptr_t param2);
    static void pumpRegisteredScript(uintptr_t hookModuleBase);

    static inline std::atomic<bool> s_registered{false};
    static inline std::atomic<bool> s_warnedUnavailable{false};
    static inline std::atomic<bool> s_registrationRequested{false};
    static inline std::atomic<bool> s_schedulerReadyLogged{false};
    static inline std::atomic<bool> s_runInterceptorInstalled{false};
    static inline std::atomic<bool> s_runInterceptorFailureLogged{false};
    static inline std::atomic<bool> s_fiberPreparationFailureLogged{false};
    static inline std::atomic<bool> s_scriptStartFailureLogged{false};
};

} // namespace Frontier::Core
