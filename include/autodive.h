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
    Vector3 position; // centro do gol (Primitive::Position)
    Vector3 size;     // tamanho (Primitive::Size)
};

// --------------------------------------------------------------------------
// AutoDive
//
// Lógica baseada no gol — mais robusta que depender dos vetores do GK:
//   1. Sabe qual gol o GK defende (APG = AwayGoal, HPG = HomeGoal)
//   2. Prediz onde a bola vai cruzar a linha do gol (usando velocidade)
//   3. Compara com o centro do gol pra saber se é esquerda ou direita
//   4. Pressiona Q (left) ou E (right) via SendInput
// --------------------------------------------------------------------------
class AutoDive {
public:
    struct Config {
        bool  enabled         = false;
        bool  forceGK         = false;  // Forçar estado de GK mesmo sem pasta Bools
        float triggerDistance = 40.f;  // studs — distância máxima pra ativar
        float reactionDelay   = 0.05f; // segundos de delay (simula reação)
        float cooldownSec     = 1.2f;  // cooldown entre dives
        float approachMinSpeed = 5.f;  // velocidade mínima de aproximação (studs/s)
    };

    Config cfg;

    void Update(const GKState& gk, const BallState& ball, const GoalState& goal);

    const char* LastDiveKey()    const { return m_lastKey; }
    bool        DiveFired()      const { return m_firedThisFrame; }

    // Debug — exposto pro menu
    struct DebugInfo {
        float distToBall   = 0.f;
        float approachDot  = 0.f;
        float predictedX   = 0.f; // posição lateral prevista da bola na linha do gol
        float goalCenterX  = 0.f;
        float lateralOffset = 0.f;
        bool  approaching  = false;
        std::string blockReason; // por que não deu dive
    } debug;

// Exposto para poder usar no ESP (linhas de ajuda visual se quisermos)
    float PredictBallAtGoalLine(const BallState& ball, const GoalState& goal) const;

private:
    bool  BallApproaching(const GKState& gk, const BallState& ball, const GoalState& goal) const;
    void  PressKey(WORD vk);

    std::chrono::steady_clock::time_point m_lastDiveTime;
    std::chrono::steady_clock::time_point m_pendingFireTime;
    bool        m_firedThisFrame = false;
    bool        m_pendingFire    = false;
    float       m_pendingLateral = 0.f;
    const char* m_lastKey        = "-";
};
