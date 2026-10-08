#pragma once

#include <windows.h>
#include <cstdint>
#include <atomic>

namespace Frontier::Core {

class ScriptBridge {
public:
    // Kept for DLL lifecycle compatibility. Registration is performed from
    // the Frontier worker thread, never from DllMain.
    static void registerScriptEarly(HMODULE module);

    // Resolves ScriptHookRDR exports and registers Frontier with ScriptHook's
    // normal script scheduler. Safe to retry until ScriptHook is available.
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
