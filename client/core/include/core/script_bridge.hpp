#pragma once

#include <windows.h>
#include <cstdint>
#include <atomic>

namespace Frontier::Core {

class ScriptBridge {
public:
    // Must be called from DLL_PROCESS_ATTACH, after the launcher has already
    // loaded ScriptHookRDR. This mirrors the ScriptHook SDK lifecycle.
    static void registerScriptEarly(HMODULE module);
    // Resolves ScriptHook exports and attempts registration from the client
    // worker thread. Safe to call repeatedly until registration succeeds.
    static void initialize(HMODULE module);
    static void tryRegister(HMODULE module);
    static bool isRegistered();

private:
    static void __cdecl scriptMain();
    static void runFrame();

    static inline std::atomic<bool> s_registered{false};
    static inline std::atomic<bool> s_warnedUnavailable{false};
};

} // namespace Frontier::Core
