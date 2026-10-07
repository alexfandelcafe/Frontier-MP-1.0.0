#pragma once

#include <windows.h>
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <string>
#include <atomic>

namespace Frontier::Core {

class EngineHooks {
public:
    static bool initialize();
    static void shutdown();

    // Estado del juego
    static bool isSingleplayerBlocked();
    static void setSingleplayerBlocked(bool blocked);
    static void requestMultiplayerWorldLoad();

    // Callbacks del ciclo de vida
    static void onScriptTick();
    static void onGameFrame();

    // Procedimiento de ventana
    static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    // DirectX SwapChain & Factory Hooks
    static HRESULT WINAPI HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
    static HRESULT WINAPI HookedResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
    static HRESULT WINAPI HookedCreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);
    static HRESULT WINAPI HookedCreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain);

    static HWND getGameWindow() { return s_gameHwnd; }
    static void setupSandboxWorld();

private:
    static bool hookGraphics();
    static bool hookScriptThread();

    static inline HWND s_gameHwnd{NULL};
    static inline WNDPROC s_originalWndProc{nullptr};
    static inline ID3D12CommandQueue* s_pCommandQueue{nullptr};

    static inline bool s_blockSingleplayer{true};
    static inline bool s_worldCleaned{false};
    static inline bool s_d3dInitialized{false};
    static inline std::atomic<bool> s_multiplayerWorldRequested{false};
    static inline std::atomic<bool> s_multiplayerPreparationStarted{false};
    static inline std::atomic<bool> s_multiplayerTransitionStarted{false};
};

} // namespace Frontier::Core
