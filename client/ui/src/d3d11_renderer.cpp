#include "ui/d3d11_renderer.hpp"
#include "net/client_net.hpp"
#include "core/player_factory.hpp"
#include "core/engine_hooks.hpp"
#include <d3dcompiler.h>
#include <iostream>
#include <chrono>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace Frontier::UI {

// Fuente de mapa de bits 8x8 compacta integrada (ASCII 32..126)
static const uint8_t s_font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32 ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 33 '!'
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, // 34 '"'
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // 35 '#'
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, // 36 '$'
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // 37 '%'
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // 38 '&'
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // 39 '''
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // 40 '('
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // 41 ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 42 '*'
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // 43 '+'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // 44 ','
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // 45 '-'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // 46 '.'
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // 47 '/'
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00}, // 48 '0'
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // 49 '1'
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00}, // 50 '2'
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, // 51 '3'
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, // 52 '4'
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, // 53 '5'
    {0x7C,0xC6,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, // 54 '6'
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, // 55 '7'
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, // 56 '8'
    {0x7C,0xC6,0xC6,0x7E,0x06,0xC6,0x7C,0x00}, // 57 '9'
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, // 58 ':'
    {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00}, // 59 ';'
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, // 60 '<'
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // 61 '='
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, // 62 '>'
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, // 63 '?'
    {0x7C,0xC6,0xDE,0xDE,0xDC,0xC0,0x7C,0x00}, // 64 '@'
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 65 'A'
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, // 66 'B'
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, // 67 'C'
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, // 68 'D'
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, // 69 'E'
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, // 70 'F'
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, // 71 'G'
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 72 'H'
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 73 'I'
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, // 74 'J'
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, // 75 'K'
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, // 76 'L'
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, // 77 'M'
    {0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0x00}, // 78 'N'
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 79 'O'
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, // 80 'P'
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xCC,0x7A,0x00}, // 81 'Q'
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, // 82 'R'
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, // 83 'S'
    {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00}, // 84 'T'
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 85 'U'
    {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 86 'V'
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, // 87 'W'
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, // 88 'X'
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, // 89 'Y'
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, // 90 'Z'
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // 91 '['
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, // 92 '\'
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // 93 ']'
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // 94 '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // 95 '_'
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // 96 '`'
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, // 97 'a'
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, // 98 'b'
    {0x00,0x00,0x78,0xCC,0xC0,0xCC,0x78,0x00}, // 99 'c'
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, // 100 'd'
    {0x00,0x00,0x78,0xCC,0xFC,0xC0,0x78,0x00}, // 101 'e'
    {0x38,0x6C,0x60,0xF0,0x60,0x60,0xF0,0x00}, // 102 'f'
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, // 103 'g'
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, // 104 'h'
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 105 'i'
    {0x0C,0x00,0x1C,0x0C,0x0C,0xCC,0xCC,0x78}, // 106 'j'
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, // 107 'k'
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 108 'l'
    {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00}, // 109 'm'
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, // 110 'n'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, // 111 'o'
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, // 112 'p'
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, // 113 'q'
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, // 114 'r'
    {0x00,0x00,0x7C,0xC0,0x78,0x0C,0xF8,0x00}, // 115 's'
    {0x10,0x30,0x7C,0x30,0x30,0x34,0x18,0x00}, // 116 't'
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, // 117 'u'
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 118 'v'
    {0x00,0x00,0xC6,0xD6,0xD6,0xFE,0x6C,0x00}, // 119 'w'
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, // 120 'x'
    {0x00,0x00,0xC6,0xC6,0xCC,0x78,0x30,0x60}, // 121 'y'
    {0x00,0x00,0xFE,0x8C,0x18,0x32,0xFE,0x00}, // 122 'z'
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // 123 '{'
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // 124 '|'
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // 125 '}'
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}  // 126 '~'
};

D3D11Renderer& D3D11Renderer::get() {
    static D3D11Renderer instance;
    return instance;
}

bool D3D11Renderer::initialize(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* pCommandQueue) {
    if (m_initialized) return true;
    if (!pSwapChain) return false;

    // 1. Probar si la SwapChain fue creada con DirectX 12
    ID3D12Device* pD3D12Device = nullptr;
    HRESULT hr12 = pSwapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&pD3D12Device));

    if (SUCCEEDED(hr12) && pD3D12Device) {
        if (!pCommandQueue) {
            pD3D12Device->Release();
            return false; // Esperar al siguiente frame hasta que ExecuteCommandLists intercepte la CommandQueue
        }

        m_pD3D12Device = pD3D12Device;

        ID3D12CommandQueue* queueRef = nullptr;
        HRESULT queueHr = pCommandQueue->QueryInterface(
            __uuidof(ID3D12CommandQueue),
            reinterpret_cast<void**>(&queueRef));
        if (FAILED(queueHr) || !queueRef) {
            std::cerr << "[GraphicsOverlay] El objeto no es una ID3D12CommandQueue válida: 0x"
                      << std::hex << queueHr << std::dec << std::endl;
            return false;
        }
        m_pCommandQueue = queueRef;

        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        IUnknown* queues[] = { static_cast<IUnknown*>(m_pCommandQueue) };
        HRESULT hr = D3D11On12CreateDevice(
            pD3D12Device,
            0,
            featureLevels,
            ARRAYSIZE(featureLevels),
            queues,
            ARRAYSIZE(queues),
            0,
            &m_pDevice,
            &m_pContext,
            nullptr
        );

        if (FAILED(hr) || !m_pDevice) {
            std::cerr << "[GraphicsOverlay] D3D11On12CreateDevice falló: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }

        hr = m_pDevice->QueryInterface(__uuidof(ID3D11On12Device), reinterpret_cast<void**>(&m_pD3D11On12Device));
        if (FAILED(hr) || !m_pD3D11On12Device) {
            std::cerr << "[GraphicsOverlay] Falló QueryInterface ID3D11On12Device." << std::endl;
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        hr = pSwapChain->GetDesc(&desc);
        if (FAILED(hr) || desc.BufferCount == 0 || desc.BufferCount > 8) {
            std::cerr << "[GraphicsOverlay] GetDesc/BufferCount inválido: hr=0x"
                      << std::hex << hr << std::dec
                      << " buffers=" << desc.BufferCount << std::endl;
            return false;
        }
        m_width = desc.BufferDesc.Width;
        m_height = desc.BufferDesc.Height;
        m_bufferCount = desc.BufferCount;

        m_wrappedBuffers.resize(m_bufferCount, nullptr);
        m_renderTargetViews.resize(m_bufferCount, nullptr);

        D3D11_RESOURCE_FLAGS d3d11Flags{};
        d3d11Flags.BindFlags = D3D11_BIND_RENDER_TARGET;

        for (UINT i = 0; i < m_bufferCount; ++i) {
            ID3D12Resource* pD3D12Buffer = nullptr;
            hr = pSwapChain->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&pD3D12Buffer));
            if (SUCCEEDED(hr) && pD3D12Buffer) {
                hr = m_pD3D11On12Device->CreateWrappedResource(
                    pD3D12Buffer,
                    &d3d11Flags,
                    D3D12_RESOURCE_STATE_PRESENT,
                    D3D12_RESOURCE_STATE_PRESENT,
                    __uuidof(ID3D11Resource),
                    reinterpret_cast<void**>(&m_wrappedBuffers[i])
                );

                if (SUCCEEDED(hr) && m_wrappedBuffers[i]) {
                    hr = m_pDevice->CreateRenderTargetView(
                        m_wrappedBuffers[i], nullptr, &m_renderTargetViews[i]);
                    if (FAILED(hr) || !m_renderTargetViews[i]) {
                        pD3D12Buffer->Release();
                        std::cerr << "[GraphicsOverlay] CreateRenderTargetView falló para backbuffer "
                                  << i << ": 0x" << std::hex << hr << std::dec << std::endl;
                        return false;
                    }
                } else {
                    pD3D12Buffer->Release();
                    std::cerr << "[GraphicsOverlay] CreateWrappedResource falló para backbuffer "
                              << i << ": 0x" << std::hex << hr << std::dec << std::endl;
                    return false;
                }
                pD3D12Buffer->Release();
            }
        }

        m_isD3D12 = true;
        std::cout << "[GraphicsOverlay] Overlay acelerado por hardware activo sobre DirectX 12 (D3D11On12)." << std::endl;
    } else {
        // Fallback nativo DirectX 11
        HRESULT hr11 = pSwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&m_pDevice));
        if (FAILED(hr11) || !m_pDevice) {
            return false;
        }

        m_pDevice->GetImmediateContext(&m_pContext);
        if (!m_pContext) {
            std::cerr << "[GraphicsOverlay] No se pudo obtener el ImmediateContext D3D11." << std::endl;
            return false;
        }

        ID3D11Texture2D* pBackBuffer = nullptr;
        HRESULT bufferHr = pSwapChain->GetBuffer(
            0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pBackBuffer));
        if (FAILED(bufferHr) || !pBackBuffer) {
            std::cerr << "[GraphicsOverlay] GetBuffer(0) D3D11 falló: 0x"
                      << std::hex << bufferHr << std::dec << std::endl;
            return false;
        }
        HRESULT rtvHr = m_pDevice->CreateRenderTargetView(
            pBackBuffer, nullptr, &m_pD3D11RTV);
        pBackBuffer->Release();
        if (FAILED(rtvHr) || !m_pD3D11RTV) {
            std::cerr << "[GraphicsOverlay] CreateRenderTargetView D3D11 falló: 0x"
                      << std::hex << rtvHr << std::dec << std::endl;
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        HRESULT descHr = pSwapChain->GetDesc(&desc);
        if (FAILED(descHr)) {
            std::cerr << "[GraphicsOverlay] GetDesc D3D11 falló: 0x"
                      << std::hex << descHr << std::dec << std::endl;
            return false;
        }
        m_width = desc.BufferDesc.Width;
        m_height = desc.BufferDesc.Height;
        m_isD3D12 = false;
        std::cout << "[GraphicsOverlay] Overlay acelerado por hardware activo sobre DirectX 11." << std::endl;
    }

    if (!initPipeline()) {
        std::cerr << "[GraphicsOverlay] Falló la creación del pipeline de renderizado 2D." << std::endl;
        shutdown();
        return false;
    }

    m_initialized = true;
    addChatMessage("^2[FrontierMP] Bienvenido al multijugador de Red Dead Redemption 1!");
    addChatMessage("^3[FrontierMP] Presiona F8 para alternar el menú | T para abrir el chat.");
    return true;
}

bool D3D11Renderer::initPipeline() {
    const char* shaderCode = R"(
        cbuffer ConstantBuffer : register(b0) {
            matrix projection;
        };
        struct VS_INPUT {
            float2 pos : POSITION;
            float2 uv  : TEXCOORD0;
            float4 col : COLOR0;
        };
        struct PS_INPUT {
            float4 pos : SV_POSITION;
            float2 uv  : TEXCOORD0;
            float4 col : COLOR0;
        };
        PS_INPUT VSMain(VS_INPUT input) {
            PS_INPUT output;
            output.pos = mul(projection, float4(input.pos, 0.0f, 1.0f));
            output.uv = input.uv;
            output.col = input.col;
            return output;
        }
        Texture2D tex : register(t0);
        SamplerState samp : register(s0);
        float4 PSMain(PS_INPUT input) : SV_TARGET {
            return input.col * tex.Sample(samp, input.uv);
        }
    )";

    auto checkHr = [](HRESULT hr, const char* operation) -> bool {
        if (FAILED(hr)) {
            std::cerr << "[D3D11Renderer] " << operation
                      << " failed: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }
        return true;
    };

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr,
                            "VSMain", "vs_4_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "[D3D11Renderer] VS Compile Error: "
                      << static_cast<const char*>(errorBlob->GetBufferPointer()) << std::endl;
            errorBlob->Release();
        }
        return false;
    }
    if (errorBlob) {
        errorBlob->Release();
        errorBlob = nullptr;
    }

    hr = D3DCompile(shaderCode, strlen(shaderCode), nullptr, nullptr, nullptr,
                    "PSMain", "ps_4_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "[D3D11Renderer] PS Compile Error: "
                      << static_cast<const char*>(errorBlob->GetBufferPointer()) << std::endl;
            errorBlob->Release();
        }
        vsBlob->Release();
        return false;
    }
    if (errorBlob) {
        errorBlob->Release();
        errorBlob = nullptr;
    }

    if (!checkHr(m_pDevice->CreateVertexShader(
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_pVertexShader),
            "CreateVertexShader")) {
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    if (!checkHr(m_pDevice->CreatePixelShader(
            psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pPixelShader),
            "CreatePixelShader")) {
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    if (!checkHr(m_pDevice->CreateInputLayout(
            layoutDesc, ARRAYSIZE(layoutDesc),
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_pInputLayout),
            "CreateInputLayout")) {
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    vsBlob->Release();
    psBlob->Release();

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(float) * 16;
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!checkHr(m_pDevice->CreateBuffer(&cbDesc, nullptr, &m_pConstantBuffer),
                 "CreateBuffer(ConstantBuffer)")) {
        return false;
    }

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(Vertex2D) * 16384;
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!checkHr(m_pDevice->CreateBuffer(&vbDesc, nullptr, &m_pVertexBuffer),
                 "CreateBuffer(VertexBuffer)")) {
        return false;
    }

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (!checkHr(m_pDevice->CreateBlendState(&blendDesc, &m_pBlendState),
                 "CreateBlendState")) {
        return false;
    }

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (!checkHr(m_pDevice->CreateSamplerState(&sampDesc, &m_pSamplerState),
                 "CreateSamplerState")) {
        return false;
    }

    D3D11_RASTERIZER_DESC rastDesc{};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    if (!checkHr(m_pDevice->CreateRasterizerState(&rastDesc, &m_pRasterizerState),
                 "CreateRasterizerState")) {
        return false;
    }

    const uint32_t whitePixel = 0xFFFFFFFF;
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = &whitePixel;
    initData.SysMemPitch = sizeof(whitePixel);

    if (!checkHr(m_pDevice->CreateTexture2D(&texDesc, &initData, &m_pWhiteTex),
                 "CreateTexture2D(White)")) {
        return false;
    }
    if (!checkHr(m_pDevice->CreateShaderResourceView(m_pWhiteTex, nullptr, &m_pWhiteSRV),
                 "CreateShaderResourceView(White)")) {
        return false;
    }

    std::vector<uint32_t> fontPixels(128 * 64, 0x00000000);
    for (int ch = 0; ch < 96; ++ch) {
        int cellX = (ch % 16) * 8;
        int cellY = (ch / 16) * 8;
        for (int row = 0; row < 8; ++row) {
            uint8_t byte = s_font8x8[ch][row];
            for (int col = 0; col < 8; ++col) {
                if (byte & (0x80 >> col)) {
                    fontPixels[(cellY + row) * 128 + (cellX + col)] = 0xFFFFFFFF;
                }
            }
        }
    }

    D3D11_TEXTURE2D_DESC fontDesc{};
    fontDesc.Width = 128;
    fontDesc.Height = 64;
    fontDesc.MipLevels = 1;
    fontDesc.ArraySize = 1;
    fontDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    fontDesc.SampleDesc.Count = 1;
    fontDesc.Usage = D3D11_USAGE_IMMUTABLE;
    fontDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA fontData{};
    fontData.pSysMem = fontPixels.data();
    fontData.SysMemPitch = 128 * sizeof(uint32_t);

    if (!checkHr(m_pDevice->CreateTexture2D(&fontDesc, &fontData, &m_pFontTex),
                 "CreateTexture2D(FontAtlas)")) {
        return false;
    }
    if (!checkHr(m_pDevice->CreateShaderResourceView(m_pFontTex, nullptr, &m_pFontSRV),
                 "CreateShaderResourceView(FontAtlas)")) {
        return false;
    }

    return true;
}

void D3D11Renderer::beginDraw() {
    m_vertices.clear();
    m_pCurrentSRV = nullptr;

    // Actualizar matriz ortográfica
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(m_pContext->Map(m_pConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        float left = 0.0f, right = (float)m_width, top = 0.0f, bottom = (float)m_height;
        float orthoMatrix[16] = {
            2.0f / (right - left), 0.0f, 0.0f, 0.0f,
            0.0f, 2.0f / (top - bottom), 0.0f, 0.0f,
            0.0f, 0.0f, 0.5f, 0.0f,
            (left + right) / (left - right), (top + bottom) / (bottom - top), 0.5f, 1.0f
        };
        memcpy(mapped.pData, orthoMatrix, sizeof(orthoMatrix));
        m_pContext->Unmap(m_pConstantBuffer, 0);
    }

    UINT stride = sizeof(Vertex2D);
    UINT offset = 0;
    m_pContext->IASetInputLayout(m_pInputLayout);
    m_pContext->IASetVertexBuffers(0, 1, &m_pVertexBuffer, &stride, &offset);
    m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_pContext->VSSetShader(m_pVertexShader, nullptr, 0);
    m_pContext->VSSetConstantBuffers(0, 1, &m_pConstantBuffer);

    m_pContext->PSSetShader(m_pPixelShader, nullptr, 0);
    m_pContext->PSSetSamplers(0, 1, &m_pSamplerState);

    float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_pContext->OMSetBlendState(m_pBlendState, blendFactor, 0xFFFFFFFF);
    m_pContext->RSSetState(m_pRasterizerState);
}

void D3D11Renderer::flushVertices() {
    if (m_vertices.empty() || !m_pCurrentSRV) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(m_pContext->Map(m_pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, m_vertices.data(), m_vertices.size() * sizeof(Vertex2D));
        m_pContext->Unmap(m_pVertexBuffer, 0);
    }

    m_pContext->PSSetShaderResources(0, 1, &m_pCurrentSRV);
    m_pContext->Draw(static_cast<UINT>(m_vertices.size()), 0);
    m_vertices.clear();
}

void D3D11Renderer::endDraw() {
    flushVertices();
}

void D3D11Renderer::drawRect(float x, float y, float w, float h, uint32_t color) {
    if (m_pCurrentSRV != m_pWhiteSRV) {
        flushVertices();
        m_pCurrentSRV = m_pWhiteSRV;
    }

    Vertex2D v[6] = {
        { x,     y,     0.0f, 0.0f, color },
        { x + w, y,     1.0f, 0.0f, color },
        { x,     y + h, 0.0f, 1.0f, color },
        { x + w, y,     1.0f, 0.0f, color },
        { x + w, y + h, 1.0f, 1.0f, color },
        { x,     y + h, 0.0f, 1.0f, color },
    };

    m_vertices.insert(m_vertices.end(), v, v + 6);
}

void D3D11Renderer::drawBorder(float x, float y, float w, float h, float thickness, uint32_t color) {
    drawRect(x, y, w, thickness, color);                 // Arriba
    drawRect(x, y + h - thickness, w, thickness, color); // Abajo
    drawRect(x, y, thickness, h, color);                 // Izquierda
    drawRect(x + w - thickness, y, thickness, h, color); // Derecha
}

void D3D11Renderer::drawPanel(float x, float y, float w, float h, uint32_t bgColor, uint32_t borderColor, float borderThick) {
    drawRect(x, y, w, h, bgColor);
    if (borderThick > 0.0f) {
        drawBorder(x, y, w, h, borderThick, borderColor);
    }
}

void D3D11Renderer::drawText(float x, float y, const std::string& text, uint32_t color, float scale) {
    if (text.empty()) return;

    if (m_pCurrentSRV != m_pFontSRV) {
        flushVertices();
        m_pCurrentSRV = m_pFontSRV;
    }

    float curX = x;
    float curY = y;
    float charW = 8.0f * scale;
    float charH = 8.0f * scale;

    uint32_t activeColor = color;

    for (size_t i = 0; i < text.size(); ++i) {
        // Soporte de códigos de color FiveM: ^1 (Rojo), ^2 (Verde), ^3 (Amarillo), ^4 (Azul), ^7 (Blanco)
        if (text[i] == '^' && i + 1 < text.size()) {
            char code = text[++i];
            if (code == '1') activeColor = 0xFF4040FF; // Rojo
            else if (code == '2') activeColor = 0xFF50FF50; // Verde
            else if (code == '3') activeColor = 0xFF50FFFF; // Amarillo
            else if (code == '4') activeColor = 0xFFFF8050; // Azul
            else if (code == '7') activeColor = 0xFFFFFFFF; // Blanco
            continue;
        }

        char c = text[i];
        if (c == '\n') {
            curX = x;
            curY += (charH + 2.0f);
            continue;
        }

        if (c < 32 || c > 126) c = '?';

        int chIdx = c - 32;
        float u0 = (chIdx % 16) * 8.0f / 128.0f;
        float v0 = (chIdx / 16) * 8.0f / 64.0f;
        float u1 = u0 + (8.0f / 128.0f);
        float v1 = v0 + (8.0f / 64.0f);

        Vertex2D v[6] = {
            { curX,         curY,         u0, v0, activeColor },
            { curX + charW, curY,         u1, v0, activeColor },
            { curX,         curY + charH, u0, v1, activeColor },
            { curX + charW, curY,         u1, v0, activeColor },
            { curX + charW, curY + charH, u1, v1, activeColor },
            { curX,         curY + charH, u0, v1, activeColor },
        };

        m_vertices.insert(m_vertices.end(), v, v + 6);
        curX += charW;
    }
}

void D3D11Renderer::drawCursor() {
    float cx = static_cast<float>(m_mousePos.x);
    float cy = static_cast<float>(m_mousePos.y);

    // Cursor estilizado en forma de flecha de puntero
    uint32_t cursorColor = 0xFFFFFFFF;
    uint32_t cursorBorder = 0xFF000000;

    drawRect(cx, cy, 3.0f, 16.0f, cursorBorder);
    drawRect(cx, cy, 16.0f, 3.0f, cursorBorder);
    drawRect(cx + 2.0f, cy + 2.0f, 10.0f, 10.0f, cursorColor);
    drawRect(cx + 4.0f, cy + 4.0f, 8.0f, 8.0f, 0xFFCC3333);
}

void D3D11Renderer::drawMainMenu() {
    m_caretTimer++;

    // Tinte oscuro de fondo cinemático sobre la pantalla del juego
    drawRect(0, 0, (float)m_width, (float)m_height, 0xD00A0A0A);

    // Ventana central del Menú Principal estilo RDR1 / FiveM
    float winW = 860.0f;
    float winH = 580.0f;
    float winX = ((float)m_width - winW) * 0.5f;
    float winY = ((float)m_height - winH) * 0.5f;

    // Fondo de la ventana con bordes dobles dorados/rojos
    drawPanel(winX, winY, winW, winH, 0xFA141414, 0xFF8A2424, 3.0f);
    drawBorder(winX + 4.0f, winY + 4.0f, winW - 8.0f, winH - 8.0f, 1.0f, 0xFF4A1818);

    // Barra de título superior
    drawPanel(winX + 6.0f, winY + 6.0f, winW - 12.0f, 75.0f, 0xFF201010, 0xFF6B1D1D, 2.0f);
    drawText(winX + 24.0f, winY + 16.0f, "FRONTIER MP", 0xFF2A8CE4, 2.4f); // Título dorado/brillante
    drawText(winX + 24.0f, winY + 50.0f, "RED DEAD REDEMPTION 1 MULTIPLAYER - FIVE-M ARCHITECTURE", 0xFFCCCCCC, 1.1f);

    // Pestañas (Tabs)
    float tabY = winY + 95.0f;
    float tabW = 260.0f;
    float tabH = 38.0f;

    // Tab 0: Conectar
    bool hoverTab0 = (m_mousePos.x >= winX + 20.0f && m_mousePos.x <= winX + 20.0f + tabW &&
                      m_mousePos.y >= tabY && m_mousePos.y <= tabY + tabH);
    uint32_t colTab0 = (m_activeTab == 0) ? 0xFF8A2424 : (hoverTab0 ? 0xFF442020 : 0xFF222222);
    drawPanel(winX + 20.0f, tabY, tabW, tabH, colTab0, 0xFF6B1D1D, 2.0f);
    drawText(winX + 75.0f, tabY + 11.0f, "CONECTAR DIRECTO", 0xFFFFFFFF, 1.3f);

    // Tab 1: Servidores
    bool hoverTab1 = (m_mousePos.x >= winX + 295.0f && m_mousePos.x <= winX + 295.0f + tabW &&
                      m_mousePos.y >= tabY && m_mousePos.y <= tabY + tabH);
    uint32_t colTab1 = (m_activeTab == 1) ? 0xFF8A2424 : (hoverTab1 ? 0xFF442020 : 0xFF222222);
    drawPanel(winX + 295.0f, tabY, tabW, tabH, colTab1, 0xFF6B1D1D, 2.0f);
    drawText(winX + 345.0f, tabY + 11.0f, "LISTA DE SERVIDORES", 0xFFFFFFFF, 1.3f);

    // Tab 2: Ajustes
    bool hoverTab2 = (m_mousePos.x >= winX + 570.0f && m_mousePos.x <= winX + 570.0f + tabW &&
                      m_mousePos.y >= tabY && m_mousePos.y <= tabY + tabH);
    uint32_t colTab2 = (m_activeTab == 2) ? 0xFF8A2424 : (hoverTab2 ? 0xFF442020 : 0xFF222222);
    drawPanel(winX + 570.0f, tabY, tabW, tabH, colTab2, 0xFF6B1D1D, 2.0f);
    drawText(winX + 645.0f, tabY + 11.0f, "AJUSTES", 0xFFFFFFFF, 1.3f);

    // Contenido según la pestaña activa
    float contentY = tabY + 52.0f;
    float contentW = winW - 40.0f;
    float contentH = 360.0f;
    drawPanel(winX + 20.0f, contentY, contentW, contentH, 0xFF181818, 0xFF3D1818, 1.0f);

    if (m_activeTab == 0) {
        // Tab 0: Conexión Directa
        drawText(winX + 45.0f, contentY + 25.0f, "CONFIGURACION DE CONEXION DIRECTA", 0xFF4080DF, 1.4f);

        // Campo 1: Nombre de jugador
        drawText(winX + 45.0f, contentY + 65.0f, "Nombre de Forajido (Nickname):", 0xFFDDDDDD, 1.2f);
        uint32_t borderName = (m_activeField == 1) ? 0xFF35B0FF : 0xFF555555;
        drawPanel(winX + 45.0f, contentY + 88.0f, 400.0f, 34.0f, 0xFF101010, borderName, 2.0f);
        std::string nameDisplay = m_playerName + ((m_activeField == 1 && (m_caretTimer % 60 < 30)) ? "_" : "");
        drawText(winX + 55.0f, contentY + 97.0f, nameDisplay, 0xFFFFFFFF, 1.3f);

        // Campo 2: Dirección IP
        drawText(winX + 45.0f, contentY + 140.0f, "Direccion IP del Servidor:", 0xFFDDDDDD, 1.2f);
        uint32_t borderIp = (m_activeField == 2) ? 0xFF35B0FF : 0xFF555555;
        drawPanel(winX + 45.0f, contentY + 163.0f, 300.0f, 34.0f, 0xFF101010, borderIp, 2.0f);
        std::string ipDisplay = m_serverIp + ((m_activeField == 2 && (m_caretTimer % 60 < 30)) ? "_" : "");
        drawText(winX + 55.0f, contentY + 172.0f, ipDisplay, 0xFFFFFFFF, 1.3f);

        // Campo 3: Puerto UDP
        drawText(winX + 365.0f, contentY + 140.0f, "Puerto UDP:", 0xFFDDDDDD, 1.2f);
        uint32_t borderPort = (m_activeField == 3) ? 0xFF35B0FF : 0xFF555555;
        drawPanel(winX + 365.0f, contentY + 163.0f, 120.0f, 34.0f, 0xFF101010, borderPort, 2.0f);
        std::string portDisplay = m_serverPort + ((m_activeField == 3 && (m_caretTimer % 60 < 30)) ? "_" : "");
        drawText(winX + 375.0f, contentY + 172.0f, portDisplay, 0xFFFFFFFF, 1.3f);

        // Botón Conectar
        float btnX = winX + 45.0f;
        float btnY = contentY + 235.0f;
        float btnW = 340.0f;
        float btnH = 50.0f;
        bool hoverBtn = (m_mousePos.x >= btnX && m_mousePos.x <= btnX + btnW &&
                         m_mousePos.y >= btnY && m_mousePos.y <= btnY + btnH);
        uint32_t btnCol = hoverBtn ? 0xFFB03030 : 0xFF8A2020;
        drawPanel(btnX, btnY, btnW, btnH, btnCol, 0xFFFFA020, 2.0f);
        drawText(btnX + 40.0f, btnY + 16.0f, "ENTRAR AL SERVIDOR", 0xFFFFFFFF, 1.5f);

        // Info box
        drawText(winX + 45.0f, contentY + 315.0f, "Presiona ENTER o haz clic en el boton para iniciar la sesion multijugador.", 0xFF888888, 1.1f);
        drawText(winX + 45.0f, contentY + 333.0f, "Mundo 100% Sandbox limpio - Recursos gestionados por Lua en el servidor.", 0xFF50D050, 1.1f);

    } else if (m_activeTab == 1) {
        // Tab 1: Lista de Servidores
        drawText(winX + 45.0f, contentY + 20.0f, "SERVIDORES PUBLICOS DISPONIBLES", 0xFF4080DF, 1.4f);

        // Encabezado de la tabla
        drawPanel(winX + 40.0f, contentY + 50.0f, contentW - 80.0f, 30.0f, 0xFF242424, 0xFF444444, 1.0f);
        drawText(winX + 55.0f, contentY + 58.0f, "NOMBRE DEL SERVIDOR", 0xFFCCCCCC, 1.1f);
        drawText(winX + 400.0f, contentY + 58.0f, "MODO", 0xFFCCCCCC, 1.1f);
        drawText(winX + 540.0f, contentY + 58.0f, "JUGADORES", 0xFFCCCCCC, 1.1f);
        drawText(winX + 650.0f, contentY + 58.0f, "PING", 0xFFCCCCCC, 1.1f);

        // Fila 1: Servidor Local
        float r1Y = contentY + 85.0f;
        drawPanel(winX + 40.0f, r1Y, contentW - 80.0f, 40.0f, 0xFF1C1C1C, 0xFF333333, 1.0f);
        drawText(winX + 55.0f, r1Y + 12.0f, "Frontier Dedicated Server (Localhost)", 0xFF50FF50, 1.2f);
        drawText(winX + 400.0f, r1Y + 12.0f, "Freeroam 1899", 0xFFCCCCCC, 1.1f);
        drawText(winX + 555.0f, r1Y + 12.0f, "1 / 32", 0xFFFFFFFF, 1.1f);
        drawText(winX + 655.0f, r1Y + 12.0f, "1ms", 0xFF50FF50, 1.1f);

        float joinBtn1X = winX + 710.0f;
        bool hoverJ1 = (m_mousePos.x >= joinBtn1X && m_mousePos.x <= joinBtn1X + 60.0f &&
                        m_mousePos.y >= r1Y + 6.0f && m_mousePos.y <= r1Y + 34.0f);
        drawPanel(joinBtn1X, r1Y + 6.0f, 60.0f, 28.0f, hoverJ1 ? 0xFFB03030 : 0xFF662020, 0xFF8A2424, 1.0f);
        drawText(joinBtn1X + 8.0f, r1Y + 13.0f, "Unirse", 0xFFFFFFFF, 1.0f);

        // Fila 2: Servidor Oficial #1
        float r2Y = contentY + 130.0f;
        drawPanel(winX + 40.0f, r2Y, contentW - 80.0f, 40.0f, 0xFF1C1C1C, 0xFF333333, 1.0f);
        drawText(winX + 55.0f, r2Y + 12.0f, "FrontierMP Official Roleplay West #1", 0xFFFFFFFF, 1.2f);
        drawText(winX + 400.0f, r2Y + 12.0f, "Roleplay HC", 0xFFCCCCCC, 1.1f);
        drawText(winX + 555.0f, r2Y + 12.0f, "18 / 32", 0xFFFFFFFF, 1.1f);
        drawText(winX + 655.0f, r2Y + 12.0f, "28ms", 0xFF50FF50, 1.1f);

        float joinBtn2X = winX + 710.0f;
        bool hoverJ2 = (m_mousePos.x >= joinBtn2X && m_mousePos.x <= joinBtn2X + 60.0f &&
                        m_mousePos.y >= r2Y + 6.0f && m_mousePos.y <= r2Y + 34.0f);
        drawPanel(joinBtn2X, r2Y + 6.0f, 60.0f, 28.0f, hoverJ2 ? 0xFFB03030 : 0xFF662020, 0xFF8A2424, 1.0f);
        drawText(joinBtn2X + 8.0f, r2Y + 13.0f, "Unirse", 0xFFFFFFFF, 1.0f);

    } else if (m_activeTab == 2) {
        // Tab 2: Ajustes
        drawText(winX + 45.0f, contentY + 25.0f, "CONFIGURACION DEL FRAMEWORK", 0xFF4080DF, 1.4f);
        drawText(winX + 45.0f, contentY + 65.0f, "Motor Grafico: DirectX 12 (Pipeline D3D11On12 Acelerado)", 0xFF50D050, 1.2f);
        drawText(winX + 45.0f, contentY + 95.0f, "Red: ENet UDP Port 4674 | Asset Streaming HTTP Port 4675", 0xFFCCCCCC, 1.2f);
        drawText(winX + 45.0f, contentY + 125.0f, "Scripting Engine: Lua 5.4 con Event Dispatcher FiveM", 0xFFCCCCCC, 1.2f);
        drawText(winX + 45.0f, contentY + 155.0f, "Mundo Sandbox: Limpieza de misiones, encuentros y peds singleplayer", 0xFFCCCCCC, 1.2f);
        drawText(winX + 45.0f, contentY + 205.0f, "Controles del Menú:", 0xFF4080DF, 1.3f);
        drawText(winX + 65.0f, contentY + 235.0f, "F8  - Abrir / Cerrar este menu principal en cualquier momento.", 0xFFFFFFFF, 1.1f);
        drawText(winX + 65.0f, contentY + 260.0f, "T   - Abrir el chat multijugador dentro del juego.", 0xFFFFFFFF, 1.1f);
        drawText(winX + 65.0f, contentY + 285.0f, "ESC - Salir del chat o cerrar dialogos.", 0xFFFFFFFF, 1.1f);
    }

    // Pie de la ventana
    drawText(winX + 24.0f, winY + winH - 24.0f, "FrontierMP v1.0.0 - Presiona F8 para ocultar el menu | FiveM para RDR1", 0xFF666666, 1.0f);
}

void D3D11Renderer::drawChat() {
    float chatX = 25.0f;
    float chatY = 25.0f;
    float chatW = 460.0f;

    // Dibujar historial de mensajes
    if (!m_chatMessages.empty() || m_chatInputActive) {
        float totalH = static_cast<float>(m_chatMessages.size() * 20 + (m_chatInputActive ? 40 : 10));
        drawPanel(chatX - 5.0f, chatY - 5.0f, chatW, totalH, 0x90000000, 0x40FFFFFF, 1.0f);

        float curY = chatY;
        for (const auto& msg : m_chatMessages) {
            drawText(chatX, curY, msg.text, 0xFFFFFFFF, 1.2f);
            curY += 20.0f;
        }

        // Campo de entrada de texto del chat cuando está activo
        if (m_chatInputActive) {
            curY += 5.0f;
            drawPanel(chatX, curY, chatW - 10.0f, 26.0f, 0xD0151515, 0xFF35B0FF, 1.5f);
            std::string inputDisplay = "[Chat]: " + m_chatInputBuffer + "_";
            drawText(chatX + 6.0f, curY + 6.0f, inputDisplay, 0xFFFFFFFF, 1.2f);
        }
    }
}

void D3D11Renderer::render(IDXGISwapChain* pSwapChain) {
    if (!m_initialized) return;

    // Pump ENet every rendered frame so ServerData(0x04), disconnects and
    // subsequent packets are consumed after connect() returns.
    Net::ClientNetwork::get().update();

    UINT backBufferIdx = 0;
    if (m_isD3D12) {
        IDXGISwapChain3* pSwapChain3 = nullptr;
        if (SUCCEEDED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&pSwapChain3)))) {
            backBufferIdx = pSwapChain3->GetCurrentBackBufferIndex();
            pSwapChain3->Release();
        }

        if (backBufferIdx >= m_wrappedBuffers.size() || !m_wrappedBuffers[backBufferIdx]) return;

        m_pD3D11On12Device->AcquireWrappedResources(&m_wrappedBuffers[backBufferIdx], 1);
        m_pContext->OMSetRenderTargets(1, &m_renderTargetViews[backBufferIdx], nullptr);
    } else {
        if (!m_pD3D11RTV) return;
        m_pContext->OMSetRenderTargets(1, &m_pD3D11RTV, nullptr);
    }

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(m_width > 0 ? m_width : 1920);
    vp.Height = static_cast<float>(m_height > 0 ? m_height : 1080);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_pContext->RSSetViewports(1, &vp);

    beginDraw();

    if (m_showMainMenu) {
        drawMainMenu();
    }

    if (m_showChat || m_chatInputActive) {
        drawChat();
    }

    if (m_showMainMenu || m_chatInputActive) {
        drawCursor();
    }

    endDraw();

    if (m_isD3D12) {
        ID3D11RenderTargetView* nullRTV = nullptr;
        m_pContext->OMSetRenderTargets(1, &nullRTV, nullptr);
        m_pD3D11On12Device->ReleaseWrappedResources(&m_wrappedBuffers[backBufferIdx], 1);
        m_pContext->Flush();
    }
}

void D3D11Renderer::onResize() {
    for (auto& rtv : m_renderTargetViews) {
        if (rtv) { rtv->Release(); rtv = nullptr; }
    }
    for (auto& buf : m_wrappedBuffers) {
        if (buf) { buf->Release(); buf = nullptr; }
    }
    if (m_pD3D11RTV) {
        m_pD3D11RTV->Release();
        m_pD3D11RTV = nullptr;
    }
}

void D3D11Renderer::shutdown() {
    onResize();

    if (m_pWhiteSRV) { m_pWhiteSRV->Release(); m_pWhiteSRV = nullptr; }
    if (m_pWhiteTex) { m_pWhiteTex->Release(); m_pWhiteTex = nullptr; }
    if (m_pFontSRV) { m_pFontSRV->Release(); m_pFontSRV = nullptr; }
    if (m_pFontTex) { m_pFontTex->Release(); m_pFontTex = nullptr; }
    if (m_pConstantBuffer) { m_pConstantBuffer->Release(); m_pConstantBuffer = nullptr; }
    if (m_pVertexBuffer) { m_pVertexBuffer->Release(); m_pVertexBuffer = nullptr; }
    if (m_pInputLayout) { m_pInputLayout->Release(); m_pInputLayout = nullptr; }
    if (m_pVertexShader) { m_pVertexShader->Release(); m_pVertexShader = nullptr; }
    if (m_pPixelShader) { m_pPixelShader->Release(); m_pPixelShader = nullptr; }
    if (m_pBlendState) { m_pBlendState->Release(); m_pBlendState = nullptr; }
    if (m_pSamplerState) { m_pSamplerState->Release(); m_pSamplerState = nullptr; }
    if (m_pRasterizerState) { m_pRasterizerState->Release(); m_pRasterizerState = nullptr; }
    if (m_pD3D11On12Device) { m_pD3D11On12Device->Release(); m_pD3D11On12Device = nullptr; }
    if (m_pContext) { m_pContext->Release(); m_pContext = nullptr; }
    if (m_pDevice) { m_pDevice->Release(); m_pDevice = nullptr; }
    if (m_pD3D12Device) { m_pD3D12Device->Release(); m_pD3D12Device = nullptr; }
    if (m_pCommandQueue) { m_pCommandQueue->Release(); m_pCommandQueue = nullptr; }

    m_wrappedBuffers.clear();
    m_renderTargetViews.clear();
    m_pCurrentSRV = nullptr;
    m_isD3D12 = false;
    m_initialized = false;
}
void D3D11Renderer::addChatMessage(const std::string& msg) {
    ChatEntry entry;
    entry.text = msg;
    entry.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    m_chatMessages.push_back(entry);

    if (m_chatMessages.size() > 20) {
        m_chatMessages.erase(m_chatMessages.begin());
    }
}

void D3D11Renderer::triggerConnect() {
    uint16_t port = 4674;
    try {
        port = static_cast<uint16_t>(std::stoi(m_serverPort));
    } catch (...) {}

    std::cout << "[D3D11Renderer] Conectando a " << m_serverIp << ":" << port
              << " como '" << m_playerName << "'..." << std::endl;

    // Connect ya no fuerza directamente el loading. La transición real es:
    // ENet -> ServerData(packet 4) -> HTTP resources -> LoadOnline/InitSpawn.
    m_showChat = true;

    auto& network = Net::ClientNetwork::get();
    if (network.isConnected()) {
        std::cout
            << "[D3D11Renderer] Ya existe una conexión ENet activa; "
               "se ignora el segundo intento."
            << std::endl;
        return;
    }

    const bool connected =
        network.connect(m_serverIp, port, m_playerName);

    if (!connected) {
        std::cerr << "[D3D11Renderer] No se pudo iniciar la conexión al servidor." << std::endl;
        return;
    }

    // El menú se oculta cuando el ServerData fue validado y comenzó la carga
    // de recursos, no solo cuando se abrió el socket UDP.
}

bool D3D11Renderer::handleInput(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_MOUSEMOVE) {
        m_mousePos.x = LOWORD(lParam);
        m_mousePos.y = HIWORD(lParam);
        return m_showMainMenu;
    }

    if (uMsg == WM_LBUTTONDOWN) {
        m_mousePos.x = LOWORD(lParam);
        m_mousePos.y = HIWORD(lParam);
        m_mouseLDown = true;

        if (m_showMainMenu) {
            float winW = 860.0f;
            float winH = 580.0f;
            float winX = ((float)m_width - winW) * 0.5f;
            float winY = ((float)m_height - winH) * 0.5f;
            float tabY = winY + 95.0f;
            float tabW = 260.0f;
            float tabH = 38.0f;

            // Clic en Tab 0 (Conectar)
            if (m_mousePos.x >= winX + 20.0f && m_mousePos.x <= winX + 20.0f + tabW &&
                m_mousePos.y >= tabY && m_mousePos.y <= tabY + tabH) {
                m_activeTab = 0;
                return true;
            }
            // Clic en Tab 1 (Servidores)
            if (m_mousePos.x >= winX + 295.0f && m_mousePos.x <= winX + 295.0f + tabW &&
                m_mousePos.y >= tabY && m_mousePos.y <= tabY + tabH) {
                m_activeTab = 1;
                return true;
            }
            // Clic en Tab 2 (Ajustes)
            if (m_mousePos.x >= winX + 570.0f && m_mousePos.x <= winX + 570.0f + tabW &&
                m_mousePos.y >= tabY && m_mousePos.y <= tabY + tabH) {
                m_activeTab = 2;
                return true;
            }

            float contentY = tabY + 52.0f;
            if (m_activeTab == 0) {
                // Clic en Campo Nickname
                if (m_mousePos.x >= winX + 45.0f && m_mousePos.x <= winX + 445.0f &&
                    m_mousePos.y >= contentY + 88.0f && m_mousePos.y <= contentY + 122.0f) {
                    m_activeField = 1;
                    return true;
                }
                // Clic en Campo IP
                if (m_mousePos.x >= winX + 45.0f && m_mousePos.x <= winX + 345.0f &&
                    m_mousePos.y >= contentY + 163.0f && m_mousePos.y <= contentY + 197.0f) {
                    m_activeField = 2;
                    return true;
                }
                // Clic en Campo Puerto
                if (m_mousePos.x >= winX + 365.0f && m_mousePos.x <= winX + 485.0f &&
                    m_mousePos.y >= contentY + 163.0f && m_mousePos.y <= contentY + 197.0f) {
                    m_activeField = 3;
                    return true;
                }
                // Clic en Botón "ENTRAR AL SERVIDOR"
                float btnX = winX + 45.0f;
                float btnY = contentY + 235.0f;
                float btnW = 340.0f;
                float btnH = 50.0f;
                if (m_mousePos.x >= btnX && m_mousePos.x <= btnX + btnW &&
                    m_mousePos.y >= btnY && m_mousePos.y <= btnY + btnH) {
                    triggerConnect();
                    return true;
                }
            } else if (m_activeTab == 1) {
                // Clic en "Unirse" de la Fila 1 (Localhost)
                float r1Y = contentY + 85.0f;
                float j1X = winX + 710.0f;
                if (m_mousePos.x >= j1X && m_mousePos.x <= j1X + 60.0f &&
                    m_mousePos.y >= r1Y + 6.0f && m_mousePos.y <= r1Y + 34.0f) {
                    m_serverIp = "127.0.0.1";
                    m_serverPort = "4674";
                    triggerConnect();
                    return true;
                }
                // Clic en "Unirse" de la Fila 2
                float r2Y = contentY + 130.0f;
                float j2X = winX + 710.0f;
                if (m_mousePos.x >= j2X && m_mousePos.x <= j2X + 60.0f &&
                    m_mousePos.y >= r2Y + 6.0f && m_mousePos.y <= r2Y + 34.0f) {
                    m_serverIp = "127.0.0.1";
                    m_serverPort = "4674";
                    triggerConnect();
                    return true;
                }
            }

            return true;
        }
    }

    if (uMsg == WM_LBUTTONUP) {
        m_mouseLDown = false;
        if (m_showMainMenu) return true;
    }

    if (uMsg == WM_KEYDOWN) {
        if (wParam == VK_F8) {
            m_showMainMenu = !m_showMainMenu;
            std::cout << "[FrontierMP] Menú principal alternado: " << (m_showMainMenu ? "ON" : "OFF") << std::endl;
            return true;
        }

        if (m_showMainMenu) {
            if (wParam == VK_RETURN) {
                triggerConnect();
                return true;
            } else if (wParam == VK_TAB) {
                m_activeField = (m_activeField % 3) + 1;
                return true;
            } else if (wParam == VK_BACK) {
                if (m_activeField == 1 && !m_playerName.empty()) m_playerName.pop_back();
                else if (m_activeField == 2 && !m_serverIp.empty()) m_serverIp.pop_back();
                else if (m_activeField == 3 && !m_serverPort.empty()) m_serverPort.pop_back();
                return true;
            } else if (wParam == VK_ESCAPE) {
                m_showMainMenu = false;
                return true;
            }
            return true;
        } else {
            if (wParam == 'T' && !m_chatInputActive) {
                m_chatInputActive = true;
                m_chatInputBuffer.clear();
                return true;
            } else if (wParam == VK_RETURN && m_chatInputActive) {
                if (!m_chatInputBuffer.empty()) {
                    Net::ClientNetwork::get().sendChatMessage(m_chatInputBuffer);
                    m_chatInputBuffer.clear();
                }
                m_chatInputActive = false;
                return true;
            } else if (wParam == VK_ESCAPE && m_chatInputActive) {
                m_chatInputActive = false;
                return true;
            } else if (wParam == VK_BACK && m_chatInputActive) {
                if (!m_chatInputBuffer.empty()) {
                    m_chatInputBuffer.pop_back();
                }
                return true;
            }
            if (m_chatInputActive) return true;
        }
    }

    if (uMsg == WM_CHAR) {
        char c = static_cast<char>(wParam);
        if (c >= 32 && c <= 126) {
            if (m_showMainMenu) {
                if (m_activeField == 1 && m_playerName.length() < 24) m_playerName += c;
                else if (m_activeField == 2 && m_serverIp.length() < 30) m_serverIp += c;
                else if (m_activeField == 3 && m_serverPort.length() < 6) m_serverPort += c;
                return true;
            } else if (m_chatInputActive && m_chatInputBuffer.length() < 120) {
                m_chatInputBuffer += c;
                return true;
            }
        }
    }

    return false;
}

} // namespace Frontier::UI
