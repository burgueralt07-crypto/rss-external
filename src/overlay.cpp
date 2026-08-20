#include "overlay.h"
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

// --------------------------------------------------------------------------
LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_DESTROY:
    {
        Overlay* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) self->m_running = false;
        PostQuitMessage(0);
        return 0;
    }
    case WM_SIZE:
    {
        Overlay* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self)
        {
            self->m_width  = LOWORD(lParam);
            self->m_height = HIWORD(lParam);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// --------------------------------------------------------------------------
Overlay::~Overlay()
{
    if (m_hwnd)     { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    if (m_hInstance) UnregisterClassW(m_className.c_str(), m_hInstance);
}

// --------------------------------------------------------------------------
bool Overlay::Init(const std::wstring& targetWindowName)
{
    m_targetName = targetWindowName;
    m_hInstance  = GetModuleHandleW(nullptr);

    m_targetHwnd = FindWindowW(nullptr, targetWindowName.c_str());
    if (!m_targetHwnd)
    {
        m_width  = GetSystemMetrics(SM_CXSCREEN);
        m_height = GetSystemMetrics(SM_CYSCREEN);
    }
    else
    {
        RECT rect{};
        GetClientRect(m_targetHwnd, &rect);
        m_width  = rect.right  - rect.left;
        m_height = rect.bottom - rect.top;
    }

    if (!RegisterOverlayClass()) return false;
    if (!CreateOverlayWindow())  return false;

    m_running = true;
    return true;
}

// --------------------------------------------------------------------------
bool Overlay::RegisterOverlayClass()
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = m_hInstance;
    wc.lpszClassName = m_className.c_str();
    wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    return RegisterClassExW(&wc) != 0;
}

// --------------------------------------------------------------------------
bool Overlay::CreateOverlayWindow()
{
    int x = 0, y = 0;
    if (m_targetHwnd)
    {
        RECT rect{};
        GetWindowRect(m_targetHwnd, &rect);
        x = rect.left;
        y = rect.top;
    }

    // Começa com WS_EX_TRANSPARENT (click-through)
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    DWORD style   = WS_POPUP;

    m_hwnd = CreateWindowExW(exStyle, m_className.c_str(), L"Overlay", style,
                              x, y, m_width, m_height,
                              nullptr, nullptr, m_hInstance, nullptr);
    if (!m_hwnd) return false;

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    MARGINS margins{ -1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    return true;
}

// --------------------------------------------------------------------------
bool Overlay::ProcessMessages()
{
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT)
        {
            m_running = false;
            return false;
        }
    }
    return true;
}

// --------------------------------------------------------------------------
void Overlay::SyncWithTarget()
{
    if (!m_targetHwnd) return;

    // Se a janela alvo sumiu, tenta reencontrar
    if (!IsWindow(m_targetHwnd))
    {
        m_targetHwnd = FindWindowW(nullptr, m_targetName.c_str());
        if (!m_targetHwnd) return;
    }

    RECT rect{};
    GetWindowRect(m_targetHwnd, &rect);

    int w = rect.right  - rect.left;
    int h = rect.bottom - rect.top;

    SetWindowPos(m_hwnd, HWND_TOPMOST,
                 rect.left, rect.top, w, h,
                 SWP_NOACTIVATE);

    m_width  = w;
    m_height = h;
}

// --------------------------------------------------------------------------
// SetVisible — mostra ou oculta o overlay completamente
// --------------------------------------------------------------------------
void Overlay::SetVisible(bool visible)
{
    if (m_visible == visible) return;
    m_visible = visible;
    ShowWindow(m_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

// --------------------------------------------------------------------------
// SetClickThrough — alterna WS_EX_TRANSPARENT no extended style
//
// true  → mouse passa direto para o jogo (ESP apenas visual)
// false → overlay captura o mouse (menu arrastável)
// --------------------------------------------------------------------------
void Overlay::SetClickThrough(bool clickThrough)
{
    if (m_clickThrough == clickThrough) return;
    m_clickThrough = clickThrough;

    LONG_PTR exStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);

    if (clickThrough)
        exStyle |=  WS_EX_TRANSPARENT;   // mouse passa para o jogo
    else
        exStyle &= ~WS_EX_TRANSPARENT;   // overlay recebe o mouse

    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exStyle);
}
