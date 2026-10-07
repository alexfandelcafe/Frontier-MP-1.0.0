#include "dependency_bootstrap.hpp"

#include <windows.h>
#include <urlmon.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "urlmon.lib")

namespace fs = std::filesystem;

namespace Frontier::Launcher {

namespace {

constexpr const char* kScriptHookVersion = "1.5.2";
constexpr const char* kScriptHookArchiveName = "ScriptHookRDR-164-1-5-2-1738573417.zip";

// This mirror is publicly linked by a current ScriptHookRDR 1.5.2 download guide.
// The launcher only downloads the archive when ScriptHookRDR.dll is missing.
constexpr const char* kPrimaryDownloadUrl =
    "https://www.dropbox.com/scl/fi/e32pm2rx98rt3hu9xg0ru/"
    "ScriptHookRDR-164-1-5-2-1738573417.zip"
    "?rlkey=ey0dqbf27wj38hh7o36911bvr&st=32g9m9c8&dl=1";

std::string quotePowerShell(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\0') {
            quoted += c;
        } else if (c == '\'') {
            quoted += "''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

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

bool downloadFile(const char* url, const fs::path& destination) {
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);

    const fs::path partialPath = destination.string() + ".part";
    fs::remove(partialPath, ec);

    std::cout << "[Launcher] Descargando ScriptHookRDR " << kScriptHookVersion
              << "..." << std::endl;

    const HRESULT hr = URLDownloadToFileA(
        nullptr,
        url,
        partialPath.string().c_str(),
        0,
        nullptr);

    if (FAILED(hr) || !fileLooksValid(partialPath, 256 * 1024)) {
        fs::remove(partialPath, ec);
        std::cerr << "[Launcher] Falló la descarga de ScriptHookRDR. HRESULT=0x"
                  << std::hex << static_cast<unsigned long>(hr)
                  << std::dec << std::endl;
        return false;
    }

    fs::remove(destination, ec);
    fs::rename(partialPath, destination, ec);
    if (ec) {
        fs::remove(partialPath, ec);
        std::cerr << "[Launcher] No se pudo mover el archivo descargado a: "
                  << destination.string() << " | error=" << ec.message() << std::endl;
        return false;
    }

    return true;
}

bool extractZip(const fs::path& archive, const fs::path& destination) {
    std::error_code ec;
    fs::remove_all(destination, ec);
    fs::create_directories(destination, ec);

    std::string command =
        "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
        "-Command \"Expand-Archive -LiteralPath " +
        quotePowerShell(archive.string()) +
        " -DestinationPath " +
        quotePowerShell(destination.string()) +
        " -Force\"";

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');

    if (!CreateProcessA(
            nullptr,
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo)) {

        std::cerr << "[Launcher] No se pudo ejecutar PowerShell para extraer "
                     "ScriptHookRDR. GetLastError=" << GetLastError() << std::endl;
        return false;
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    if (exitCode != 0) {
        std::cerr << "[Launcher] Expand-Archive falló. ExitCode="
                  << exitCode << std::endl;
        return false;
    }

    return true;
}

fs::path findFileRecursive(const fs::path& root, const std::string& filename) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return {};
    }

    for (const auto& entry : fs::recursive_directory_iterator(
             root,
             fs::directory_options::skip_permission_denied,
             ec)) {

        if (ec) {
            ec.clear();
            continue;
        }

        if (!entry.is_regular_file(ec)) {
            continue;
        }

        if (_stricmp(entry.path().filename().string().c_str(), filename.c_str()) == 0) {
            return entry.path();
        }
    }

    return {};
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

void writeDependencyMarker(
    const fs::path& directory,
    const fs::path& scriptHook,
    const fs::path& loader)
{
    std::ostringstream content;
    content << "ScriptHookRDR=" << kScriptHookVersion << "\n";
    content << "ScriptHookRDR.dll=" << scriptHook.string() << "\n";
    if (!loader.empty()) {
        content << "dinput8.dll=" << loader.string() << "\n";
    }
    writeTextFile(directory / "scripthookrdr.runtime.txt", content.str());
}

} // namespace

DependencyBootstrapResult ensureScriptHookRDR(
    const fs::path& gameDirectory,
    const fs::path& launcherDirectory)
{
    DependencyBootstrapResult result;

    std::error_code ec;
    fs::path gameScriptHook = gameDirectory / "ScriptHookRDR.dll";
    fs::path gameLoader = gameDirectory / "dinput8.dll";

    if (fileLooksValid(gameScriptHook)) {
        result.ready = true;
        result.scriptHookPath = gameScriptHook;
        if (fileLooksValid(gameLoader)) {
            result.loaderPath = gameLoader;
        }

        std::cout << "[Launcher] ScriptHookRDR.dll ya está instalado en: "
                  << gameScriptHook.string() << std::endl;
        return result;
    }

    const fs::path cacheDirectory =
        launcherDirectory / "dependencies" / "scripthookrdr" / kScriptHookVersion;
    const fs::path archivePath = cacheDirectory / kScriptHookArchiveName;
    const fs::path extractDirectory = cacheDirectory / "extracted";
    const fs::path cachedScriptHook = cacheDirectory / "ScriptHookRDR.dll";
    const fs::path cachedLoader = cacheDirectory / "dinput8.dll";

    fs::create_directories(cacheDirectory, ec);

    if (!fileLooksValid(cachedScriptHook)) {
        if (!fileLooksValid(archivePath)) {
            if (!downloadFile(kPrimaryDownloadUrl, archivePath)) {
                result.message =
                    "No se pudo descargar ScriptHookRDR 1.5.2 automáticamente.";
                return result;
            }
            result.downloaded = true;
        }

        if (!extractZip(archivePath, extractDirectory)) {
            result.message = "La descarga de ScriptHookRDR no pudo extraerse.";
            return result;
        }

        const fs::path extractedScriptHook =
            findFileRecursive(extractDirectory, "ScriptHookRDR.dll");
        const fs::path extractedLoader =
            findFileRecursive(extractDirectory, "dinput8.dll");

        if (extractedScriptHook.empty() ||
            !copyIfMissing(extractedScriptHook, cachedScriptHook)) {
            result.message =
                "El paquete descargado no contiene un ScriptHookRDR.dll válido.";
            return result;
        }

        if (!extractedLoader.empty() &&
            !fileLooksValid(cachedLoader, 16 * 1024)) {
            copyIfMissing(extractedLoader, cachedLoader);
        }
    }

    if (!fileLooksValid(cachedScriptHook)) {
        result.message = "ScriptHookRDR.dll no quedó disponible en la caché.";
        return result;
    }

    if (copyIfMissing(cachedScriptHook, gameScriptHook)) {
        result.installedToGame = true;
    }

    // dinput8.dll is optional for Frontier's explicit LoadLibraryEx path.
    // Install it only when the game does not already have one, so we never
    // overwrite another ASI loader or user's existing setup.
    if (!fileLooksValid(gameLoader) && fileLooksValid(cachedLoader, 16 * 1024)) {
        if (copyIfMissing(cachedLoader, gameLoader)) {
            result.loaderPath = gameLoader;
        }
    } else if (fileLooksValid(gameLoader)) {
        result.loaderPath = gameLoader;
    }

    result.scriptHookPath = fileLooksValid(gameScriptHook)
        ? gameScriptHook
        : cachedScriptHook;

    result.ready = fileLooksValid(result.scriptHookPath);
    if (result.ready) {
        writeDependencyMarker(
            cacheDirectory,
            result.scriptHookPath,
            result.loaderPath);
        std::cout << "[Launcher] ScriptHookRDR " << kScriptHookVersion
                  << " listo en: " << result.scriptHookPath.string() << std::endl;
        if (result.installedToGame) {
            std::cout << "[Launcher] ScriptHookRDR instalado en el directorio del juego."
                      << std::endl;
        }
    } else {
        result.message =
            "ScriptHookRDR no quedó disponible después del bootstrap.";
    }

    return result;
}

} // namespace Frontier::Launcher
