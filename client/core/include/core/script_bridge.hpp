#pragma once

#include <windows.h>
#include <cstdint>
#include <atomic>

namespace Frontier::Core {

class ScriptBridge {
public:
    // Registers Frontier through the public ScriptHookRDR API from DLL attach
    // when ScriptHookRDR has already been mapped by the launcher.
    static void registerScriptEarly(HMODULE module);

    // Resolves ScriptHookRDR exports. Safe to call repeatedly.
    static void initialize(HMODULE module);
    static void tryRegister(HMODULE module);
    static void update(HMODULE module);

    static bool isRegistered();

private:
    static void __cdecl scriptMain();
    static void runFrame();

    // ScriptHookRDR 1.5.2 keeps script registration public but starts the
    // registered fiber from its internal Script ID dispatcher. We hook only
    // the ScriptHook detour entry so we can invoke that existing dispatcher
    // on the real RAGE script thread after ScriptHook has assigned an ID.
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
};

} // namespace Frontier::Core
