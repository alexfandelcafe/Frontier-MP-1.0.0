#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <atomic>

namespace Frontier::Core {

struct scrNativeCallContext {
    void* m_return;
    uint32_t m_argCount;
    void* m_args;
    uint32_t m_dataCount;

    uint8_t m_vectorSpace[192];
};

using scrNativeHandler = void (*)(scrNativeCallContext*);
using scrGetCommandHandler = scrNativeHandler (*)(uint32_t hash);

class NativeInvoker {
public:
    static bool initialize();
    static void init(uintptr_t getCommandAddress);
    static bool isReady() {
        return !s_faulted.load(std::memory_order_acquire) &&
               (s_commandsRegistration != nullptr || s_getCommandFunc != nullptr);
    }
    static scrNativeHandler findNative(uint32_t hash);

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
        return *reinterpret_cast<T*>(&s_returnData);
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
    static inline scrGetCommandHandler s_getCommandFunc{nullptr};
    static inline uintptr_t s_regPtrAddress{0};
    static inline uintptr_t* s_commandsRegistration{nullptr};
    static inline uint64_t s_args[32]{};
    static inline uint32_t s_argCount{0};
    static inline uint64_t s_returnData[4]{};
    static inline scrNativeCallContext s_context{};
    static inline std::atomic<bool> s_faulted{false};
};

} // namespace Frontier::Core
