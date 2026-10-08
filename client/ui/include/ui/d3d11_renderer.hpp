#pragma once

#include <windows.h>
#include <d3d12.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <dxgi1_4.h>
#include <string>
#include <vector>
#include <mutex>

namespace Frontier::UI {

struct ChatEntry {
    std::string text;
    float alpha{1.0f};
    uint64_t timestamp{0};
};

struct Vertex2D {
    float x, y;
    float u, v;
    uint32_t color; // 0xAABBGGRR
};

class D3D11Renderer {
public:
    static D3D11Renderer& get();

    bool initialize(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* pCommandQueue = nullptr);
    void render(IDXGISwapChain* pSwapChain);
    void shutdown();
    void onResize();

    bool handleInput(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void setMainMenuVisible(bool visible) { m_showMainMenu = visible; }
    bool isMainMenuVisible() const { return m_showMainMenu; }

    void setChatVisible(bool visible) { m_showChat = visible; }
    bool isChatVisible() const { return m_showChat; }

    void addChatMessage(const std::string& msg);
    void triggerConnect();

    // Visual equivalent of RDRMP's CEF "set_loading_screen" message.
    void setLoadingScreenVisible(bool visible, const std::string& message);
    bool isLoadingScreenVisible() const;

private:
    D3D11Renderer() = default;
    ~D3D11Renderer() = default;

    bool initPipeline();
    void beginDraw();
    void endDraw();
    void flushVertices();

    void drawRect(float x, float y, float w, float h, uint32_t color);
    void drawBorder(float x, float y, float w, float h, float thickness, uint32_t color);
    void drawPanel(float x, float y, float w, float h, uint32_t bgColor, uint32_t borderColor, float borderThick = 2.0f);
    void drawText(float x, float y, const std::string& text, uint32_t color, float scale = 1.0f);
    void drawCursor();
    void drawMainMenu();
    void drawChat();

    // D3D12 & D3D11On12 objects
    bool m_isD3D12{false};
    ID3D12Device* m_pD3D12Device{nullptr};
    ID3D12CommandQueue* m_pCommandQueue{nullptr};
    ID3D11On12Device* m_pD3D11On12Device{nullptr};
    std::vector<ID3D11Resource*> m_wrappedBuffers;
    std::vector<ID3D11RenderTargetView*> m_renderTargetViews;

    // D3D11 objects
    ID3D11Device* m_pDevice{nullptr};
    ID3D11DeviceContext* m_pContext{nullptr};
    ID3D11RenderTargetView* m_pD3D11RTV{nullptr};

    // Shaders & Resources
    ID3D11VertexShader* m_pVertexShader{nullptr};
    ID3D11PixelShader* m_pPixelShader{nullptr};
    ID3D11InputLayout* m_pInputLayout{nullptr};
    ID3D11Buffer* m_pVertexBuffer{nullptr};
    ID3D11Buffer* m_pConstantBuffer{nullptr};
    ID3D11BlendState* m_pBlendState{nullptr};
    ID3D11RasterizerState* m_pRasterizerState{nullptr};
    ID3D11SamplerState* m_pSamplerState{nullptr};
    ID3D11Texture2D* m_pWhiteTex{nullptr};
    ID3D11ShaderResourceView* m_pWhiteSRV{nullptr};
    ID3D11Texture2D* m_pFontTex{nullptr};
    ID3D11ShaderResourceView* m_pFontSRV{nullptr};

    bool m_initialized{false};
    bool m_showMainMenu{true};
    bool m_showChat{false};
    bool m_chatInputActive{false};
    uint32_t m_width{1920};
    uint32_t m_height{1080};
    uint32_t m_bufferCount{2};

    // Batching
    std::vector<Vertex2D> m_vertices;
    ID3D11ShaderResourceView* m_pCurrentSRV{nullptr};

    // UI state
    std::string m_playerName{"Outlaw_Alex"};
    std::string m_serverIp{"127.0.0.1"};
    std::string m_serverPort{"4674"};
    std::string m_chatInputBuffer;
    int m_activeTab{0};   // 0 = Conectar, 1 = Servidores, 2 = Ajustes
    int m_activeField{1}; // 1 = name, 2 = ip, 3 = port

    POINT m_mousePos{0, 0};
    bool m_mouseLDown{false};
    uint32_t m_caretTimer{0};

    std::vector<ChatEntry> m_chatMessages;

    mutable std::mutex m_loadingMutex;
    bool m_loadingScreenVisible{false};
    std::string m_loadingMessage;
};

} // namespace Frontier::UI
