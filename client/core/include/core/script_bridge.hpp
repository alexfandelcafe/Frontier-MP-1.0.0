#pragma once

#include <windows.h>
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

    static inline std::atomic<bool> s_registered{false};
    static inline std::atomic<bool> s_warnedUnavailable{false};
    static inline std::atomic<bool> s_registrationRequested{false};

};

} // namespace Frontier::Core
