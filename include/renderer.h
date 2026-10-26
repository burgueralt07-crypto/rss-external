#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    // Inicializa D3D11 + ImGui para a janela fornecida
    bool Init(HWND hwnd, int width, int height);

    // Limpa o back-buffer com alpha=0 (transparência total)
    void BeginFrame();

    // Renderiza o ImGui e apresenta o frame
    void EndFrame();

    // Redimensiona swap chain quando a janela muda de tamanho
    void Resize(int width, int height);

    void Shutdown();

    ID3D11Device*           GetDevice()        const { return m_device; }
    ID3D11DeviceContext*    GetContext()        const { return m_context; }

private:
    bool CreateDeviceAndSwapChain(HWND hwnd);
    bool CreateRenderTarget();
    void ReleaseRenderTarget();

    ID3D11Device*           m_device            = nullptr;
    ID3D11DeviceContext*    m_context           = nullptr;
    IDXGISwapChain*         m_swapChain         = nullptr;
    ID3D11RenderTargetView* m_renderTargetView  = nullptr;

    int m_width  = 0;
    int m_height = 0;
};
