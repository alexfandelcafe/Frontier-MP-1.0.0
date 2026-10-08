#include "net/resource_client.hpp"

#include <windows.h>
#include <winhttp.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Frontier::Net {

namespace {

std::wstring widen(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

bool splitUrlTarget(
    HINTERNET session,
    const std::string& host,
    uint16_t port,
    const std::string& path,
    HINTERNET& connection,
    HINTERNET& request)
{
    connection = WinHttpConnect(
        session,
        widen(host).c_str(),
        port,
        0);

    if (!connection) return false;

    request = WinHttpOpenRequest(
        connection,
        L"GET",
        widen(path).c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0);

    if (!request) {
        WinHttpCloseHandle(connection);
        connection = nullptr;
        return false;
    }

    return true;
}

bool beginRequest(
    const std::string& host,
    uint16_t port,
    const std::string& path,
    HINTERNET& session,
    HINTERNET& connection,
    HINTERNET& request)
{
    session = WinHttpOpen(
        L"FrontierMP/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!session) return false;

    WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);

    if (!splitUrlTarget(session, host, port, path, connection, request)) {
        WinHttpCloseHandle(session);
        session = nullptr;
        return false;
    }

    if (!WinHttpSendRequest(
            request,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        request = nullptr;
        connection = nullptr;
        session = nullptr;
        return false;
    }

    return true;
}

bool queryStatus(HINTERNET request) {
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);

    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }

    return statusCode >= 200 && statusCode < 300;
}

bool readResponse(HINTERNET request, std::string& body) {
    body.clear();

    std::vector<char> buffer(64 * 1024);

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            return false;
        }

        if (available == 0) break;

        const DWORD toRead =
            (available < buffer.size())
                ? available
                : static_cast<DWORD>(buffer.size());

        DWORD read = 0;
        if (!WinHttpReadData(
                request,
                buffer.data(),
                toRead,
                &read)) {
            return false;
        }

        if (read == 0) break;
        body.append(buffer.data(), buffer.data() + read);
    }

    return true;
}

bool writeResponseToFile(HINTERNET request, const fs::path& destination) {
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);

    const fs::path temporary = destination.string() + ".part";
    fs::remove(temporary, ec);

    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) return false;

    std::vector<char> buffer(64 * 1024);

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            output.close();
            fs::remove(temporary, ec);
            return false;
        }

        if (available == 0) break;

        const DWORD toRead =
            (available < buffer.size())
                ? available
                : static_cast<DWORD>(buffer.size());

        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), toRead, &read)) {
            output.close();
            fs::remove(temporary, ec);
            return false;
        }

        if (read == 0) break;

        output.write(buffer.data(), static_cast<std::streamsize>(read));
        if (!output) {
            output.close();
            fs::remove(temporary, ec);
            return false;
        }
    }

    output.close();

    fs::remove(destination, ec);
    fs::rename(temporary, destination, ec);
    return !ec;
}

std::string urlEncodePathSegment(const std::string& value) {
    std::ostringstream out;

    for (unsigned char c : value) {
        const bool safe =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';

        if (safe) {
            out << static_cast<char>(c);
        } else {
            const char hex[] = "0123456789ABCDEF";
            out << '%' << hex[(c >> 4) & 0xF] << hex[c & 0xF];
        }
    }

    return out.str();
}

} // namespace

bool ResourceClient::httpGet(
    const std::string& host,
    uint16_t port,
    const std::string& path,
    std::string& body)
{
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;

    if (!beginRequest(host, port, path, session, connection, request)) {
        return false;
    }

    const bool ok =
        queryStatus(request) &&
        readResponse(request, body);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

bool ResourceClient::httpDownloadFile(
    const std::string& host,
    uint16_t port,
    const std::string& path,
    const fs::path& destination)
{
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;

    if (!beginRequest(host, port, path, session, connection, request)) {
        return false;
    }

    const bool ok =
        queryStatus(request) &&
        writeResponseToFile(request, destination);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

bool ResourceClient::downloadAll(
    const std::string& host,
    uint16_t httpPort,
    const fs::path& cacheRoot)
{
    std::string manifest;
    if (!httpGet(host, httpPort, "/manifest", manifest)) {
        std::cerr
            << "[ResourceClient] No se pudo obtener /manifest desde "
            << host << ":" << httpPort << std::endl;
        return false;
    }

    std::cout << "[ResourceClient] Manifest recibido." << std::endl;

    std::istringstream input(manifest);
    std::string resource;
    std::string relativeFile;
    uint32_t count = 0;

    while (std::getline(input, resource, '\t')) {
        if (!std::getline(input, relativeFile)) {
            break;
        }

        if (resource.empty() || relativeFile.empty()) {
            continue;
        }

        // El servidor solo debe emitir rutas relativas. Rechazar traversal
        // también protege al cliente ante un servidor mal configurado.
        const fs::path relative(relativeFile);
        if (relative.is_absolute() ||
            relative.lexically_normal().generic_string().find("../") == 0) {
            std::cerr
                << "[ResourceClient] Ruta de recurso rechazada: "
                << relativeFile << std::endl;
            return false;
        }

        const fs::path destination =
            cacheRoot / resource / relative;

        const std::string encodedResource =
            urlEncodePathSegment(resource);

        std::string encodedFile;
        for (const auto& part : relative) {
            if (!encodedFile.empty()) encodedFile += '/';
            encodedFile += urlEncodePathSegment(part.string());
        }

        const std::string requestPath =
            "/resources/" + encodedResource + "/" + encodedFile;

        std::cout
            << "[ResourceClient] Downloading " << resource
            << "/" << relative.generic_string() << "..."
            << std::endl;

        if (!httpDownloadFile(
                host,
                httpPort,
                requestPath,
                destination)) {

            std::cerr
                << "[ResourceClient] Falló la descarga: "
                << requestPath << std::endl;
            return false;
        }

        ++count;
    }

    std::cout
        << "[ResourceClient] Todos los recursos descargados. Archivos="
        << count << " Cache=" << cacheRoot.string()
        << std::endl;

    return true;
}

} // namespace Frontier::Net
