#include "core/native_invoker.hpp"
#include "core/pattern_scanner.hpp"
#include <iostream>

namespace Frontier::Core {

bool NativeInvoker::initialize() {
    if (s_commandsRegistration && s_commandsRegistration[0] != 0) return true;

    if (!s_regPtrAddress) {
        uintptr_t match = PatternScanner::findPattern(nullptr, "4C 8B 1D ? ? ? ? 41 8B C1");
        if (!match) {
            return false;
        }

        s_regPtrAddress = PatternScanner::getRelativeAddress(match, 7, 3);
    }

    if (s_regPtrAddress) {
        s_commandsRegistration = *reinterpret_cast<uintptr_t**>(s_regPtrAddress);
        if (s_commandsRegistration && s_commandsRegistration[0] != 0) {
            std::cout << "[NativeInvoker] rage::scrThread::sm_CommandsRegistration listo en 0x" 
                      << std::hex << (uintptr_t)s_commandsRegistration << std::dec << std::endl;
            return true;
        }
    }

    return false;
}

void NativeInvoker::init(uintptr_t getCommandAddress) {
    s_getCommandFunc = reinterpret_cast<scrGetCommandHandler>(getCommandAddress);
}

scrNativeHandler NativeInvoker::findNative(uint32_t targetHash) {
    if (s_getCommandFunc) {
        return s_getCommandFunc(targetHash);
    }

    if (!s_commandsRegistration || s_commandsRegistration[0] == 0) {
        initialize();
        if (!s_commandsRegistration || s_commandsRegistration[0] == 0) return nullptr;
    }

    uintptr_t tablePtr = s_commandsRegistration[0];
    if (!tablePtr) return nullptr;

    uint32_t capacity = static_cast<uint32_t>(s_commandsRegistration[1]);
    if (capacity == 0) return nullptr;

    uint8_t* table = reinterpret_cast<uint8_t*>(tablePtr);
    uint32_t probeHash = targetHash;
    uint32_t index = targetHash % capacity;

    while (true) {
        uint32_t entryHash = *reinterpret_cast<uint32_t*>(table + index * 16);
        if (entryHash == targetHash) {
            return *reinterpret_cast<scrNativeHandler*>(table + index * 16 + 8);
        }
        if (entryHash == 0) {
            return nullptr;
        }

        // Algoritmo de sondeo secundario verificado de RAGE engine
        uint32_t step = (probeHash >> 1) + 1;
        probeHash = step;
        index = (step + index) % capacity;
    }
}

void NativeInvoker::beginCall() {
    s_argCount = 0;
    std::memset(s_args, 0, sizeof(s_args));
    std::memset(s_returnData, 0, sizeof(s_returnData));
}

void NativeInvoker::endCall(uint32_t hash) {
    scrNativeHandler handler = findNative(hash);
    if (!handler) {
        return;
    }

    s_context.m_return = s_returnData;
    s_context.m_argCount = s_argCount;
    s_context.m_args = s_args;
    s_context.m_dataCount = 0;

    handler(&s_context);
}

} // namespace Frontier::Core
