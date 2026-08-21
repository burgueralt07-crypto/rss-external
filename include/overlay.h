#pragma once
#include <Windows.h>
#include <string>

class Overlay {
public:
    Overlay()  = default;
    ~Overlay();

    bool Init(const std::wstring& targetWindowName);
    bool ProcessMessages();
    void SyncWithTarget();

    // Mostra ou oculta o overlay por inteiro
    void SetVisible(bool visible);

    // Liga/desliga click-through (WS_EX_TRANSPARENT)
    // true  = transparente ao mouse (jogo recebe input)
    // false = overlay recebe input (menu arrastável)
    void SetClickThrough(bool clickThrough);

    // Streamproof: oculta a janela de capturas de tela, OBS, Discord, etc.
    // true  = janela invisível para software de captura
    // false = comportamento normal (visível para capturas)
    void SetStreamproof(bool enable);

    HWND  GetHWND()        const { return m_hwnd; }
    HWND  GetTargetHWND()  const { return m_targetHwnd; }
    int   GetWidth()       const { return m_width; }
    int   GetHeight()      const { return m_height; }
    bool  IsRunning()      const { return m_running; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    bool RegisterOverlayClass();
    bool CreateOverlayWindow();

    HWND         m_hwnd        = nullptr;
    HWND         m_targetHwnd  = nullptr;
    HINSTANCE    m_hInstance   = nullptr;
    int          m_width       = 0;
    int          m_height      = 0;
    bool         m_running     = false;
    bool         m_visible     = true;
    bool         m_clickThrough = true;
    bool         m_streamproof  = false;

    std::wstring m_className  = L"OverlayClass";
    std::wstring m_targetName;
};
