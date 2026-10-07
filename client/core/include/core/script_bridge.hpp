#pragma once

#include <windows.h>
#include <cstdint>
#include <atomic>

namespace Frontier::Core {

class ScriptBridge {
public:
    static void registerScript(HMODULE module);
    static bool isRegistered();

private:
    static void __cdecl scriptMain();
    static void runFrame();

    static inline std::atomic<bool> s_registered{false};
    static inline std::atomic<bool> s_warnedUnavailable{false};
    static inline std::atomic<bool> s_registrationInFlight{false};
    static inline std::atomic<uint64_t> s_lastRegistrationTick{0};
};

} // namespace Frontier::Core
