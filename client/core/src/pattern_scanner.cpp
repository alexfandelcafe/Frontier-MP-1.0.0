#include "core/pattern_scanner.hpp"
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace Frontier::Core {

uintptr_t PatternScanner::findPattern(const char* moduleName, const std::string& pattern) {
#ifdef _WIN32
    HMODULE hMod = moduleName ? GetModuleHandleA(moduleName) : GetModuleHandleA(nullptr);
    if (!hMod) return 0;

    MODULEINFO modInfo{};
    if (!GetModuleInformation(GetCurrentProcess(), hMod, &modInfo, sizeof(modInfo))) {
        return 0;
    }

    return findPattern(reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll), modInfo.SizeOfImage, pattern);
#else
    return 0;
#endif
}

uintptr_t PatternScanner::findPattern(uintptr_t startAddress, size_t size, const std::string& pattern) {
    std::vector<int> patternBytes;
    std::istringstream stream(pattern);
    std::string byteStr;

    while (stream >> byteStr) {
        if (byteStr == "?" || byteStr == "??") {
            patternBytes.push_back(-1); // Wildcard
        } else {
            patternBytes.push_back(std::stoi(byteStr, nullptr, 16));
        }
    }

    const auto* scanBytes = reinterpret_cast<const uint8_t*>(startAddress);
    const size_t patternSize = patternBytes.size();

    for (size_t i = 0; i <= size - patternSize; ++i) {
        bool found = true;
        for (size_t j = 0; j < patternSize; ++j) {
            if (patternBytes[j] != -1 && scanBytes[i + j] != static_cast<uint8_t>(patternBytes[j])) {
                found = false;
                break;
            }
        }
        if (found) {
            return startAddress + i;
        }
    }

    return 0;
}

uintptr_t PatternScanner::getRelativeAddress(uintptr_t instructionAddress, int instructionSize, int offsetPosition) {
    if (!instructionAddress) return 0;
    int32_t offset = *reinterpret_cast<const int32_t*>(instructionAddress + offsetPosition);
    return instructionAddress + instructionSize + offset;
}

} // namespace Frontier::Core
