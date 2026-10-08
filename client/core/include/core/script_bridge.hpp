#pragma once

#include <windows.h>
#include <cstdint>
#include <atomic>

namespace Frontier::Core {

class ScriptBridge {
public:
    // Public ScriptHook registration entry. Kept for DLL lifecycle
    // compatibility; the worker performs the scheduler fallback as needed.
    static void registerScriptAtAttach(HMODULE module);
    static void registerScript(HMODULE module);
    static bool isRegistered();

private:
    static void __cdecl scriptMain();
    static void runFrame();

    // ScriptHookRDR 1.5.2 scheduler bridge. ScriptHook exposes registration
    // publicly, but its registered script fiber is dispatched from these
    // internal scheduler entry points. The bridge hooks the ScriptHook Run
    // detour and asks that existing scheduler to start Frontier's fiber.
    static bool installRunDispatchHook(HMODULE hookModule);
    static std::uint64_t __cdecl hookedScriptHookRun(
        uintptr_t scriptThread,
        uintptr_t param2);
    static void dispatchRegisteredScript(uintptr_t hookModuleBase);

    static inline std::atomic<bool> s_registered{false};
    static inline std::atomic<bool> s_warnedUnavailable{false};
    static inline std::atomic<bool> s_registrationRequested{false};
    static inline std::atomic<bool> s_runDispatchHookInstalled{false};
    static inline std::atomic<bool> s_runDispatchFailureLogged{false};
    static inline std::atomic<bool> s_scriptDispatchLogged{false};
    static inline std::atomic<bool> s_schedulerReadyLogged{false};
};

} // namespace Frontier::Core
