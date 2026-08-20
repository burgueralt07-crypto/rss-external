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
    Vector3 position; // centro do gol (GetBoundingBox para Model, Primitive::Position para Part)
    Vector3 size;     // tamanho (GetBoundingBox para Model, Primitive::Size para Part)
};

// --------------------------------------------------------------------------
// AutoDive
//
// Lógica baseada no script Lua que funciona:
//   1. Detecta qual gol o GK defende (APG = AwayGoal, HPG = HomeGoal)
//   2. Converte posição da bola para espaço LOCAL do GK (relPos)
//   3. Thresholds simples e eficazes:
//      - relPos.X > 3  → Right Dive (E)
//      - relPos.X < -3 → Left Dive (Q)
//      - |relPos.X| ≤ 3 e relPos.Z < 0 → Front Dive
//      - relPos.Y >= 5.5 e |relPos.X| ≤ 6 → High Jump
//   4. Verifica se bola está vindo pro gol (dot product com direção do gol)
//   5. Cooldown, distância mínima, velocidade mínima da bola
// --------------------------------------------------------------------------
class AutoDive {
public:
    struct Config {
        bool  enabled         = false;
        bool  forceGK         = false;   // Forçar estado de GK mesmo sem pasta Bools
        float triggerDistance = 18.f;    // studs — distância máxima pra ativar (Lua default: 18)
        float reactionDelay   = 0.05f;   // segundos de delay (simula reação)
        float cooldownSec     = 1.2f;    // cooldown entre dives (Lua default: 1.2)
        float minBallSpeed    = 8.f;     // velocidade mínima da bola (Lua default: 8)
        float goalMargin      = 2.f;     // margem extra além da trave (Lua default: 2)
    };

    Config cfg;

    void Update(const GKState& gk, const BallState& ball, const GoalState& goal);

    const char* LastDiveKey()    const { return m_lastKey; }
    bool        DiveFired()      const { return m_firedThisFrame; }

    // Debug — exposto pro menu
    struct DebugInfo {
        float distToBall   = 0.f;
        float approachDot  = 0.f;
        float relPosX      = 0.f;  // posição relativa da bola no espaço do GK
        float relPosZ      = 0.f;
        float relPosY      = 0.f;
        bool  approaching  = false;
        std::string blockReason; // por que não deu dive
        
        // Debug extra para diagnosticar
        float ballPosX = 0.f;
        float ballPosZ = 0.f;
        float ballVelX = 0.f;
        float ballVelZ = 0.f;
        float goalPosX = 0.f;
        float goalPosZ = 0.f;
        float goalSizeX = 0.f;
        float goalSizeZ = 0.f;
        bool  isAPG    = false;
    } debug;

private:
    bool  IsBallTargetingGoal(const GKState& gk, const BallState& ball, const GoalState& goal) const;
    void  PressKey(WORD vk);
    Vector3 GetGoalCenter(const GoalState& goal) const;

    std::chrono::steady_clock::time_point m_lastDiveTime;
    bool        m_firedThisFrame = false;
    const char* m_lastKey        = "-";
};
