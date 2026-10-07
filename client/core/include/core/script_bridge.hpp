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

    // Advances the ScriptHook-backed script bridge. This also contains the
    // 1.5.2-specific fallback that manually enters the registered script fiber
    // when the game never reaches rage::scrThread::Run (for example, while the
    // title/frontend has no active RAGE script thread yet).
    static void update(HMODULE module);

    static bool isRegistered();

private:
    static void __cdecl scriptMain();
    static void runFrame();
    static void pumpRegisteredScript(HMODULE hookModule);
    static bool prepareFallbackScript(HMODULE hookModule);

    static inline std::atomic<bool> s_registered{false};
    static inline std::atomic<bool> s_warnedUnavailable{false};
    static inline std::atomic<bool> s_registrationRequested{false};
    static inline std::atomic<bool> s_schedulerReadyLogged{false};
    static inline std::atomic<bool> s_fallbackActive{false};
    static inline std::atomic<bool> s_fallbackPreparedLogged{false};
    static inline std::atomic<uint32_t> s_fallbackPollTicks{0};
    static inline std::atomic<uint32_t> s_fallbackScriptId{0};
};

} // namespace Frontier::Core
