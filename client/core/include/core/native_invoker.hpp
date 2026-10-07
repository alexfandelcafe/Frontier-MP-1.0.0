#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <atomic>

namespace Frontier::Core {

using ScriptNativeInitFn = void (*)(uint64_t hash);
using ScriptNativePush64Fn = void (*)(uint64_t value);
using ScriptNativeCallFn = uint64_t* (*)();
class NativeInvoker {
public:
    static bool initialize();
    static void init(uintptr_t getCommandAddress);
    static bool isReady();
    static void setScriptHookApi(
        ScriptNativeInitFn nativeInit,
        ScriptNativePush64Fn nativePush64,
        ScriptNativeCallFn nativeCall);

    static void beginCall();

    template <typename T>
    static void pushArg(T value) {
        static_assert(sizeof(T) <= sizeof(uint64_t), "Argument size exceeds register limit");
        if (s_argCount >= 32) {
            return;
        }

        uint64_t val = 0;
        std::memcpy(&val, &value, sizeof(T));
        s_args[s_argCount++] = val;
    }

    static void endCall(uint32_t hash);

    template <typename T>
    static T getReturn() {
        T value{};
        std::memcpy(&value, s_returnData, sizeof(T));
        return value;
    }

    template <typename Ret, typename... Args>
    static Ret invoke(uint32_t hash, Args... args) {
        beginCall();
        (pushArg(args), ...);
        endCall(hash);

        if constexpr (!std::is_void_v<Ret>) {
            return getReturn<Ret>();
        }
    }

private:
    static inline ScriptNativeInitFn s_nativeInit{nullptr};
    static inline ScriptNativePush64Fn s_nativePush64{nullptr};
    static inline ScriptNativeCallFn s_nativeCall{nullptr};
"
    static inline uint64_t s_args[32]{};
    static inline uint32_t s_argCount{0};
    static inline uint64_t s_returnData[4]{};

    static inline std::atomic<bool> s_faulted{false};
    static inline std::atomic<bool> s_scriptHookApiReady{false};
};

} // namespace Frontier::Core
