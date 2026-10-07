#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include "dependency_bootstrap.hpp"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

struct LauncherSettings {
    std::string gamePath; // Ruta a la carpeta que contiene RDR.exe o ruta al exe
    std::string playerName{"Outlaw_John"};
    std::string serverIp{"127.0.0.1"};
    uint16_t serverPort{4674};
    bool autoConnect{true};
    bool openDebugConsole{true};
};

static fs::path getExecutableDirectory() {
#ifdef _WIN32
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    return fs::path(path).parent_path();
#else
    return fs::current_path();
#endif
}

LauncherSettings loadSettings(const fs::path& settingsFile) {
    LauncherSettings s;
    std::ifstream f(settingsFile);
    if (!f.is_open()) return s;

    std::string line;
    while (std::getline(f, line)) {
        if (line.find("\"game_path\"") != std::string::npos) {
            size_t q1 = line.find(':', line.find("\"game_path\""));
            size_t s1 = line.find('\"', q1);
            size_t s2 = line.find('\"', s1 + 1);
            if (s1 != std::string::npos && s2 != std::string::npos) {
                s.gamePath = line.substr(s1 + 1, s2 - s1 - 1);
            }
        } else if (line.find("\"player_name\"") != std::string::npos) {
            size_t q1 = line.find(':', line.find("\"player_name\""));
            size_t s1 = line.find('\"', q1);
            size_t s2 = line.find('\"', s1 + 1);
            if (s1 != std::string::npos && s2 != std::string::npos) {
                s.playerName = line.substr(s1 + 1, s2 - s1 - 1);
            }
        } else if (line.find("\"server_ip\"") != std::string::npos) {
            size_t q1 = line.find(':', line.find("\"server_ip\""));
            size_t s1 = line.find('\"', q1);
            size_t s2 = line.find('\"', s1 + 1);
            if (s1 != std::string::npos && s2 != std::string::npos) {
                s.serverIp = line.substr(s1 + 1, s2 - s1 - 1);
            }
        } else if (line.find("\"server_port\"") != std::string::npos) {
            size_t q1 = line.find(':', line.find("\"server_port\""));
            if (q1 != std::string::npos) {
                s.serverPort = static_cast<uint16_t>(std::stoi(line.substr(q1 + 1)));
            }
        } else if (line.find("\"auto_connect\"") != std::string::npos) {
            if (line.find("false") != std::string::npos) {
                s.autoConnect = false;
            }
        }
    }
    return s;
}

void saveSettings(const fs::path& settingsFile, const LauncherSettings& s) {
    std::ofstream f(settingsFile);
    if (!f.is_open()) return;

    // Escapar barras invertidas para JSON
    std::string escapedPath;
    for (char c : s.gamePath) {
        if (c == '\\') escapedPath += "\\\\";
        else escapedPath += c;
    }

    f << "{\n";
    f << "  \"game_path\": \"" << escapedPath << "\",\n";
    f << "  \"player_name\": \"" << s.playerName << "\",\n";
    f << "  \"server_ip\": \"" << s.serverIp << "\",\n";
    f << "  \"server_port\": " << s.serverPort << ",\n";
    f << "  \"auto_connect\": " << (s.autoConnect ? "true" : "false") << ",\n";
    f << "  \"open_debug_console\": " << (s.openDebugConsole ? "true" : "false") << "\n";
    f << "}\n";
}

std::string selectGameExecutableDialog() {
#ifdef _WIN32
    char filename[MAX_PATH] = {0};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "Red Dead Redemption (RDR.exe)\0RDR.exe;PlayRDR.exe;*.exe\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Selecciona el ejecutable de Red Dead Redemption 1 (RDR.exe)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameA(&ofn)) {
        return std::string(filename);
    }
#endif
    return {};
}

bool injectDll(HANDLE hProcess, const std::string& dllPath) {
#ifdef _WIN32
    std::cout << "[Launcher] Remote-loading: " << dllPath << std::endl;

    void* loc = VirtualAllocEx(hProcess, nullptr, dllPath.size() + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!loc) {
        std::cerr << "[Launcher] Failed to allocate memory in remote process. Error: " << GetLastError() << std::endl;
        return false;
    }

    if (!WriteProcessMemory(hProcess, loc, dllPath.c_str(), dllPath.size() + 1, nullptr)) {
        std::cerr << "[Launcher] Failed to write DLL path to remote process. Error: " << GetLastError() << std::endl;
        VirtualFreeEx(hProcess, loc, 0, MEM_RELEASE);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) {
        std::cerr << "[Launcher] kernel32.dll no está disponible en el proceso del launcher."
                  << std::endl;
        VirtualFreeEx(hProcess, loc, 0, MEM_RELEASE);
        return false;
    }

    LPTHREAD_START_ROUTINE loadLibAddr =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(hKernel32, "LoadLibraryA"));

    if (!loadLibAddr) {
        std::cerr << "[Launcher] LoadLibraryA no pudo resolverse. Error="
                  << GetLastError() << std::endl;
        VirtualFreeEx(hProcess, loc, 0, MEM_RELEASE);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(
        hProcess,
        nullptr,
        0,
        loadLibAddr,
        loc,
        0,
        nullptr
    );

    if (!hThread) {
        std::cerr << "[Launcher] Failed to create remote thread for LoadLibraryA. Error: " << GetLastError() << std::endl;
        VirtualFreeEx(hProcess, loc, 0, MEM_RELEASE);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(hThread, 30000);
    if (waitResult != WAIT_OBJECT_0) {
        std::cerr << "[Launcher] Remote LoadLibrary no terminó correctamente. waitResult="
                  << waitResult << " error=" << GetLastError() << std::endl;
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, loc, 0, MEM_RELEASE);
        return false;
    }

    DWORD exitCode = 0;
    if (!GetExitCodeThread(hThread, &exitCode)) {
        std::cerr << "[Launcher] No se pudo obtener el resultado de LoadLibraryA. Error="
                  << GetLastError() << std::endl;
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, loc, 0, MEM_RELEASE);
        return false;
    }
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, loc, 0, MEM_RELEASE);

    if (exitCode == 0) {
        std::cerr
            << "[Launcher] LoadLibraryA returned NULL in remote process for: "
            << dllPath
            << ". The module did not load or DllMain returned FALSE."
            << std::endl;
        return false;
    }

    std::cout
        << "[Launcher] Remote LoadLibrary succeeded: "
        << dllPath
        << " | remote module=0x"
        << std::hex << static_cast<uintptr_t>(exitCode)
        << std::dec << std::endl;

    return true;
#else
    return false;
#endif
}

int main(int argc, char* argv[]) {
    std::cout << "==========================================================" << std::endl;
    std::cout << "                 FrontierMP - RDR1 PC Launcher            " << std::endl;
    std::cout << "==========================================================" << std::endl;

    fs::path launcherDir = getExecutableDirectory();
    fs::path settingsPath = launcherDir / "settings.json";

    std::cout << "[Launcher] FrontierMP Folder: " << launcherDir.string() << std::endl;

    LauncherSettings settings = loadSettings(settingsPath);

    // Validar si la ruta guardada a RDR.exe existe
    fs::path rdrExePath = settings.gamePath;
    if (fs::is_directory(rdrExePath)) {
        rdrExePath = rdrExePath / "RDR.exe";
    }

    if (!fs::exists(rdrExePath)) {
        std::cout << "\n[Launcher] Red Dead Redemption 1 executable location is not set or not found." << std::endl;
        std::cout << "[Launcher] Opening file selector... Please locate your 'RDR.exe'." << std::endl;

        std::string selected = selectGameExecutableDialog();
        if (selected.empty()) {
            std::cout << "\n[Launcher] No file selected. Enter the full path to RDR.exe manually:\n> ";
            std::getline(std::cin, selected);
        }

        rdrExePath = selected;
        if (!fs::exists(rdrExePath)) {
            std::cerr << "\n[Launcher] Error: The specified path does not exist: " << rdrExePath << std::endl;
            std::cout << "Press Enter to exit..." << std::endl;
            std::cin.get();
            return 1;
        }

        settings.gamePath = rdrExePath.string();
        saveSettings(settingsPath, settings);
        std::cout << "[Launcher] Saved game location to: " << settingsPath.string() << std::endl;
    }

    fs::path gameDir = rdrExePath.parent_path();
    std::cout << "[Launcher] Game Directory: " << gameDir.string() << std::endl;
    std::cout << "[Launcher] Game Executable: " << rdrExePath.string() << std::endl;

    // Bootstrap de dependencias externas. ScriptHookRDR es un binario de runtime,
    // por lo que no se embebe en el repositorio ni se exige una instalación manual.
    const auto scriptHook = Frontier::Launcher::ensureScriptHookRDR(gameDir, launcherDir);
    if (!scriptHook.ready) {
        std::cerr << "[Launcher] [ERROR] ScriptHookRDR 1.5.2 no está disponible. "
                  << scriptHook.message << std::endl;
        std::cout << "Press Enter to exit..." << std::endl;
        std::cin.get();
        return 1;
    }

    // Frontier is loaded manually by the launcher, so keep it as a normal DLL.
    // The .asi extension is relevant to ScriptHook's automatic loader, which
    // we are deliberately not using for the client-side module.
    fs::path dllPath = launcherDir / "frontier_core.dll";
    if (!fs::exists(dllPath)) {
        dllPath = launcherDir / "data" / "frontier_core.dll";
    }

    if (!fs::exists(dllPath)) {
        std::cerr << "[Launcher] Error: 'frontier_core.dll' not found in: " << launcherDir.string() << std::endl;
        std::cout << "Press Enter to exit..." << std::endl;
        std::cin.get();
        return 1;
    }

    std::cout << "[Launcher] Core DLL to inject: " << dllPath.string() << std::endl;
    std::cout << "[Launcher] Launching " << rdrExePath.filename().string() << " suspended..." << std::endl;

#ifdef _WIN32
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    // Ejecutar RDR.exe con su propio directorio de trabajo
    if (!CreateProcessA(
            rdrExePath.string().c_str(),
            nullptr,
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED,
            nullptr,
            gameDir.string().c_str(),
            &si,
            &pi)) {
        std::cerr << "[Launcher] Failed to launch RDR.exe. Error: " << GetLastError() << std::endl;
        std::cout << "Press Enter to exit..." << std::endl;
        std::cin.get();
        return 1;
    }

    std::cout << "[Launcher] Process launched (PID: " << pi.dwProcessId
              << "). Staging FrontierMP before ScriptHookRDR..." << std::endl;

    // Frontier is made resident first. Its worker thread can then observe
    // ScriptHookRDR as soon as that module appears, which avoids racing the
    // ScriptHook startup scan while the game main thread is still suspended.
    // Neither module is copied into or loaded from the game installation.
    if (!injectDll(pi.hProcess, dllPath.string())) {
        std::cerr
            << "[Launcher] [ERROR] FrontierMP Core injection failed."
            << std::endl;
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::cout << "Press Enter to exit..." << std::endl;
        std::cin.get();
        return 1;
    }

    std::cout << "[Launcher] [SUCCESS] FrontierMP Core staged in client directory."
              << std::endl;
    std::cout << "[Launcher] Loading ScriptHookRDR after FrontierMP..."
              << std::endl;

    if (!injectDll(pi.hProcess, scriptHook.scriptHookPath.string())) {
        std::cerr
            << "[Launcher] [ERROR] Could not load ScriptHookRDR from the Frontier client."
            << std::endl;
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::cout << "Press Enter to exit..." << std::endl;
        std::cin.get();
        return 1;
    }

    std::cout << "[Launcher] [SUCCESS] ScriptHookRDR loaded from client directory."
              << std::endl;

    // Reanudar la ejecución del juego
    std::cout << "[Launcher] Resuming game execution..." << std::endl;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#endif

    std::cout << "\n[Launcher] Ready! Check the 'FrontierMP - Client Console' window for live connection logs." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return 0;
}
