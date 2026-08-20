#pragma once
#include "rmath.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

// --------------------------------------------------------------------------
// GKState
// --------------------------------------------------------------------------
struct GKState {
    bool    isGK     = false;
    Vector3 position;
    Vector3 rightVec  = { 1.f, 0.f,  0.f };
    Vector3 upVec     = { 0.f, 1.f,  0.f };
    Vector3 lookVec   = { 0.f, 0.f, -1.f };
};

// --------------------------------------------------------------------------
// BallState
// --------------------------------------------------------------------------
struct BallState {
    bool    exists   = false;
    bool    isWelded = false;
    Vector3 position;
    Vector3 velocity;
};

// --------------------------------------------------------------------------
// GoalState
// --------------------------------------------------------------------------
struct GoalState {
    bool    exists   = false;
    Vector3 position;
    Vector3 size;
    Vector3 rightVec  = { 1.f, 0.f, 0.f };
    Vector3 upVec     = { 0.f, 1.f, 0.f };
    Vector3 lookVec   = { 0.f, 0.f, 1.f };
};

// Forward declaration
class RobloxReader;

// --------------------------------------------------------------------------
// AutoDive
//
// Thread dedicada ao scan — lê memória e decide dive sem bloquear o render.
// PressKey() apenas armazena a tecla num atomic; a thread principal chama
// DispatchPendingKeys() a cada frame para enviar via SendInput.
// --------------------------------------------------------------------------
class AutoDive {
public:
    AutoDive()  = default;
    ~AutoDive() { Stop(); }

    AutoDive(const AutoDive&)            = delete;
    AutoDive& operator=(const AutoDive&) = delete;

    struct Config {
        bool  enabled         = false;
        bool  forceGK         = false;
        bool  onlyInGoal      = true;
        bool  highJump        = true;
        float triggerDistance = 18.f;
        float cooldownSec     = 1.2f;
        float minBallSpeed    = 8.f;
        float goalMargin      = 2.f;
        int   simSteps        = 45;
        float simDt           = 0.035f;
        float gravity         = 156.96f;  // workspace.Gravity * 0.8, igual ao Lua
        int   scanRate        = 240;    // scans por segundo
    };

    Config cfg;

    // Inicia thread de scan. Chame quando attach OK.
    void Start(RobloxReader* rbx);
    // Para thread. Chame ao desativar ou ao fechar.
    void Stop();

    // Chamado pela thread principal a cada frame (antes do Sleep).
    // Despacha tecla pendente via SendInput — deve vir da thread com foco de UI.
    void DispatchPendingKeys(HWND targetHwnd)
    {
        WORD vk = static_cast<WORD>(m_pendingKey.exchange(0));
        if (!vk) return;
        SendVKey(targetHwnd, vk);
    }

    const char* LastDiveKey() const { return m_lastKey; }
    bool        DiveFired()   const { return m_firedThisFrame; }

    // Debug
    struct DebugInfo {
        float distToBall  = 0.f;
        float relPosX     = 0.f;
        float relPosY     = 0.f;
        float relPosZ     = 0.f;
        float impactX     = 0.f;  // ponto de impacto projetado no plano do gol (X)
        float impactY     = 0.f;  // ponto de impacto projetado no plano do gol (Y)
        bool  approaching = false;
        std::string blockReason;
    mutable DebugInfo debug;

private:
    // Envia key down+up via SendInput (thread principal)
    static void SendVKey(HWND targetHwnd, WORD vk)
    {
        if (targetHwnd && IsWindow(targetHwnd) && GetForegroundWindow() != targetHwnd)
            SetForegroundWindow(targetHwnd);

        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        INPUT inputs[2] = {};
        inputs[0].type       = INPUT_KEYBOARD;
        inputs[0].ki.wVk     = vk;
        inputs[0].ki.wScan   = static_cast<WORD>(sc);
        inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs[1].type       = INPUT_KEYBOARD;
        inputs[1].ki.wVk     = vk;
        inputs[1].ki.wScan   = static_cast<WORD>(sc);
        inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        SendInput(2, inputs, sizeof(INPUT));
    }

    // Armazena tecla — chamado pela ScanLoop (thread de background)
    void PressKey(WORD vk) { m_pendingKey.store(vk); }

    static Vector3 PointToObjectSpace(const Vector3& origin,
                                      const Vector3& right,
                                      const Vector3& up,
                                      const Vector3& look,
                                      const Vector3& worldPos);

    bool IsBallTargetingGoal(const BallState& ball, const GoalState& goal) const;

    // Loop da thread de scan
    void ScanLoop(RobloxReader* rbx);

    // Lógica de decisão (chamada pelo ScanLoop com cópias locais)
    void Evaluate(const GKState& gk, const BallState& ball, const GoalState& goal);

    std::chrono::steady_clock::time_point m_lastDiveTime;
    bool        m_firedThisFrame = false;
    const char* m_lastKey        = "-";

    // Tecla pendente: escrita pelo ScanLoop, lida pela thread principal
    std::atomic<uint16_t> m_pendingKey{ 0 };

    std::thread       m_thread;
    std::atomic<bool> m_running{ false };
};
