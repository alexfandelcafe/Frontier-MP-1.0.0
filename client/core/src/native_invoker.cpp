#include "core/native_invoker.hpp"
#include <windows.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>

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

std::string hexValue(uint64_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << value << std::dec;
    return stream.str();
}

int sehScriptNativeCall(
    ScriptNativeInitFn nativeInit,
    ScriptNativePush64Fn nativePush64,
    ScriptNativeCallFn nativeCall,
    uint64_t hash,
    const uint64_t* args,
    uint32_t argCount,
    uint64_t* outReturn,
    uint32_t outReturnWords,
    DWORD* outExceptionCode)
{
    if (outReturn && outReturnWords) {
        std::memset(outReturn, 0, sizeof(uint64_t) * outReturnWords);
    }
    if (outExceptionCode) {
        *outExceptionCode = 0;
    }

    __try {
        nativeInit(hash);

        for (uint32_t i = 0; i < argCount; ++i) {
            nativePush64(args[i]);
        }

        uint64_t* result = nativeCall();
        if (result && outReturn && outReturnWords) {
            std::memcpy(outReturn, result, sizeof(uint64_t) * outReturnWords);
        }

        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outExceptionCode) {
            *outExceptionCode = GetExceptionCode();
        }
        return 0;
    }
}


template <typename T>
T resolveMangledExport(HMODULE module, const char* token) {
    if (!module || !token || !*token) {
        return nullptr;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }

    const auto& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || !directory.Size) {
        return nullptr;
    }

    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        reinterpret_cast<const uint8_t*>(module) + directory.VirtualAddress);

    const auto* names = reinterpret_cast<const DWORD*>(
        reinterpret_cast<const uint8_t*>(module) + exports->AddressOfNames);

    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const char* exportedName = reinterpret_cast<const char*>(
            reinterpret_cast<const uint8_t*>(module) + names[i]);

        const char* match = std::strstr(exportedName, token);
        if (!match) {
            continue;
        }

        const char after = match[std::strlen(token)];
        if (after != '\0' && after != '@') {
            continue;
        }

        FARPROC proc = GetProcAddress(module, exportedName);
        if (proc) {
            return reinterpret_cast<T>(proc);
        }
    }

    return nullptr;
}

} // namespace

void NativeInvoker::setScriptHookApi(
    ScriptNativeInitFn nativeInit,
    ScriptNativePush64Fn nativePush64,
    ScriptNativeCallFn nativeCall)
{
    s_nativeInit = nativeInit;
    s_nativePush64 = nativePush64;
    s_nativeCall = nativeCall;

    const bool ready = s_nativeInit != nullptr &&
                       s_nativePush64 != nullptr &&
                       s_nativeCall != nullptr;
    s_scriptHookApiReady.store(ready, std::memory_order_release);

    std::ostringstream line;
    line << "[NativeInvoker] ScriptHook native API "
         << (ready ? "lista." : "incompleta.")
         << " nativeInit=" << hexValue(reinterpret_cast<uintptr_t>(s_nativeInit))
         << " nativePush64=" << hexValue(reinterpret_cast<uintptr_t>(s_nativePush64))
         << " nativeCall=" << hexValue(reinterpret_cast<uintptr_t>(s_nativeCall));
    std::cout << line.str() << std::endl;
    appendNativeLog(line.str());
}

bool NativeInvoker::initialize() {
    if (s_faulted.load(std::memory_order_acquire)) {
        return false;
    }

    if (s_scriptHookApiReady.load(std::memory_order_acquire)) {
        return true;
    }

    HMODULE hookModule = GetModuleHandleA("ScriptHookRDR.dll");
    if (!hookModule) {
        const std::string message =
            "[NativeInvoker] ScriptHookRDR.dll todavía no está cargado.";
        appendNativeLog(message);
        return false;
    }

    const auto nativeInit = resolveMangledExport<ScriptNativeInitFn>(
        hookModule, "nativeInit");
    const auto nativePush64 = resolveMangledExport<ScriptNativePush64Fn>(
        hookModule, "nativePush64");
    const auto nativeCall = resolveMangledExport<ScriptNativeCallFn>(
        hookModule, "nativeCall");
    setScriptHookApi(nativeInit, nativePush64, nativeCall);

    if (!s_scriptHookApiReady.load(std::memory_order_acquire)) {
        static bool loggedUnavailable = false;
        if (!loggedUnavailable) {
            loggedUnavailable = true;
            const std::string message =
                "[NativeInvoker] No se pudieron resolver los exports nativos de ScriptHookRDR.";
            std::cerr << message << std::endl;
            appendNativeLog(message);
        }
        return false;
    }

    return true;
}

void NativeInvoker::init(uintptr_t) {
    initialize();
}

void NativeInvoker::beginCall() {
    s_argCount = 0;
    std::memset(s_args, 0, sizeof(s_args));
    std::memset(s_returnData, 0, sizeof(s_returnData));
}

void NativeInvoker::endCall(uint32_t hash) {
    if (!s_scriptHookApiReady.load(std::memory_order_acquire) ||
        s_faulted.load(std::memory_order_acquire)) {
        return;
    }

    DWORD exceptionCode = 0;
    if (!sehScriptNativeCall(
            s_nativeInit,
            s_nativePush64,
            s_nativeCall,
            static_cast<uint64_t>(hash),
            s_args,
            s_argCount,
            s_returnData,
            4,
            &exceptionCode)) {

        std::ostringstream line;
        line << "[NativeInvoker] Excepción de ScriptHook nativeCall"
             << " | code=0x" << std::hex << exceptionCode
             << " | hash=" << hexValue(hash);

        std::cerr << line.str() << std::endl;
        appendNativeLog(line.str());
        s_faulted.store(true, std::memory_order_release);
        return;
    }

    std::ostringstream line;
    line << "[NativeInvoker] nativeCall OK hash="
         << hexValue(hash)
         << " argc=" << s_argCount
         << " return0=" << hexValue(s_returnData[0]);
    appendNativeLog(line.str());
    std::cout << line.str() << std::endl;
}

} // namespace Frontier::Core
