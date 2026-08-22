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
    bool    exists          = false;
    bool    isWelded        = false;
    Vector3 position;
    Vector3 velocity;
    // Velocidade angular (spin) lida do Primitive::AssemblyAngularVelocity.
    // Usada para calcular Magnus force na simulação de trajetória com curva.
    // angularVelocity.y  → sidespin (curva lateral, tipo banana)
    // angularVelocity.x  → topspin/backspin (afeta queda/subida)
    Vector3 angularVelocity;
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
        float triggerDistance     = 18.f;   // LEGACY — não usado, veja diveFireDistance
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
        // Janela de tempo para disparar Jump+Dive antecipado (s).
        // Quando sim.timeToGoal <= jumpDiveTimeWindow o Space é enviado
        // mesmo que dist > diveFireDistance, desde que a trajetória já
        // esteja confirmada. 0 = desativado (usa só distância).
        float jumpDiveTimeWindow  = 0.55f;

        // ── Simulação de trajetória (spin/curva) ─────────────────────────
        int   simSteps            = 60;      // passos de integração Euler
        float simDt               = 0.03f;   // dt por passo (s)
        float gravity             = 156.96f; // workspace.Gravity * fator (studs/s²)
        // Coeficiente de Magnus — escala a força lateral por spin.
        // Roblox usa unidades arbitrárias de angularVelocity; tunar conforme jogo.
        float magnusCoeff         = 0.12f;
        // Drag linear — fração da velocidade removida por segundo.
        float dragCoeff           = 0.006f;

        // ── WatchRange — detecção antecipada ─────────────────────────────
        // A thread fica "de olho" na bola a partir desta distância.
        // Quando a trajetória simulada confirma que a bola vai no gol,
        // o dive é disparado assim que dist <= diveFireDistance.
        float watchRange          = 150.f;   // studs — começa a monitorar (hardcoded para testes)
        float diveFireDistance    = 18.f;    // studs — dispara o dive ao chegar aqui

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
        float ballLocalZ  = 0.f;   // localZ inicial da bola no espaço do gol
        std::string blockReason;

        float ballPosX  = 0.f, ballPosZ  = 0.f;
        float ballVelX  = 0.f, ballVelZ  = 0.f;
        float goalPosX  = 0.f, goalPosZ  = 0.f;
        float goalSizeX = 0.f, goalSizeZ = 0.f;

        // Spin / curva
        float spinX = 0.f;   // angularVelocity.x (topspin/backspin)
        float spinY = 0.f;   // angularVelocity.y (sidespin — curva lateral)
        float spinZ = 0.f;   // angularVelocity.z

        // Trajetória simulada — ponto previsto no plano do gol
        float predGoalX = 0.f;   // local X no espaço do gol onde a bola deve cruzar
        float predGoalY = 0.f;   // local Y no espaço do gol
        bool  simValid  = false; // true se a simulação encontrou cruzamento com o plano

        // WatchRange
        bool  inWatchRange  = false;  // bola está no range de monitoramento
        bool  trajectoryOK  = false;  // simulação diz que vai no gol
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

    // Resultado de uma simulação de trajetória.
    struct SimResult {
        bool    hit      = false;  // a trajetória cruzou o plano do gol?
        float   crossX   = 0.f;   // local X no plano do gol no momento do cruzamento
        float   crossY   = 0.f;   // local Y no plano do gol no momento do cruzamento
        float   timeToGoal = 0.f; // tempo estimado até cruzar o plano (s)
        Vector3 velAtCross;        // velocidade da bola no cruzamento (espaço mundo)
    };

    // Simula a trajetória da bola com gravidade, drag e Magnus force (spin/curva).
    // Integração de Euler simples com cfg.simSteps * cfg.simDt segundos totais.
    // Retorna o ponto previsto de cruzamento com o plano frontal do gol.
    SimResult SimulateBallPath(const BallState& ball, const GoalState& goal) const;

    bool IsBallTargetingGoal(const BallState& ball, const GoalState& goal, SimResult* outSim = nullptr) const;
    bool IsBallHittingGK(const BallState& ball, const GKState& gk) const;

    // Loop da thread de scan
    void ScanLoop(RobloxReader* rbx);

    // Lógica de decisão (chamada pelo ScanLoop com cópias locais)
    void Evaluate(const GKState& gk, const BallState& ball, const GoalState& goal);

    std::chrono::steady_clock::time_point m_lastDiveTime;
    bool        m_firedThisFrame = false;
    const char* m_lastKey        = "-";

    // WatchRange state — mantidos entre frames do ScanLoop
    bool        m_watchActive    = false;  // bola está dentro do watchRange
    bool        m_trajectoryOK   = false;  // simulação confirmou trajetória para o gol
    SimResult   m_lastSim;                 // último resultado de simulação

    std::thread       m_thread;
    std::atomic<bool> m_running{ false };
};
