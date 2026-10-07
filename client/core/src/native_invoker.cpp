#include "core/native_invoker.hpp"
#include "core/pattern_scanner.hpp"
#include <windows.h>
#include <iostream>
#include <fstream>
#include <sstream>

namespace Frontier::Core {

namespace {

std::string nativeLogPath() {
    char modulePath[MAX_PATH] = {};
    HMODULE module = GetModuleHandleA("frontier_core.dll");
    if (!module || !GetModuleFileNameA(module, modulePath, MAX_PATH)) {
        return "frontier_native_crash.log";
    }

    std::string path(modulePath);
    const size_t slash = path.find_last_of("\\\\/");
    if (slash != std::string::npos) {
        path.resize(slash + 1);
    } else {
        path.clear();
    }

    return path + "frontier_native_crash.log";
}

void appendNativeLog(const std::string& line) {
    std::ofstream log(nativeLogPath(), std::ios::app);
    if (log.is_open()) {
        log << line << std::endl;
    }
}

std::string hexValue(uintptr_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << value << std::dec;
    return stream.str();
}

bool isReadableAddress(uintptr_t address, SIZE_T bytes = sizeof(uintptr_t)) {
    if (!address || bytes == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }

    if (mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS || mbi.Protect == PAGE_GUARD) {
        return false;
    }

    const uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t regionEnd = regionStart + mbi.RegionSize;
    return address >= regionStart && bytes <= regionEnd - address;
}

bool isExecutableAddress(uintptr_t address) {
    if (!address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }

    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) {
        return false;
    }

    const DWORD protection = mbi.Protect & 0xFF;
    return protection == PAGE_EXECUTE ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

bool readQword(uintptr_t address, uint64_t* outValue) {
    if (!outValue || !isReadableAddress(address, sizeof(uint64_t))) {
        return false;
    }
    *outValue = *reinterpret_cast<const uint64_t*>(address);
    return true;
}

void dumpRegistrationLayout(uintptr_t registration) {
    std::ostringstream line;
    line << "[NativeInvoker] Inspeccionando layout de sm_CommandsRegistration en "
         << hexValue(registration);
    appendNativeLog(line.str());
    std::cerr << line.str() << std::endl;

    for (uint32_t offset = 0; offset <= 0x38; offset += 8) {
        uint64_t value = 0;
        if (readQword(registration + offset, &value)) {
            std::ostringstream field;
            field << "[NativeInvoker] reg+" << std::hex << offset
                  << " = 0x" << value << std::dec;
            appendNativeLog(field.str());
            std::cerr << field.str() << std::endl;
        } else {
            std::ostringstream field;
            field << "[NativeInvoker] reg+" << std::hex << offset
                  << " = <unreadable>" << std::dec;
            appendNativeLog(field.str());
            std::cerr << field.str() << std::endl;
        }
    }

    uint64_t maybeTable = 0;
    if (readQword(registration + 8, &maybeTable) && maybeTable) {
        std::ostringstream field;
        field << "[NativeInvoker] Candidato de tabla en reg+8: "
              << hexValue(static_cast<uintptr_t>(maybeTable));
        appendNativeLog(field.str());
        std::cerr << field.str() << std::endl;

        for (uint32_t offset = 0; offset <= 0x30; offset += 8) {
            uint64_t value = 0;
            if (readQword(static_cast<uintptr_t>(maybeTable) + offset, &value)) {
                std::ostringstream entry;
                entry << "[NativeInvoker] candidateTable+" << std::hex << offset
                      << " = 0x" << value << std::dec;
                appendNativeLog(entry.str());
                std::cerr << entry.str() << std::endl;
            }
        }
    }
}

// Estos helpers contienen solo tipos POD. MSVC permite __try/__except aquí
// sin activar C2712 en las funciones que construyen objetos C++.
int sehGetCommand(
    scrGetCommandHandler function,
    uint32_t hash,
    scrNativeHandler* outHandler,
    DWORD* outCode)
{
    if (outHandler) {
        *outHandler = nullptr;
    }
    if (outCode) {
        *outCode = 0;
    }

    __try {
        if (outHandler) {
            *outHandler = function ? function(hash) : nullptr;
        }
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) {
            *outCode = GetExceptionCode();
        }
        return 0;
    }
}

int sehReadRegistration(
    uintptr_t* registration,
    uintptr_t* outTable,
    uint32_t* outCapacity,
    DWORD* outCode)
{
    if (outTable) {
        *outTable = 0;
    }
    if (outCapacity) {
        *outCapacity = 0;
    }
    if (outCode) {
        *outCode = 0;
    }

    __try {
        if (outTable) {
            *outTable = registration ? registration[0] : 0;
        }
        if (outCapacity) {
            *outCapacity = registration ? static_cast<uint32_t>(registration[1]) : 0;
        }
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) {
            *outCode = GetExceptionCode();
        }
        return 0;
    }
}

int sehReadEntry(
    uintptr_t entryAddress,
    uint32_t* outHash,
    scrNativeHandler* outHandler,
    DWORD* outCode)
{
    if (outHash) {
        *outHash = 0;
    }
    if (outHandler) {
        *outHandler = nullptr;
    }
    if (outCode) {
        *outCode = 0;
    }

    __try {
        if (outHash) {
            *outHash = *reinterpret_cast<uint32_t*>(entryAddress);
        }
        if (outHandler) {
            *outHandler = *reinterpret_cast<scrNativeHandler*>(entryAddress + 8);
        }
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) {
            *outCode = GetExceptionCode();
        }
        return 0;
    }
}

int sehInvoke(
    scrNativeHandler handler,
    scrNativeCallContext* context,
    DWORD* outCode)
{
    if (outCode) {
        *outCode = 0;
    }

    __try {
        if (handler) {
            handler(context);
        }
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) {
            *outCode = GetExceptionCode();
        }
        return 0;
    }
}

void logSehFailure(const char* phase, uint32_t hash, DWORD code) {
    std::ostringstream line;
    line << "[NativeInvoker] SEH atrapó una excepción durante " << phase
         << " | code=0x" << std::hex << code
         << " | hash=" << hexValue(hash);
    appendNativeLog(line.str());
    std::cerr << line.str() << std::endl;
}

} // namespace

bool NativeInvoker::initialize() {
    if (s_faulted.load(std::memory_order_acquire)) {
        return false;
    }

    if (s_layoutConfirmed.load(std::memory_order_acquire)) {
        return true;
    }

    if (!s_regPtrAddress) {
        uintptr_t match = PatternScanner::findPattern(nullptr, "4C 8B 1D ? ? ? ? 41 8B C1");
        if (!match) {
            static bool loggedPatternMiss = false;
            if (!loggedPatternMiss) {
                loggedPatternMiss = true;
                const std::string message =
                    "[NativeInvoker] No se encontró el patrón de sm_CommandsRegistration: "
                    "4C 8B 1D ? ? ? ? 41 8B C1";
                std::cerr << message << std::endl;
                appendNativeLog(message);
            }
            return false;
        }

        s_regPtrAddress = PatternScanner::getRelativeAddress(match, 7, 3);

        std::ostringstream line;
        line << "[NativeInvoker] Patrón encontrado en "
             << hexValue(match)
             << ", global resuelto en "
             << hexValue(s_regPtrAddress);
        std::cout << line.str() << std::endl;
        appendNativeLog(line.str());
    }

    if (!isReadableAddress(s_regPtrAddress, sizeof(uintptr_t))) {
        const std::string message =
            "[NativeInvoker] El global resuelto no es legible: " +
            hexValue(s_regPtrAddress);
        std::cerr << message << std::endl;
        appendNativeLog(message);
        return false;
    }

    s_commandsRegistration =
        *reinterpret_cast<uintptr_t**>(s_regPtrAddress);

    if (!s_commandsRegistration) {
        return false;
    }

    if (!isReadableAddress(
            reinterpret_cast<uintptr_t>(s_commandsRegistration),
            sizeof(uintptr_t) * 2)) {
        const std::string message =
            "[NativeInvoker] sm_CommandsRegistration apunta a memoria no legible: " +
            hexValue(reinterpret_cast<uintptr_t>(s_commandsRegistration));
        std::cerr << message << std::endl;
        appendNativeLog(message);
        s_commandsRegistration = nullptr;
        return false;
    }

    static bool layoutDumped = false;
    if (!layoutDumped) {
        layoutDumped = true;
        dumpRegistrationLayout(reinterpret_cast<uintptr_t>(s_commandsRegistration));
        appendNativeLog(
            "[NativeInvoker] Layout no confirmado: no se ejecutaran natives hasta identificar "
            "la estructura real de sm_CommandsRegistration.");
    }

    // La implementación anterior asumía que reg[0] era un puntero a tabla y
    // reg[1] su capacidad. La salida de RDR demuestra que esa interpretación
    // es incorrecta (0x100000000 y 0x142c8b028). No marcamos el invocador como
    // listo ni ejecutamos hashes con un layout no confirmado.
    return false;
}

void NativeInvoker::init(uintptr_t getCommandAddress) {
    if (!isExecutableAddress(getCommandAddress)) {
        const std::string message =
            "[NativeInvoker] getCommandAddress rechazado por no ser ejecutable: " +
            hexValue(getCommandAddress);
        std::cerr << message << std::endl;
        appendNativeLog(message);
        s_getCommandFunc = nullptr;
        return;
    }

    s_getCommandFunc =
        reinterpret_cast<scrGetCommandHandler>(getCommandAddress);

    std::ostringstream line;
    line << "[NativeInvoker] getCommand handler configurado en "
         << hexValue(getCommandAddress);
    std::cout << line.str() << std::endl;
    appendNativeLog(line.str());
}

scrNativeHandler NativeInvoker::findNative(uint32_t targetHash) {
    if (s_faulted.load(std::memory_order_acquire)) {
        return nullptr;
    }

    if (s_getCommandFunc) {
        scrNativeHandler handler = nullptr;
        DWORD exceptionCode = 0;

        if (!sehGetCommand(
                s_getCommandFunc,
                targetHash,
                &handler,
                &exceptionCode)) {
            logSehFailure(
                "s_getCommandFunc",
                targetHash,
                exceptionCode);
            s_faulted.store(true, std::memory_order_release);
            return nullptr;
        }

        if (!handler) {
            return nullptr;
        }

        if (!isExecutableAddress(reinterpret_cast<uintptr_t>(handler))) {
            std::ostringstream line;
            line << "[NativeInvoker] getCommand devolvió handler no ejecutable para hash "
                 << hexValue(targetHash)
                 << ": "
                 << hexValue(reinterpret_cast<uintptr_t>(handler));
            appendNativeLog(line.str());
            std::cerr << line.str() << std::endl;
            return nullptr;
        }

        return handler;
    }

    if (!s_commandsRegistration ||
        !isReadableAddress(
            reinterpret_cast<uintptr_t>(s_commandsRegistration),
            sizeof(uintptr_t) * 2)) {
        if (!initialize()) {
            return nullptr;
        }
    }

    uintptr_t tablePtr = 0;
    uint32_t capacity = 0;
    DWORD exceptionCode = 0;

    if (!sehReadRegistration(
            s_commandsRegistration,
            &tablePtr,
            &capacity,
            &exceptionCode)) {
        logSehFailure(
            "lectura de sm_CommandsRegistration",
            targetHash,
            exceptionCode);
        s_faulted.store(true, std::memory_order_release);
        return nullptr;
    }

    if (!tablePtr || capacity == 0 || capacity > 0x1000000) {
        std::ostringstream line;
        line << "[NativeInvoker] Tabla inválida para hash "
             << hexValue(targetHash)
             << " | table=" << hexValue(tablePtr)
             << " | capacity=" << capacity;
        appendNativeLog(line.str());
        std::cerr << line.str() << std::endl;
        return nullptr;
    }

    uint8_t* table = reinterpret_cast<uint8_t*>(tablePtr);
    uint32_t probeHash = targetHash;
    uint32_t index = targetHash % capacity;

    for (uint32_t probes = 0; probes < capacity; ++probes) {
        const uintptr_t entryAddress =
            reinterpret_cast<uintptr_t>(table) +
            static_cast<uintptr_t>(index) * 16;

        if (!isReadableAddress(entryAddress, 16)) {
            std::ostringstream line;
            line << "[NativeInvoker] Entrada de tabla no legible para hash "
                 << hexValue(targetHash)
                 << " | entry=" << hexValue(entryAddress)
                 << " | index=" << index
                 << " | capacity=" << capacity;
            appendNativeLog(line.str());
            std::cerr << line.str() << std::endl;
            return nullptr;
        }

        uint32_t entryHash = 0;
        scrNativeHandler handler = nullptr;
        exceptionCode = 0;

        if (!sehReadEntry(
                entryAddress,
                &entryHash,
                &handler,
                &exceptionCode)) {
            logSehFailure(
                "lectura de entrada nativa",
                targetHash,
                exceptionCode);
            s_faulted.store(true, std::memory_order_release);
            return nullptr;
        }

        if (entryHash == targetHash) {
            if (!handler ||
                !isExecutableAddress(reinterpret_cast<uintptr_t>(handler))) {
                std::ostringstream line;
                line << "[NativeInvoker] Hash encontrado pero handler inválido para "
                     << hexValue(targetHash)
                     << " | handler="
                     << hexValue(reinterpret_cast<uintptr_t>(handler))
                     << " | entry="
                     << hexValue(entryAddress);
                appendNativeLog(line.str());
                std::cerr << line.str() << std::endl;
                return nullptr;
            }

            std::ostringstream line;
            line << "[NativeInvoker] Hash "
                 << hexValue(targetHash)
                 << " -> handler "
                 << hexValue(reinterpret_cast<uintptr_t>(handler))
                 << " | entry="
                 << hexValue(entryAddress);
            appendNativeLog(line.str());
            return handler;
        }

        if (entryHash == 0) {
            return nullptr;
        }

        const uint32_t step = (probeHash >> 1) + 1;
        probeHash = step;
        index = (step + index) % capacity;
    }

    appendNativeLog(
        "[NativeInvoker] Sondeo agotado sin encontrar el hash.");
    return nullptr;
}

void NativeInvoker::beginCall() {
    s_argCount = 0;
    std::memset(s_args, 0, sizeof(s_args));
    std::memset(s_returnData, 0, sizeof(s_returnData));
}

void NativeInvoker::endCall(uint32_t hash) {
    if (s_faulted.load(std::memory_order_acquire)) {
        return;
    }

    scrNativeHandler handler = findNative(hash);
    if (!handler) {
        return;
    }

    s_context.m_return = s_returnData;
    s_context.m_argCount = s_argCount;
    s_context.m_args = s_args;
    s_context.m_dataCount = 0;

    {
        std::ostringstream line;
        line << "[NativeInvoker] Llamando hash "
             << hexValue(hash)
             << " | handler="
             << hexValue(reinterpret_cast<uintptr_t>(handler))
             << " | argc="
             << s_argCount;
        appendNativeLog(line.str());
        std::cout << line.str() << std::endl;
    }

    DWORD exceptionCode = 0;
    if (!sehInvoke(
            handler,
            &s_context,
            &exceptionCode)) {
        logSehFailure(
            "handler nativo",
            hash,
            exceptionCode);
        s_faulted.store(true, std::memory_order_release);
        std::cerr << "[NativeInvoker] Se deshabilitó el invocador tras la excepción."
                  << std::endl;
        appendNativeLog(
            "[NativeInvoker] Se deshabilitó el invocador tras la excepción.");
        return;
    }
}

} // namespace Frontier::Core
