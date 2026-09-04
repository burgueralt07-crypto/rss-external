#pragma once
#include "rmath.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
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

    // Aceleração diferencial medida: (velAtual - velAnterior) / dt
    // Captura a curva REAL que o jogo está aplicando (qualquer mecanismo interno:
    // Magnus, forças scriptadas, spin forçado, etc.). Usada pelo SimulateBallPath
    // em vez do Magnus teórico para prever a trajetória com curva.
    // Calculada no ScanLoop a cada frame em que a bola está em voo livre.
    Vector3 measuredAccel;
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
        // Altura mínima de cruzamento (sim.crossY, espaço local do gol) para
        // considerar o chute "alto" e acionar Jump/Jump+Dive.
        // 0 = qualquer coisa acima do centro do gol (comportamento anterior).
        // Ex: 1.5 = só bolas que vão cruzar 1.5 studs acima do centro do gol.
        float jumpMinCrossY       = 1.5f;

        // ── Detecção de chute com curva que sobe ─────────────────────────
        // Complementa jumpMinCrossY para o caso em que a simulação subestima
        // a subida porque a curva ainda está sendo capturada pelo EMA.
        //
        // velAtCrossYMin: se sim.velAtCross.y >= este valor (bola ainda
        // subindo ao cruzar o plano do gol), força ballHigh=true mesmo que
        // crossY < jumpMinCrossY.
        // 0 = desativado. Valor sugerido: 5.0 (studs/s).
        float velAtCrossYMin      = 5.f;
        //
        // measuredAccelYMin: se ball.measuredAccel.y >= este valor (aceleração
        // vertical positiva medida no instante do disparo), força ballHigh=true.
        // Captura curvas que empurram a bola para cima logo após o chute.
        // 0 = desativado. Valor sugerido: 8.0 (studs/s²).
        float measuredAccelYMin   = 8.f;

        // ── Simulação de trajetória (RK4 + EMA + decaimento) ────────────
        int   simSteps            = 80;      // passos de integração RK4 (~2.8 s de lookahead a dt=0.035)
        float simDt               = 0.035f;  // dt por passo (s) — ~1.575 s de lookahead
        float gravity             = 156.96f; // workspace.Gravity * fator (studs/s²)
        // Coeficiente de Magnus — fallback quando measuredAccel não disponível.
        float magnusCoeff         = 0.12f;
        // Drag linear — fração da velocidade removida por segundo.
        float dragCoeff           = 0.004f;
        // Taxa de decaimento exponencial da curva por segundo.
        // Equivale a CurveDecayRate=0.85 do Lua: curva *= exp(-decayRate * t).
        float curveDecayRate      = 0.85f;
        // Suavização EMA da aceleração medida (0 = sem filtro, 1 = ignora novo valor).
        // Equivale a EMA_Alpha=0.30 do Lua.
        float emaAlpha            = 0.30f;

        // ── WatchRange — detecção antecipada ─────────────────────────────
        // A thread fica "de olho" na bola a partir desta distância.
        // Quando a trajetória simulada confirma que a bola vai no gol,
        // o dive é disparado assim que dist <= diveFireDistance.
        float watchRange          = 150.f;   // studs — começa a monitorar
        float diveFireDistance    = 18.f;    // studs — dispara o dive ao chegar aqui

        int   scanRate            = 240;     // scans por segundo
        // Duração do key-press em ms — quanto tempo a tecla fica pressionada.
        // 25ms = quase imperceptível; 80-120ms = mais humanizado.
        int   keyHoldMs           = 80;      // ms

        // ── Camera pre-rotation ───────────────────────────────────────────
        // Antes de pressionar Q/E, move o mouse lateralmente para rotacionar
        // a câmera/corpo do GK na direção do chute. Isso expande o alcance
        // do dive, pois o personagem já começa com parte da rotação feita.
        //
        // camPreRotate   : ativa/desativa a pré-rotação.
        // camRotAngle    : ângulo de rotação em graus (0–90). Valores típicos:
        //                  10–30° já fazem diferença perceptível.
        // camRotDelayMs  : delay (ms) entre o movimento de mouse e o key-down
        //                  do dive. Dá tempo do engine registrar a nova direção
        //                  antes do input da tecla. Sugerido: 20–60 ms.
        bool  camPreRotate        = false;
        float camRotAngle         = 20.f;    // graus
        int   camRotDelayMs       = 30;      // ms
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

        // Aceleração diferencial medida (curva real do jogo)
        float measuredAccelX = 0.f;
        float measuredAccelY = 0.f;
        float measuredAccelZ = 0.f;

        // Trajetória simulada — ponto previsto no plano do gol
        float predGoalX = 0.f;   // local X no espaço do gol onde a bola deve cruzar
        float predGoalY = 0.f;   // local Y no espaço do gol
        bool  simValid  = false; // true se a simulação encontrou cruzamento com o plano

        // WatchRange
        bool  inWatchRange  = false;  // bola está no range de monitoramento
        bool  trajectoryOK  = false;  // simulação diz que vai no gol
    } debug;

private:
    // Rotaciona a câmera via mouse relativo (MOUSEEVENTF_MOVE).
    // direction > 0 → direita, < 0 → esquerda.
    // Converte cfg.camRotAngle em pixels com base numa sensibilidade base de
    // 1 grau ≈ 8 counts (ajuste empírico para sens padrão do Roblox).
    // O retorno é síncrono: bloqueia cfg.camRotDelayMs ms antes de voltar
    // para que o engine registre a nova direção antes do key-down do dive.
    void RotateCamera(float direction);

    // Dispara key-down imediatamente via hardware scancode.
    // Enfileira o key-up para a thread dedicada (KeyUpLoop) soltar holdMs depois.
    // Não cria threads avulsas — uma única thread de key-up drena a fila.
    void PressKey(WORD vk, int holdMs = 80)
    {
        WORD sc = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));

        // Key-down imediato
        INPUT down = {};
        down.type       = INPUT_KEYBOARD;
        down.ki.wVk     = 0;
        down.ki.wScan   = sc;
        down.ki.dwFlags = KEYEVENTF_SCANCODE;
        SendInput(1, &down, sizeof(INPUT));

        // Enfileira key-up
        auto releaseAt = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(holdMs);
        {
            std::lock_guard<std::mutex> lk(m_keyUpMtx);
            m_keyUpQueue.push({ sc, releaseAt });
        }
        m_keyUpCv.notify_one();
    }

    static Vector3 PointToObjectSpace(const Vector3& origin,
                                      const Vector3& right,
                                      const Vector3& up,
                                      const Vector3& look,
                                      const Vector3& worldPos);

    // Resultado de uma simulação de trajetória.
    struct SimResult {
        bool    hit        = false;  // a trajetória cruzou o plano do gol?
        float   crossX     = 0.f;   // local X no plano do gol no momento do cruzamento
        float   crossY     = 0.f;   // local Y no plano do gol no momento do cruzamento
        float   timeToGoal = 0.f;   // tempo estimado até cruzar o plano (s)
        Vector3 velAtCross;          // velocidade da bola no cruzamento (espaço mundo)
        Vector3 worldCross;          // posição 3D do cruzamento no espaço mundo
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

    // Estado anterior da bola — usado para calcular aceleração diferencial
    // (velAtual - velAnterior) / dt, que captura a curva real do jogo
    Vector3     m_prevBallVel;             // velocidade no frame anterior
    bool        m_prevBallValid = false;   // false no primeiro frame ou após reset
    std::chrono::steady_clock::time_point m_prevBallTime;

    // Filtro EMA da aceleração medida — suaviza o ruído de Δv/Δt a 240 Hz.
    // Persistido entre frames do ScanLoop; zerado quando a bola para/é presa.
    // Equivale a filteredAccel do script Lua (EMA_Alpha = cfg.emaAlpha).
    Vector3     m_filteredAccel;           // aceleração filtrada (EMA)
    bool        m_filteredAccelValid = false; // false até ter ao menos uma medição

    std::thread       m_thread;
    std::atomic<bool> m_running{ false };

    // Thread dedicada ao key-up — drena m_keyUpQueue sem criar threads avulsas
    struct KeyUpEntry {
        WORD                                     scancode;
        std::chrono::steady_clock::time_point    releaseAt;
    };
    std::thread                m_keyUpThread;
    std::mutex                 m_keyUpMtx;
    std::condition_variable    m_keyUpCv;
    std::queue<KeyUpEntry>     m_keyUpQueue;

    void KeyUpLoop();
};
