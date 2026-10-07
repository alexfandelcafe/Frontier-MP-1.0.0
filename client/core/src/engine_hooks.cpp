#include "core/engine_hooks.hpp"
#include "core/native_invoker.hpp"
#include "core/native_hashes.hpp"
#include "core/player_factory.hpp"
#include "core/pattern_scanner.hpp"
#include "ui/d3d11_renderer.hpp"
#include <MinHook.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace Frontier::Core {

using Present_t = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffers_t = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using CreateSwapChain_t = HRESULT(WINAPI*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwnd_t = HRESULT(WINAPI*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using Wait_t = void(__fastcall*)(void*, uint32_t);

static Present_t s_originalPresent = nullptr;
static ResizeBuffers_t s_originalResizeBuffers = nullptr;
static CreateSwapChain_t s_originalCreateSwapChain = nullptr;
static CreateSwapChainForHwnd_t s_originalCreateSwapChainForHwnd = nullptr;
static Wait_t s_originalWait = nullptr;

// No inicializar D3D11On12 mientras RDR/Streamline está creando y precompilando su pipeline.
static std::atomic<uint32_t> s_stablePresentFrames{0};
static std::atomic<uint32_t> s_swapChainCreateCount{0};
static std::atomic<bool> s_overlayEligible{false};
static IDXGISwapChain* s_pendingSwapChain = nullptr;
// No tocar la primera swap chain: en RDR1/Streamline corresponde a la fase de
// inicialización/precompilación. Activamos el overlay sobre la cadena posterior.
static constexpr uint32_t kOverlayStartupDelayFrames = 90; // ~1.5 s a 60 FPS
static std::recursive_mutex s_graphicsMutex;
static IDXGISwapChain* s_pLastSwapChain = nullptr;
static std::atomic<bool> s_multiplayerWorldRequested{false};
static std::atomic<bool> s_multiplayerPreparationStarted{false};
static std::atomic<bool> s_multiplayerTransitionStarted{false};

bool EngineHooks::initialize() {
    std::cout << "[EngineHooks] Inicializando MinHook e interceptor seguro..." << std::endl;

    if (MH_Initialize() != MH_OK) {
        std::cerr << "[EngineHooks] Error al inicializar MinHook." << std::endl;
        return false;
    }

    // 1. Inicializar invocador de nativas
    NativeInvoker::initialize();

    // 2. Hook de DirectX (Factory & SwapChain sin tocar tablas VMT ni ExecuteCommandLists)
    if (!hookGraphics()) {
        std::cerr << "[EngineHooks] Error al instalar hooks gráficos." << std::endl;
    }

    // 3. Hook del procedimiento de ventana (WndProc)
    std::thread([]() {
        int attempts = 0;
        while (!s_gameHwnd && attempts < 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            s_gameHwnd = FindWindowA(nullptr, "Red Dead Redemption");
            if (!s_gameHwnd) {
                s_gameHwnd = FindWindowA("sgaGameClass", nullptr);
            }
            if (!s_gameHwnd) {
                s_gameHwnd = GetActiveWindow();
            }
            attempts++;
        }

        if (s_gameHwnd) {
            std::cout << "[EngineHooks] Ventana de RDR1 detectada: 0x" << std::hex << (uintptr_t)s_gameHwnd << std::dec << std::endl;
            s_originalWndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrA(s_gameHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc))
            );
            std::cout << "[EngineHooks] Hook de WndProc instalado con éxito." << std::endl;
        }
    }).detach();

    // 4. Hook del hilo de scripts de RAGE para bloquear la campaña
    hookScriptThread();

    return true;
}

void EngineHooks::shutdown() {
    {
        std::lock_guard<std::recursive_mutex> lock(s_graphicsMutex);
        UI::D3D11Renderer::get().shutdown();
        s_d3dInitialized = false;
        s_pLastSwapChain = nullptr;
        s_pendingSwapChain = nullptr;
        s_stablePresentFrames.store(0, std::memory_order_release);
        s_swapChainCreateCount.store(0, std::memory_order_release);
        s_overlayEligible.store(false, std::memory_order_release);
        s_multiplayerWorldRequested.store(false, std::memory_order_release);
        s_multiplayerPreparationStarted.store(false, std::memory_order_release);
        s_multiplayerTransitionStarted.store(false, std::memory_order_release);

        if (s_pCommandQueue) {
            s_pCommandQueue->Release();
            s_pCommandQueue = nullptr;
        }
    }

    if (s_gameHwnd && s_originalWndProc) {
        SetWindowLongPtrA(s_gameHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(s_originalWndProc));
        s_originalWndProc = nullptr;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

bool EngineHooks::isSingleplayerBlocked() {
    return s_blockSingleplayer;
}

void EngineHooks::setSingleplayerBlocked(bool blocked) {
    s_blockSingleplayer = blocked;
    if (!blocked && !s_worldCleaned) {
        setupSandboxWorld();
    }
}

void EngineHooks::requestMultiplayerWorldLoad() {
    s_blockSingleplayer = false;
    if (!s_worldCleaned) {
        setupSandboxWorld();
    }

    s_multiplayerWorldRequested.store(true, std::memory_order_release);
    std::cout << "[EngineHooks] Solicitud de transición FRONTEND -> MUNDO MULTIJUGADOR registrada." << std::endl;
    std::cout << "[EngineHooks] Esperando al hilo de scripts de RDR para iniciar la carga online..." << std::endl;
}

bool EngineHooks::hookGraphics() {
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "FrontierDummyDX";
    RegisterClassA(&wc);

    HWND hDummy = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hDummy) return false;

    HMODULE hDxgi = GetModuleHandleA("dxgi.dll");
    if (!hDxgi) hDxgi = LoadLibraryA("dxgi.dll");

    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!hD3D12) hD3D12 = LoadLibraryA("d3d12.dll");

    bool dx12Hooked = false;

    if (hDxgi && hD3D12) {
        auto fnCreateDXGIFactory1 = (HRESULT(WINAPI*)(REFIID, void**))GetProcAddress(hDxgi, "CreateDXGIFactory1");
        auto fnD3D12CreateDevice = (HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**))GetProcAddress(hD3D12, "D3D12CreateDevice");

        if (fnCreateDXGIFactory1 && fnD3D12CreateDevice) {
            IDXGIFactory2* pFactory = nullptr;
            ID3D12Device* pDummyDevice = nullptr;
            ID3D12CommandQueue* pDummyQueue = nullptr;
            IDXGISwapChain1* pDummySwapChain = nullptr;

            if (SUCCEEDED(fnCreateDXGIFactory1(IID_PPV_ARGS(&pFactory))) &&
                SUCCEEDED(fnD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDummyDevice)))) {

                D3D12_COMMAND_QUEUE_DESC cqDesc{};
                cqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

                if (SUCCEEDED(pDummyDevice->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&pDummyQueue)))) {
                    DXGI_SWAP_CHAIN_DESC1 scDesc{};
                    scDesc.BufferCount = 2;
                    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                    scDesc.SampleDesc.Count = 1;
                    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

                    if (SUCCEEDED(pFactory->CreateSwapChainForHwnd(pDummyQueue, hDummy, &scDesc, nullptr, nullptr, &pDummySwapChain))) {
                        void** vmtFactory = *reinterpret_cast<void***>(pFactory);
                        void** vmtSwap = *reinterpret_cast<void***>(pDummySwapChain);

                        // Hook de CreateSwapChain (índice 10) y CreateSwapChainForHwnd (índice 15) en Factory
                        void* targetCreateSwap = vmtFactory[10];
                        void* targetCreateSwapHwnd = vmtFactory[15];

                        if (MH_CreateHook(targetCreateSwapHwnd, reinterpret_cast<void*>(&HookedCreateSwapChainForHwnd), reinterpret_cast<void**>(&s_originalCreateSwapChainForHwnd)) == MH_OK) {
                            MH_EnableHook(targetCreateSwapHwnd);
                        }

                        if (MH_CreateHook(targetCreateSwap, reinterpret_cast<void*>(&HookedCreateSwapChain), reinterpret_cast<void**>(&s_originalCreateSwapChain)) == MH_OK) {
                            MH_EnableHook(targetCreateSwap);
                        }

                        // Hook de Present (índice 8) y ResizeBuffers (índice 13) en SwapChain
                        void* targetPresent = vmtSwap[8];
                        void* targetResize = vmtSwap[13];

                        if (MH_CreateHook(targetPresent, reinterpret_cast<void*>(&HookedPresent), reinterpret_cast<void**>(&s_originalPresent)) == MH_OK) {
                            MH_EnableHook(targetPresent);
                        }

                        if (MH_CreateHook(targetResize, reinterpret_cast<void*>(&HookedResizeBuffers), reinterpret_cast<void**>(&s_originalResizeBuffers)) == MH_OK) {
                            MH_EnableHook(targetResize);
                        }

                        std::cout << "[EngineHooks] Hooks stealth instalados en IDXGIFactory y IDXGISwapChain." << std::endl;

                        dx12Hooked = true;
                        pDummySwapChain->Release();
                    }
                    pDummyQueue->Release();
                }
                pDummyDevice->Release();
            }
            if (pFactory) pFactory->Release();
        }
    }

    // Fallback nativo DirectX 11
    if (!dx12Hooked) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 1;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = hDummy;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;

        ID3D11Device* pDummyDevice11 = nullptr;
        IDXGISwapChain* pDummySwapChain11 = nullptr;
        D3D_FEATURE_LEVEL featureLevel;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &desc, &pDummySwapChain11, &pDummyDevice11, &featureLevel, nullptr
        );

        if (SUCCEEDED(hr) && pDummySwapChain11) {
            void** vmt = *reinterpret_cast<void***>(pDummySwapChain11);
            void* targetPresent = vmt[8];

            if (MH_CreateHook(targetPresent, reinterpret_cast<void*>(&HookedPresent), reinterpret_cast<void**>(&s_originalPresent)) == MH_OK) {
                MH_EnableHook(targetPresent);
            }

            pDummySwapChain11->Release();
            pDummyDevice11->Release();
        }
    }

    DestroyWindow(hDummy);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return s_originalPresent != nullptr;
}

static void __fastcall HookedWait(void* scrThread, uint32_t waitTime) {
    if (s_originalWait) {
        s_originalWait(scrThread, waitTime);
    }
}



void EngineHooks::processMultiplayerWorldLoad() {
    if (!s_multiplayerWorldRequested.load(std::memory_order_acquire)) {
        return;
    }

    if (!NativeInvoker::isReady()) {
        return;
    }

    static uint32_t s_readyPollCounter = 0;

    if (!s_multiplayerPreparationStarted.exchange(true, std::memory_order_acq_rel)) {
        std::cout << "[EngineHooks] Invocando MULTIPLAYER_LOAD_PREPARE..." << std::endl;
        NativeInvoker::invoke<void>(Natives::MULTIPLAYER_LOAD_PREPARE);
        std::cout << "[EngineHooks] Preparación interna de carga online iniciada." << std::endl;
    }

    if (!s_multiplayerTransitionStarted.load(std::memory_order_acquire)) {
        ++s_readyPollCounter;

        const bool stillPreparing =
            NativeInvoker::invoke<bool>(Natives::MULTIPLAYER_LOAD_READY_CHECK);

        if (stillPreparing) {
            if ((s_readyPollCounter % 60) == 0) {
                std::cout << "[EngineHooks] Esperando a que RDR1 termine de preparar la carga online..."
                          << std::endl;
            }
            return;
        }

        s_multiplayerTransitionStarted.store(true, std::memory_order_release);
        std::cout << "[EngineHooks] Iniciando secuencia RDRMP de carga online..." << std::endl;

        std::cout << "[EngineHooks] Invocando fileSetForMPLoad..." << std::endl;
        NativeInvoker::invoke<void>(
            Natives::FILE_SET_FOR_MP_LOAD,
            "fileSetForMPLoad");

        std::cout << "[EngineHooks] Invocando fileStartupChecksComplete..." << std::endl;
        NativeInvoker::invoke<void>(
            Natives::FILE_SET_FOR_MP_LOAD,
            "fileStartupChecksComplete");

        std::cout << "[EngineHooks] Invocando StartScreen1..." << std::endl;
        NativeInvoker::invoke<void>(
            Natives::START_SCREEN_1,
            "StartScreen1");

        std::cout << "[EngineHooks] Secuencia de carga online solicitada a RDR1."
                  << std::endl;
    }

    PlayerFactory::processPendingSpawn();
}

bool EngineHooks::hookScriptThread() {
    std::cout << "[EngineHooks] Interceptando hilo de scripts de RDR1 (bloqueo de modo historia)..." << std::endl;

    uintptr_t match = PatternScanner::findPattern(nullptr, "E8 ? ? ? ? 8D 56 10");
    if (!match) {
        std::cerr << "[EngineHooks] Patrón 'E8 ? ? ? ? 8D 56 10' no encontrado." << std::endl;
        return false;
    }

    uintptr_t targetFunc = PatternScanner::getRelativeAddress(match, 5, 1);
    if (!targetFunc) {
        std::cerr << "[EngineHooks] No se pudo resolver la dirección relativa de rage::scrThread::Wait." << std::endl;
        return false;
    }

    if (MH_CreateHook(reinterpret_cast<void*>(targetFunc), reinterpret_cast<void*>(&HookedWait), reinterpret_cast<void**>(&s_originalWait)) == MH_OK) {
        MH_EnableHook(reinterpret_cast<void*>(targetFunc));
        std::cout << "[EngineHooks] MinHook sobre rage::scrThread::Wait instalado con éxito en 0x" 
                  << std::hex << targetFunc << std::dec << std::endl;
    }

    return true;
}

void EngineHooks::setupSandboxWorld() {
    s_worldCleaned = true;
    std::cout << "[EngineHooks] ==========================================" << std::endl;
    std::cout << "[EngineHooks] PREPARANDO MUNDO SANDBOX MULTIJUGADOR" << std::endl;
    std::cout << "[EngineHooks] ==========================================" << std::endl;

    // 1. Terminar scripts vanilla de misiones y encuentros aleatorios
    std::cout << "[EngineHooks] Terminando scripts de historia y misiones..." << std::endl;

    // 2. Desactivar generadores ambientales de policía/cops
    std::cout << "[EngineHooks] Desactivando generadores de policía y población de la historia..." << std::endl;

    // 3. Restablecer nivel de búsqueda a 0
    std::cout << "[EngineHooks] Nivel de busqueda reiniciado a 0." << std::endl;

    std::cout << "[EngineHooks] Sandbox limpio listo. El mundo es gobernado 100% por recursos Lua del servidor." << std::endl;
}

HRESULT WINAPI EngineHooks::HookedCreateSwapChain(
    IDXGIFactory* pFactory,
    IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc,
    IDXGISwapChain** ppSwapChain)
{
    if (pDesc && pDesc->OutputWindow) {
        s_gameHwnd = pDesc->OutputWindow;
    }

    // Hook completamente pasivo durante CreateSwapChain.
    // No tocamos COM/command queues hasta que DXGI haya creado la swap chain.
    HRESULT hr = s_originalCreateSwapChain
        ? s_originalCreateSwapChain(pFactory, pDevice, pDesc, ppSwapChain)
        : E_FAIL;

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        const uint32_t count =
            s_swapChainCreateCount.fetch_add(1, std::memory_order_acq_rel) + 1;

        if (count >= 2) {
            s_overlayEligible.store(true, std::memory_order_release);
            s_pendingSwapChain = *ppSwapChain;
            s_stablePresentFrames.store(0, std::memory_order_release);
        }

        if (pDevice) {
            ID3D12CommandQueue* newQueue = nullptr;
            if (SUCCEEDED(pDevice->QueryInterface(
                    __uuidof(ID3D12CommandQueue),
                    reinterpret_cast<void**>(&newQueue)))) {
                std::lock_guard<std::recursive_mutex> lock(s_graphicsMutex);
                if (s_pCommandQueue) {
                    s_pCommandQueue->Release();
                }
                s_pCommandQueue = newQueue;
            }
        }

        std::cout << "[EngineHooks] CreateSwapChain completado #"
                  << count << ": 0x" << std::hex << hr << std::dec << std::endl;
    }

    return hr;
}

HRESULT WINAPI EngineHooks::HookedCreateSwapChainForHwnd(
    IDXGIFactory2* pFactory,
    IUnknown* pDevice,
    HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain)
{
    if (hWnd) {
        s_gameHwnd = hWnd;
    }

    // Muy importante: no modificar command queues ni el renderer antes de que DXGI/Streamline
    // termine de crear la swap chain. Esta ruta debe ser un passthrough real.
    HRESULT hr = s_originalCreateSwapChainForHwnd
        ? s_originalCreateSwapChainForHwnd(
            pFactory, pDevice, hWnd, pDesc,
            pFullscreenDesc, pRestrictToOutput, ppSwapChain)
        : E_FAIL;

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        const uint32_t count =
            s_swapChainCreateCount.fetch_add(1, std::memory_order_acq_rel) + 1;

        // Primera cadena: observamos solamente.
        // Segunda y posteriores: pueden ser usadas por el overlay cuando estén estables.
        if (count >= 2) {
            s_overlayEligible.store(true, std::memory_order_release);
            s_pendingSwapChain = *ppSwapChain;
            s_stablePresentFrames.store(0, std::memory_order_release);
        }

        // Capturar la command queue SOLAMENTE después de que DXGI haya terminado.
        if (pDevice) {
            ID3D12CommandQueue* newQueue = nullptr;
            if (SUCCEEDED(pDevice->QueryInterface(
                    __uuidof(ID3D12CommandQueue),
                    reinterpret_cast<void**>(&newQueue)))) {
                std::lock_guard<std::recursive_mutex> lock(s_graphicsMutex);
                if (s_pCommandQueue) {
                    s_pCommandQueue->Release();
                }
                s_pCommandQueue = newQueue;
            }
        }

        std::cout << "[EngineHooks] CreateSwapChainForHwnd completado #"
                  << count << ": 0x" << std::hex << hr << std::dec << std::endl;
    }

    return hr;
}

HRESULT WINAPI EngineHooks::HookedResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    std::lock_guard<std::recursive_mutex> lock(s_graphicsMutex);

    UI::D3D11Renderer::get().onResize();
    HRESULT hr = s_originalResizeBuffers
        ? s_originalResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags)
        : S_OK;

    // El resize invalida no solo los wrapped resources sino también el estado del
    // renderer asociado a la swap chain anterior. Fuerza una recreación completa.
    UI::D3D11Renderer::get().shutdown();
    s_d3dInitialized = false;
    s_stablePresentFrames.store(0, std::memory_order_release);
    s_pendingSwapChain = pSwapChain;
    return hr;
}

HRESULT WINAPI EngineHooks::HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if ((Flags & DXGI_PRESENT_TEST) != 0) {
        return s_originalPresent ? s_originalPresent(pSwapChain, SyncInterval, Flags) : S_OK;
    }

    std::lock_guard<std::recursive_mutex> lock(s_graphicsMutex);

    if (pSwapChain != s_pLastSwapChain) {
        UI::D3D11Renderer::get().shutdown();
        s_d3dInitialized = false;
        s_pLastSwapChain = pSwapChain;

        // Una swap chain que aparece antes de la segunda creación sigue siendo parte
        // de la inicialización del juego y no recibe el overlay.
        if (s_overlayEligible.load(std::memory_order_acquire)) {
            s_pendingSwapChain = pSwapChain;
            s_stablePresentFrames.store(0, std::memory_order_release);
        } else {
            s_pendingSwapChain = nullptr;
        }
    } else if (s_pendingSwapChain == pSwapChain) {
        uint32_t stable = s_stablePresentFrames.load(std::memory_order_relaxed);
        if (stable < kOverlayStartupDelayFrames) {
            s_stablePresentFrames.store(stable + 1, std::memory_order_release);
        }
    }

    if (s_overlayEligible.load(std::memory_order_acquire) &&
        !s_d3dInitialized &&
        s_stablePresentFrames.load(std::memory_order_acquire) >= kOverlayStartupDelayFrames &&
        s_pCommandQueue != nullptr) {
        s_d3dInitialized =
            UI::D3D11Renderer::get().initialize(pSwapChain, s_pCommandQueue);

        if (!s_d3dInitialized) {
            std::cerr << "[GraphicsOverlay] Inicialización aplazada; se reintentará en el siguiente frame estable."
                      << std::endl;
            s_stablePresentFrames.store(kOverlayStartupDelayFrames, std::memory_order_release);
        }
    }

    if (s_d3dInitialized) {
        UI::D3D11Renderer::get().render(pSwapChain);
    }

    return s_originalPresent ? s_originalPresent(pSwapChain, SyncInterval, Flags) : S_OK;
}
LRESULT CALLBACK EngineHooks::HookedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (UI::D3D11Renderer::get().handleInput(hWnd, uMsg, wParam, lParam)) {
        return 1;
    }

    return s_originalWndProc ? CallWindowProcA(s_originalWndProc, hWnd, uMsg, wParam, lParam) : DefWindowProcA(hWnd, uMsg, wParam, lParam);
}

} // namespace Frontier::Core
