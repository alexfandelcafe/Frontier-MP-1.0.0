#pragma once

#include <windows.h>
#include <cstdint>
#include <atomic>

namespace Frontier::Core {

class ScriptBridge {
public:
    static void registerScriptAtAttach(HMODULE module);
    static void registerScript(HMODULE module);
    static bool isRegistered();

private:
    static void __cdecl scriptMain();
    static void runFrame();

    static inline std::atomic<bool> s_registered{false};
    static inline std::atomic<bool> s_warnedUnavailable{false};
    static inline std::atomic<bool> s_registrationRequested{false};
    static inline std::atomic<bool> s_runDispatchHookInstalled{false};
    static inline std::atomic<bool> s_runDispatchFailureLogged{false};
    static inline std::atomic<bool> s_scriptDispatchLogged{false};
    static inline std::atomic<bool> s_schedulerReadyLogged{false};
};

} // namespace Frontier::Core
