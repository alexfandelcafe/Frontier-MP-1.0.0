#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace Frontier::Core {

class PatternScanner {
public:
    // Escaneo de patrones estilo IDA (ej: "48 8B 05 ? ? ? ? 48 85 C0")
    static uintptr_t findPattern(const char* moduleName, const std::string& pattern);
    static uintptr_t findPattern(uintptr_t startAddress, size_t size, const std::string& pattern);

    // Resuelve direcciones relativas RIP (típicas en x86_64: MOV RAX, [RIP + offset])
    static uintptr_t getRelativeAddress(uintptr_t instructionAddress, int instructionSize, int offsetPosition);
};

} // namespace Frontier::Core
