#pragma once

#include <windows.h>
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
};

} // namespace Frontier::Core
