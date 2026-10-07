#include "dependency_bootstrap.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

namespace Frontier::Launcher {

namespace {

constexpr const char* kScriptHookVersion = "1.5.2";
// Public mirror configured for the runtime dependency bootstrap.
// ScriptHookRDR remains outside the repository and is downloaded only when
// the client-side copy is missing.
constexpr const char* kPrimaryDownloadUrl =
    "https://raw.githubusercontent.com/alexfandelcafe/Frontier-MP-1.0.0/main/ScriptHookRDR.dll";

bool writeTextFile(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << content;
    return static_cast<bool>(file);
}

bool fileLooksValid(const fs::path& path, uintmax_t minimumBytes = 4096) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return false;
    }
    const auto size = fs::file_size(path, ec);
    return !ec && size >= minimumBytes;
}

bool fileLooksLikePeImage(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    file.read(
        reinterpret_cast<char*>(&dos),
        sizeof(dos));

    if (!file || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos))) {
        return false;
    }

    file.seekg(dos.e_lfanew, std::ios::beg);

    DWORD signature = 0;
    file.read(
        reinterpret_cast<char*>(&signature),
        sizeof(signature));

    return file &&
           signature == IMAGE_NT_SIGNATURE;
}

std::wstring widenAscii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

bool downloadFile(const char* url, const fs::path& destination) {
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);

    const fs::path partialPath = destination.string() + ".part";
    fs::remove(partialPath, ec);

    std::cout << "[Launcher] Descargando ScriptHookRDR " << kScriptHookVersion
              << "..." << std::endl;

    const std::wstring wideUrl = widenAscii(url);

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);

    wchar_t hostName[256] = {};
    wchar_t urlPath[4096] = {};
    wchar_t extraInfo[4096] = {};

    components.lpszHostName = hostName;
    components.dwHostNameLength = static_cast<DWORD>(std::size(hostName));
    components.lpszUrlPath = urlPath;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(urlPath));
    components.lpszExtraInfo = extraInfo;
    components.dwExtraInfoLength = static_cast<DWORD>(std::size(extraInfo));

    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components)) {
        std::cerr << "[Launcher] WinHttpCrackUrl falló. Error="
                  << GetLastError() << std::endl;
        return false;
    }

    HINTERNET session = WinHttpOpen(
        L"FrontierMP Launcher/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!session) {
        std::cerr << "[Launcher] WinHttpOpen falló. Error="
                  << GetLastError() << std::endl;
        return false;
    }

    WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);

    const DWORD requestFlags =
        components.nScheme == INTERNET_SCHEME_HTTPS
            ? WINHTTP_FLAG_SECURE
            : 0;

    HINTERNET connection = WinHttpConnect(
        session,
        hostName,
        components.nPort,
        0);

    if (!connection) {
        std::cerr << "[Launcher] WinHttpConnect falló. Error="
                  << GetLastError() << std::endl;
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"GET",
        urlPath,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        requestFlags);

    if (!request) {
        std::cerr << "[Launcher] WinHttpOpenRequest falló. Error="
                  << GetLastError() << std::endl;
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    const BOOL sent = WinHttpSendRequest(
        request,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0);

    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        std::cerr << "[Launcher] WinHTTP no pudo iniciar la descarga. Error="
                  << GetLastError() << std::endl;
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        std::cerr << "[Launcher] No se pudo consultar el HTTP status. Error="
                  << GetLastError() << std::endl;
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    if (statusCode < 200 || statusCode >= 300) {
        std::cerr << "[Launcher] Descarga rechazada por el servidor. HTTP="
                  << statusCode << std::endl;
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    std::ofstream output(partialPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        std::cerr << "[Launcher] No se pudo crear el archivo temporal: "
                  << partialPath.string() << std::endl;
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    std::vector<char> buffer(64 * 1024);

    for (;;) {
        DWORD available = 0;

        if (!WinHttpQueryDataAvailable(request, &available)) {
            std::cerr << "[Launcher] WinHttpQueryDataAvailable falló. Error="
                      << GetLastError() << std::endl;
            output.close();
            fs::remove(partialPath, ec);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }

        if (available == 0) {
            break;
        }

        const DWORD toRead = std::min<DWORD>(
            available,
            static_cast<DWORD>(buffer.size()));

        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), toRead, &read)) {
            std::cerr << "[Launcher] WinHttpReadData falló. Error="
                      << GetLastError() << std::endl;
            output.close();
            fs::remove(partialPath, ec);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }

        if (read == 0) {
            break;
        }

        output.write(buffer.data(), static_cast<std::streamsize>(read));
        if (!output) {
            std::cerr << "[Launcher] Error escribiendo el archivo temporal."
                      << std::endl;
            output.close();
            fs::remove(partialPath, ec);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }
    }

    output.close();
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!fileLooksValid(partialPath, 256 * 1024) ||
        !fileLooksLikePeImage(partialPath)) {
        fs::remove(partialPath, ec);
        std::cerr << "[Launcher] La descarga terminó pero el archivo no es válido."
                  << std::endl;
        return false;
    }

    fs::remove(destination, ec);
    fs::rename(partialPath, destination, ec);
    if (ec) {
        fs::remove(partialPath, ec);
        std::cerr << "[Launcher] No se pudo mover el archivo descargado a: "
                  << destination.string() << " | error=" << ec.message()
                  << std::endl;
        return false;
    }

    std::cout << "[Launcher] ScriptHookRDR descargado correctamente."
              << std::endl;
    return true;
}

bool copyIfMissing(const fs::path& source, const fs::path& destination) {
    std::error_code ec;
    if (fileLooksValid(destination)) {
        return true;
    }

    fs::create_directories(destination.parent_path(), ec);
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[Launcher] No se pudo copiar " << source.string()
                  << " -> " << destination.string()
                  << " | error=" << ec.message() << std::endl;
        return false;
    }

    return fileLooksValid(destination);
}

void writeDependencyMarker(const fs::path& directory, const fs::path& scriptHook) {
    std::ostringstream content;
    content << "ScriptHookRDR=" << kScriptHookVersion << "\n";
    content << "ScriptHookRDR.dll=" << scriptHook.string() << "\n";
    writeTextFile(directory / "scripthookrdr.runtime.txt", content.str());
}

} // namespace

DependencyBootstrapResult ensureScriptHookRDR(
    const fs::path& gameDirectory,
    const fs::path& launcherDirectory) {

    (void)gameDirectory;

    DependencyBootstrapResult result;
    std::error_code ec;

    const fs::path clientScriptHook =
        launcherDirectory / "ScriptHookRDR.dll";

    // The game installation is deliberately not touched. Frontier owns this
    // dependency and loads it explicitly from the client directory.
    if (fileLooksValid(clientScriptHook) &&
        fileLooksLikePeImage(clientScriptHook)) {
        result.ready = true;
        result.preparedInClient = true;
        result.scriptHookPath = clientScriptHook;
        std::cout << "[Launcher] ScriptHookRDR.dll ya está disponible en: "
                  << clientScriptHook.string() << std::endl;
        return result;
    }

    const fs::path cacheDirectory =
        launcherDirectory / "dependencies" /
        "scripthookrdr" / kScriptHookVersion;

    const fs::path cachedScriptHook =
        cacheDirectory / "ScriptHookRDR.dll";

    fs::create_directories(cacheDirectory, ec);

    if (!fileLooksValid(cachedScriptHook)) {
        if (!downloadFile(kPrimaryDownloadUrl, cachedScriptHook)) {
            result.message =
                "No se pudo descargar ScriptHookRDR 1.5.2 automáticamente.";
            return result;
        }

        result.downloaded = true;
    }

    if (!fileLooksValid(cachedScriptHook) ||
        !fileLooksLikePeImage(cachedScriptHook)) {
        result.message =
            "ScriptHookRDR.dll no quedó disponible en la caché o no es un PE válido.";
        return result;
    }
    if (!copyIfMissing(cachedScriptHook, clientScriptHook)) {
        result.message =
            "No se pudo preparar ScriptHookRDR.dll en el directorio del cliente.";
        return result;
    }

    result.preparedInClient = true;
    result.scriptHookPath = clientScriptHook;
    result.ready =
        fileLooksValid(clientScriptHook) &&
        fileLooksLikePeImage(clientScriptHook);

    if (result.ready) {
        writeDependencyMarker(cacheDirectory, result.scriptHookPath);
        std::cout << "[Launcher] ScriptHookRDR " << kScriptHookVersion
                  << " listo en: " << result.scriptHookPath.string() << std::endl;
        std::cout << "[Launcher] La instalación del juego no fue modificada."
                  << std::endl;
    } else {
        result.message = "ScriptHookRDR no quedó disponible en el cliente.";
    }

    return result;
}

} // namespace Frontier::Launcher
