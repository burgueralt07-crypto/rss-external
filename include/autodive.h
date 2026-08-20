#pragma once
#include "rmath.h"
#include <Windows.h>
#include <chrono>
#include <string>

// --------------------------------------------------------------------------
// GKState — estado do goleiro lido da memória a cada frame
// --------------------------------------------------------------------------
struct GKState {
    bool    isGK     = false;
    Vector3 position;
    // CFrame do GK (rotação) — necessário para PointToObjectSpace
    // Armazenamos os vetores Right, Up, Look que compõem a rotação
    Vector3 rightVec  = { 1.f, 0.f,  0.f };
    Vector3 upVec     = { 0.f, 1.f,  0.f };
    Vector3 lookVec   = { 0.f, 0.f, -1.f }; // forward do personagem
};

// --------------------------------------------------------------------------
// BallState — estado da bola lido da memória a cada frame
// --------------------------------------------------------------------------
struct BallState {
    bool    exists   = false;
    bool    isWelded = false;
    Vector3 position;
    Vector3 velocity; // AssemblyLinearVelocity
};

// --------------------------------------------------------------------------
// GoalState — posição e tamanho do gol que o GK defende
// --------------------------------------------------------------------------
struct GoalState {
    bool    exists   = false;
    Vector3 position; // centro do gol
    Vector3 size;     // tamanho (X = largura, Y = altura, Z = profundidade)
    // Eixos do gol para PointToObjectSpace — caso o gol seja rotacionado
    Vector3 rightVec  = { 1.f, 0.f, 0.f };
    Vector3 upVec     = { 0.f, 1.f, 0.f };
    Vector3 lookVec   = { 0.f, 0.f, 1.f };
};

// --------------------------------------------------------------------------
// AutoDive
//
// Lógica portada do script Lua:
//   1. isGoalie()          — APG/HPG via pasta Bools, ou forceGK por proximidade
//   2. isBallTargetingGoal — simulação física com gravidade (45 steps x dt=0.035)
//                            verifica se trajetória cruza volume da trave + margem
//   3. relPos              — root.CFrame:PointToObjectSpace(ballPos)
//                            usando vetores Right/Up/Look do GK
//   4. Decisão de dive:
//      - relPos.Y >= 5.5 e |relPos.X| <= 6  → Jump    (VK_SPACE)
//      - relPos.X > 3                        → Right   ('E')
//      - relPos.X < -3                       → Left    ('Q')
//      - |relPos.X| <= 3 e relPos.Z < 0      → Front   ('F')
//   5. Cooldown, distância máxima, velocidade mínima da bola
//   6. Bola welded (playerWeld) → ignora
// --------------------------------------------------------------------------
class AutoDive {
public:
    struct Config {
        bool  enabled         = false;
        bool  forceGK         = false;  // ignorar pasta Bools, detectar por proximidade
        bool  onlyInGoal      = true;   // só age se simulação prevê bola no gol
        bool  lowDive         = true;   // diferencia bola rasteira (ball.Y <= 3 após vel)
        bool  highJump        = true;   // ativa GKJump para bolas altas no centro
        float triggerDistance = 18.f;   // distância máxima GK-bola (studs)
        float cooldownSec     = 1.2f;   // cooldown entre dives (seg)
        float minBallSpeed    = 8.f;    // velocidade mínima da bola (studs/s)
        float goalMargin      = 2.f;    // margem extra além da trave (studs)
        int   simSteps        = 45;     // passos de simulação de trajetória
        float simDt           = 0.035f; // passo de tempo da simulação (seg)
        float gravity         = 196.2f; // workspace.Gravity * 0.8 (~196 studs/s²)
    };

    Config cfg;

    void Update(const GKState& gk, const BallState& ball, const GoalState& goal);

    const char* LastDiveKey() const { return m_lastKey; }
    bool        DiveFired()   const { return m_firedThisFrame; }

    // Debug — exposto pro menu ImGui
    struct DebugInfo {
        float distToBall   = 0.f;
        float relPosX      = 0.f;
        float relPosY      = 0.f;
        float relPosZ      = 0.f;
        bool  approaching  = false;   // isBallTargetingGoal
        bool  isAPG        = false;
        std::string blockReason;

        // valores brutos para diagnóstico
        float ballPosX  = 0.f, ballPosZ  = 0.f;
        float ballVelX  = 0.f, ballVelZ  = 0.f;
        float goalPosX  = 0.f, goalPosZ  = 0.f;
        float goalSizeX = 0.f, goalSizeZ = 0.f;
    } debug;

private:
    // Converte worldPos para espaço local usando os vetores do CFrame
    static Vector3 PointToObjectSpace(const Vector3& origin,
                                      const Vector3& right,
                                      const Vector3& up,
                                      const Vector3& look,
                                      const Vector3& worldPos);

    // Simulação de trajetória com gravidade — retorna true se bola vai entrar no gol
    bool IsBallTargetingGoal(const BallState& ball, const GoalState& goal) const;

    static void PressKey(WORD vk);

    std::chrono::steady_clock::time_point m_lastDiveTime;
    bool        m_firedThisFrame = false;
    const char* m_lastKey        = "-";
};
