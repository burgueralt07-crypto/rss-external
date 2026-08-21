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
    bool    isGK      = false;
    Vector3 position;
    Vector3 rightVec  = { 1.f, 0.f,  0.f };
    Vector3 upVec     = { 0.f, 1.f,  0.f };
    Vector3 lookVec   = { 0.f, 0.f, -1.f };
    // Hitbox física do GK (part "Hitbox" dentro do model)
    Vector3 hitboxPos;
    Vector3 hitboxSize; // zero se não encontrada
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
// GameMode — seleciona os thresholds e a lógica de dive
// --------------------------------------------------------------------------
enum class GameMode : int {
    Mode4v4 = 0,   // gol pequeno, sem Jump+Dive
    Mode7v7 = 1,   // gol grande, suporta Space+Q/E para chutes altos
};

// --------------------------------------------------------------------------
// AutoDive
//
// Thread dedicada ao scan — lê memória diretamente e decide dive sem
// bloquear o render. PressKey() dispara SendInput imediatamente na ScanLoop,
// usando hardware scancode puro (wVk=0). O key-up é enviado 25ms depois
// em thread separada para não travar o loop de 240 Hz.
// --------------------------------------------------------------------------
class AutoDive {
public:
    AutoDive()  = default;
    ~AutoDive() { Stop(); }

    AutoDive(const AutoDive&)            = delete;
    AutoDive& operator=(const AutoDive&) = delete;

    struct Config {
        bool      enabled         = false;
        bool      forceGK         = false;
        bool      onlyInGoal      = true;
        bool      highJump        = true;
        GameMode  gameMode        = GameMode::Mode4v4;

        // ── 4v4 ──────────────────────────────────────────────────────────
        float triggerDistance     = 18.f;
        float cooldownSec         = 1.2f;
        float minBallSpeed        = 8.f;
        float goalMargin          = 2.f;
        // relPos.x acima deste valor → dive direita/esquerda
        float diveXThreshold      = 3.f;
        // relPos.y acima deste valor → Jump (Space)
        float jumpYThreshold      = 5.5f;
        // |relPos.x| máximo para o Jump puro ter efeito
        float jumpXMaxForPure     = 6.f;

        // ── 7v7 (gol maior) ──────────────────────────────────────────────
        // Usar valores abaixo quando gameMode == Mode7v7
        float diveXThreshold7v7   = 5.f;   // relX para dive direita/esquerda (zona baixa)
        float jumpDiveXMin7v7     = 2.f;   // |relX| mínimo para acionar Jump+Dive (zona alta)
        float jumpPureXMax7v7     = 2.f;   // |relX| máximo para Jump puro (sem dive lateral)
        // Delay entre Space e Q/E no combo Jump+Dive (ms)
        int   jumpDiveDelayMs     = 180;

        int   simSteps            = 45;
        float simDt               = 0.035f;
        float gravity             = 156.96f; // workspace.Gravity * 0.8
        int   scanRate            = 240;     // scans por segundo
    };

    Config cfg;

    // Inicia thread de scan. Chame quando attach OK.
    void Start(RobloxReader* rbx);
    // Para thread. Chame ao desativar ou ao fechar.
    void Stop();

    const char* LastDiveKey() const { return m_lastKey; }
    bool        DiveFired()   const { return m_firedThisFrame; }

    // Debug
    struct DebugInfo {
        float distToBall  = 0.f;
        float relPosX     = 0.f;
        float relPosY     = 0.f;
        float relPosZ     = 0.f;
        bool  approaching = false;
        bool  isAPG       = false;
        float ballLocalZ  = 0.f;   // localZ inicial da bola no espaço do gol (debug da previsão)
        std::string blockReason;

        float ballPosX  = 0.f, ballPosZ  = 0.f;
        float ballVelX  = 0.f, ballVelZ  = 0.f;
        float goalPosX  = 0.f, goalPosZ  = 0.f;
        float goalSizeX = 0.f, goalSizeZ = 0.f;
    } debug;

private:
    // Dispara a tecla imediatamente por hardware scancode puro (wVk=0).
    // Solta a tecla 25ms depois em thread assíncrona — não trava o loop de scan.
    static void PressKey(WORD vk)
    {
        WORD sc = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));

        // Key down — imediato, sem passar pelo virtual-key path
        INPUT down = {};
        down.type       = INPUT_KEYBOARD;
        down.ki.wVk     = 0;
        down.ki.wScan   = sc;
        down.ki.dwFlags = KEYEVENTF_SCANCODE;
        SendInput(1, &down, sizeof(INPUT));

        // Key up — 25ms depois em thread separada para não bloquear o scan
        std::thread([sc]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            INPUT up = {};
            up.type       = INPUT_KEYBOARD;
            up.ki.wVk     = 0;
            up.ki.wScan   = sc;
            up.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
            SendInput(1, &up, sizeof(INPUT));
        }).detach();
    }

    static Vector3 PointToObjectSpace(const Vector3& origin,
                                      const Vector3& right,
                                      const Vector3& up,
                                      const Vector3& look,
                                      const Vector3& worldPos);

    bool IsBallTargetingGoal(const BallState& ball, const GoalState& goal) const;
    bool IsBallHittingGK(const BallState& ball, const GKState& gk) const;

    // Loop da thread de scan
    void ScanLoop(RobloxReader* rbx);

    // Lógica de decisão (chamada pelo ScanLoop com cópias locais)
    void Evaluate(const GKState& gk, const BallState& ball, const GoalState& goal);

    std::chrono::steady_clock::time_point m_lastDiveTime;
    bool        m_firedThisFrame = false;
    const char* m_lastKey        = "-";

    std::thread       m_thread;
    std::atomic<bool> m_running{ false };
};
