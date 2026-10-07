#include <windows.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>
#include "core/player_factory.hpp"
#include "core/pattern_scanner.hpp"
#include "core/native_invoker.hpp"
#include "core/engine_hooks.hpp"
#include "core/script_bridge.hpp"
#include "net/client_net.hpp"
#include "ui/cef_manager.hpp"
#include "ui/d3d11_renderer.hpp"

namespace fs = std::filesystem;

static fs::path getModDirectory(HMODULE hModule) {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(hModule, path, MAX_PATH);
    return fs::path(path).parent_path();
}

static void appendBootLog(const fs::path& modDir, const std::string& message) {
    std::ofstream log(modDir / "frontier_core_boot.log", std::ios::app);
    if (log.is_open()) {
        log << message << std::endl;
    }
}

struct ClientConfig {
    std::string playerName{"Outlaw_Player"};
    std::string serverIp{"127.0.0.1"};
    uint16_t serverPort{4674};
    bool autoConnect{true};
    bool showConsole{true};
};

static ClientConfig loadClientConfig(const fs::path& modDir) {
    ClientConfig cfg;
    fs::path configPath = modDir / "settings.json";

    std::ifstream f(configPath);
    if (!f.is_open()) {
        configPath = modDir.parent_path() / "settings.json";
        f.open(configPath);
    }

    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("\"player_name\"") != std::string::npos) {
                size_t q1 = line.find(':', line.find("\"player_name\""));
                size_t s1 = line.find('\"', q1);
                size_t s2 = line.find('\"', s1 + 1);
                if (s1 != std::string::npos && s2 != std::string::npos) {
                    cfg.playerName = line.substr(s1 + 1, s2 - s1 - 1);
                }
            } else if (line.find("\"server_ip\"") != std::string::npos) {
                size_t q1 = line.find(':', line.find("\"server_ip\""));
                size_t s1 = line.find('\"', q1);
                size_t s2 = line.find('\"', s1 + 1);
                if (s1 != std::string::npos && s2 != std::string::npos) {
                    cfg.serverIp = line.substr(s1 + 1, s2 - s1 - 1);
                }
            } else if (line.find("\"server_port\"") != std::string::npos) {
                size_t q1 = line.find(':', line.find("\"server_port\""));
                if (q1 != std::string::npos) {
                    cfg.serverPort = static_cast<uint16_t>(std::stoi(line.substr(q1 + 1)));
                }
            } else if (line.find("\"auto_connect\"") != std::string::npos) {
                if (line.find("false") != std::string::npos) {
                    cfg.autoConnect = false;
                }
            }
        }
    }
    return cfg;
}

DWORD WINAPI FrontierMainThread(LPVOID lpParam) {
    HMODULE hModule = static_cast<HMODULE>(lpParam);
    fs::path modDir = getModDirectory(hModule);
    appendBootLog(modDir, "[FrontierClient] FrontierMainThread started.");

    ClientConfig cfg = loadClientConfig(modDir);
    appendBootLog(modDir, "[FrontierClient] Settings loaded.");

    // 1. Abrir consola de depuración interactiva
    if (cfg.showConsole) {
        if (!GetConsoleWindow()) {
            if (!AllocConsole()) {
                appendBootLog(modDir, "[FrontierClient] AllocConsole failed: " + std::to_string(GetLastError()));
            } else {
                appendBootLog(modDir, "[FrontierClient] AllocConsole succeeded.");
            }
        } else {
            appendBootLog(modDir, "[FrontierClient] Existing console detected.");
        }
        FILE* fDummy = nullptr;
        freopen_s(&fDummy, "CONOUT$", "w", stdout);
        freopen_s(&fDummy, "CONOUT$", "w", stderr);
        freopen_s(&fDummy, "CONIN$", "r", stdin);
        SetConsoleTitleA("FrontierMP - Client Console (RDR1 PC)");
    }

    std::cout << "==========================================================" << std::endl;
    std::cout << "         FrontierMP Client v1.0.0 (RDR1 PC)               " << std::endl;
    std::cout << "         Injected successfully into RDR.exe!              " << std::endl;
    std::cout << "==========================================================" << std::endl;
    std::cout << "[FrontierClient] Mod location: " << modDir.string() << std::endl;
    std::cout << "[FrontierClient] Player: " << cfg.playerName << std::endl;
    std::cout << "[FrontierClient] Target Server: " << cfg.serverIp << ":" << cfg.serverPort << std::endl;

    // ScriptHookRDR was preloaded by the launcher. Frontier registered its
    // ScriptMain during DLL_PROCESS_ATTACH, matching the ScriptHook SDK lifecycle.
    Frontier::Core::ScriptBridge::initialize(hModule);
    appendBootLog(modDir,
        "[ScriptBridge] Early registration requested from DLL_PROCESS_ATTACH.");

    // 2. Inicializar hooks de DirectX 12 / DirectX 11, WndProc y bloqueo de campaña
    std::cout << "[FrontierClient] Installing DirectX 12 / DirectX 11 overlay hooks and script interceptor..." << std::endl;
    const bool engineHooksReady = Frontier::Core::EngineHooks::initialize();
    appendBootLog(modDir, engineHooksReady
        ? "[EngineHooks] initialize returned TRUE."
        : "[EngineHooks] initialize returned FALSE.");

    if (!Frontier::Core::ScriptBridge::isRegistered()) {
        std::cout
            << "[ScriptBridge] ScriptMain todavía no ha iniciado; "
               "ScriptBridge esperará a que ScriptHookRDR termine su inicialización."
            << std::endl;
    }

    // 3. Inicializar subsistemas del cliente
    std::cout << "[FrontierClient] Initializing PlayerFactory and Native Invoker..." << std::endl;
    Frontier::Core::PlayerFactory::initialize();

    // 4. Inicializar interfaz gráfica (FiveM Menu & CEF)
    std::cout << "[FrontierClient] Activating FrontierMP In-Game GUI Overlay (D3D11 / CEF)..." << std::endl;
    Frontier::UI::D3D11Renderer::get().setMainMenuVisible(true);

    if (cfg.autoConnect) {
        std::cout << "[FrontierClient] Ready. Press Enter in menu or click 'Conectar' to join server." << std::endl;
    }

    // 5. Bucle de actualización del cliente
    bool worldLoadFallbackLogged = false;

    while (true) {
        Frontier::Net::ClientNetwork::get().update();

        // The script is registered exactly once from DLL_PROCESS_ATTACH.
        // This persistent thread does not retry registration and never invokes
        // natives outside ScriptHookRDR's script scheduler.
        if (!worldLoadFallbackLogged &&
            !Frontier::Core::ScriptBridge::isRegistered() &&
            Frontier::Core::EngineHooks::isSingleplayerBlocked() == false) {
            std::cout
                << "[FrontierClient] ScriptMain de ScriptHookRDR no inició; "
                   "la transición multiplayer usará el contexto rage::scrThread::Wait "
                   "cuando el servidor solicite cargar el mundo."
                << std::endl;
            worldLoadFallbackLogged = true;
        }

        Frontier::UI::CefManager::get().update();
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }

    return 0;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        // ScriptHookRDR is loaded first by the launcher while the game is
        // suspended. Register Frontier immediately during module attach.
        Frontier::Core::ScriptBridge::registerScriptEarly(hModule);

        DisableThreadLibraryCalls(hModule);
        HANDLE hThread = CreateThread(nullptr, 0,
            (LPTHREAD_START_ROUTINE)FrontierMainThread, hModule, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);
            OutputDebugStringA(
                "[FrontierClient] FrontierMainThread created.\n");
        } else {
            OutputDebugStringA(
                "[FrontierClient] CreateThread failed in DllMain.\n");
        }
    }
    return TRUE;
}
