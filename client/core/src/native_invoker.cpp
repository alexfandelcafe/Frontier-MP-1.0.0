#include "core/native_invoker.hpp"
#include "core/pattern_scanner.hpp"
#include <windows.h>
#include <iostream>
#include <fstream>
#include <iomanip>
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

std::string hexValue(uintptr_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << value << std::dec;
    return stream.str();
}

int nativeExceptionFilter(EXCEPTION_POINTERS* exceptionInfo, const char* phase, uint32_t hash) {
    uintptr_t exceptionAddress = 0;
    DWORD code = EXCEPTION_ACCESS_VIOLATION;

    if (exceptionInfo) {
        if (exceptionInfo->ExceptionRecord) {
            code = exceptionInfo->ExceptionRecord->ExceptionCode;
            exceptionAddress =
                reinterpret_cast<uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionAddress);
        }
    }

    std::ostringstream line;
    line << "[NativeInvoker] SEH atrapó excepción durante " << phase
         << " | code=0x" << std::hex << code
         << " | exceptionAddress=" << hexValue(exceptionAddress)
         << " | hash=" << hexValue(hash);

    appendNativeLog(line.str());
    std::cerr << line.str() << std::endl;

    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

bool NativeInvoker::initialize() {
    if (s_faulted) {
        return false;
    }

    if (s_commandsRegistration && s_commandsRegistration[0] != 0) {
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
            "[NativeInvoker] El global resuelto no es legible: " + hexValue(s_regPtrAddress);
        std::cerr << message << std::endl;
        appendNativeLog(message);
        return false;
    }

    s_commandsRegistration = *reinterpret_cast<uintptr_t**>(s_regPtrAddress);

    if (!s_commandsRegistration) {
        return false;
    }

    if (!isReadableAddress(reinterpret_cast<uintptr_t>(s_commandsRegistration),
                           sizeof(uintptr_t) * 2)) {
        const std::string message =
            "[NativeInvoker] sm_CommandsRegistration apunta a memoria no legible: " +
            hexValue(reinterpret_cast<uintptr_t>(s_commandsRegistration));
        std::cerr << message << std::endl;
        appendNativeLog(message);
        s_commandsRegistration = nullptr;
        return false;
    }

    if (s_commandsRegistration[0] != 0) {
        std::ostringstream line;
        line << "[NativeInvoker] rage::scrThread::sm_CommandsRegistration listo en "
             << hexValue(reinterpret_cast<uintptr_t>(s_commandsRegistration))
             << " table=" << hexValue(s_commandsRegistration[0])
             << " capacity=" << s_commandsRegistration[1];
        std::cout << line.str() << std::endl;
        appendNativeLog(line.str());
        return true;
    }

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

    s_getCommandFunc = reinterpret_cast<scrGetCommandHandler>(getCommandAddress);

    std::ostringstream line;
    line << "[NativeInvoker] getCommand handler configurado en "
         << hexValue(getCommandAddress);
    std::cout << line.str() << std::endl;
    appendNativeLog(line.str());
}

scrNativeHandler NativeInvoker::findNative(uint32_t targetHash) {
    if (s_faulted) {
        return nullptr;
    }

    if (s_getCommandFunc) {
        scrNativeHandler handler = nullptr;

        __try {
            handler = s_getCommandFunc(targetHash);
        }
        __except (nativeExceptionFilter(GetExceptionInformation(),
                                         "s_getCommandFunc",
                                         targetHash)) {
            s_faulted = true;
            return nullptr;
        }

        if (!handler) {
            return nullptr;
        }

        if (!isExecutableAddress(reinterpret_cast<uintptr_t>(handler))) {
            std::ostringstream line;
            line << "[NativeInvoker] getCommand devolvió handler no ejecutable para hash "
                 << hexValue(targetHash)
                 << ": " << hexValue(reinterpret_cast<uintptr_t>(handler));
            appendNativeLog(line.str());
            std::cerr << line.str() << std::endl;
            return nullptr;
        }

        return handler;
    }

    if (!s_commandsRegistration ||
        !isReadableAddress(reinterpret_cast<uintptr_t>(s_commandsRegistration),
                           sizeof(uintptr_t) * 2)) {
        if (!initialize()) {
            return nullptr;
        }
    }

    uintptr_t tablePtr = 0;
    uint32_t capacity = 0;

    __try {
        tablePtr = s_commandsRegistration[0];
        capacity = static_cast<uint32_t>(s_commandsRegistration[1]);
    }
    __except (nativeExceptionFilter(GetExceptionInformation(),
                                    "lectura de sm_CommandsRegistration",
                                    targetHash)) {
        s_faulted = true;
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

    // La implementación actual trata la tabla como slots abiertos de 16 bytes.
    // Validamos cada acceso antes de desreferenciarlo para evitar que una firma
    // incorrecta de RDR.exe convierta un hash en un crash del proceso.
    uint8_t* table = reinterpret_cast<uint8_t*>(tablePtr);
    uint32_t probeHash = targetHash;
    uint32_t index = targetHash % capacity;

    for (uint32_t probes = 0; probes < capacity; ++probes) {
        const uintptr_t entryAddress =
            reinterpret_cast<uintptr_t>(table) + static_cast<uintptr_t>(index) * 16;

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

        __try {
            entryHash = *reinterpret_cast<uint32_t*>(entryAddress);
            handler = *reinterpret_cast<scrNativeHandler*>(entryAddress + 8);
        }
        __except (nativeExceptionFilter(GetExceptionInformation(),
                                        "lectura de entrada nativa",
                                        targetHash)) {
            s_faulted = true;
            return nullptr;
        }

        if (entryHash == targetHash) {
            if (!handler ||
                !isExecutableAddress(reinterpret_cast<uintptr_t>(handler))) {
                std::ostringstream line;
                line << "[NativeInvoker] Hash encontrado pero handler inválido para "
                     << hexValue(targetHash)
                     << " | handler=" << hexValue(reinterpret_cast<uintptr_t>(handler))
                     << " | entry=" << hexValue(entryAddress);
                appendNativeLog(line.str());
                std::cerr << line.str() << std::endl;
                return nullptr;
            }

            std::ostringstream line;
            line << "[NativeInvoker] Hash " << hexValue(targetHash)
                 << " -> handler " << hexValue(reinterpret_cast<uintptr_t>(handler))
                 << " | entry=" << hexValue(entryAddress);
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

    appendNativeLog("[NativeInvoker] Sondeo agotado sin encontrar el hash.");
    return nullptr;
}

void NativeInvoker::beginCall() {
    s_argCount = 0;
    std::memset(s_args, 0, sizeof(s_args));
    std::memset(s_returnData, 0, sizeof(s_returnData));
}

void NativeInvoker::endCall(uint32_t hash) {
    if (s_faulted) {
        return;
    }

    scrNativeHandler handler = nullptr;

    __try {
        handler = findNative(hash);
    }
    __except (nativeExceptionFilter(GetExceptionInformation(),
                                    "findNative",
                                    hash)) {
        s_faulted = true;
        return;
    }

    if (!handler) {
        return;
    }

    s_context.m_return = s_returnData;
    s_context.m_argCount = s_argCount;
    s_context.m_args = s_args;
    s_context.m_dataCount = 0;

    {
        std::ostringstream line;
        line << "[NativeInvoker] Llamando hash " << hexValue(hash)
             << " | handler=" << hexValue(reinterpret_cast<uintptr_t>(handler))
             << " | argc=" << s_argCount;
        appendNativeLog(line.str());
        std::cout << line.str() << std::endl;
    }

    __try {
        handler(&s_context);
    }
    __except (nativeExceptionFilter(GetExceptionInformation(),
                                    "handler nativo",
                                    hash)) {
        s_faulted = true;
        std::cerr << "[NativeInvoker] Se deshabilitó el invocador tras la excepción." << std::endl;
        appendNativeLog("[NativeInvoker] Se deshabilitó el invocador tras la excepción.");
        return;
    }
}

} // namespace Frontier::Core
