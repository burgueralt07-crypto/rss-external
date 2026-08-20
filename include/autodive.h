#pragma once
#include "rmath.h"
#include <Windows.h>
#include <chrono>

// --------------------------------------------------------------------------
// GKState — estado do goleiro lido da memória a cada frame
// --------------------------------------------------------------------------
struct GKState {
    bool    isGK        = false;   // local player é goleiro (APG ou HPG)
    Vector3 position;              // HumanoidRootPart.Position
    Vector3 rightVector;           // HumanoidRootPart right vector (coluna X da rotação)
    Vector3 lookVector;            // HumanoidRootPart look vector (coluna -Z da rotação)
};

// --------------------------------------------------------------------------
// BallState — estado da bola lido da memória a cada frame
// --------------------------------------------------------------------------
struct BallState {
    bool    exists   = false;
    Vector3 position;
    Vector3 velocity; // AssemblyLinearVelocity
};

// --------------------------------------------------------------------------
// AutoDive — decide e executa dives automáticos para o goleiro
//
// Lógica:
//   1. Só age se isGK == true
//   2. A bola precisa estar se aproximando (velocidade em direção ao GK)
//   3. Prediz onde a bola vai cruzar a linha do GK (eixo Z ou X dependendo
//      da orientação do campo)
//   4. Calcula o lado (dot com rightVector do GK)
//   5. Pressiona Q (left) ou E (right) via SendInput
//   6. Cooldown interno de 1.2s para evitar spam (alinhado com o debounce
//      do servidor que é ~1s)
// --------------------------------------------------------------------------
class AutoDive {
public:
    struct Config {
        bool  enabled         = false;
        float triggerDistance = 30.f;  // studs — distância máxima pra ativar
        float reactionDelay   = 0.08f; // segundos de delay artificial (simula reação humana)
        float cooldownSec     = 1.2f;  // cooldown entre dives
    };

    Config cfg;

    // Chama a cada frame com os dados atuais
    void Update(const GKState& gk, const BallState& ball);

    // Retorna true se um dive foi executado neste frame
    bool DiveFiredThisFrame() const { return m_firedThisFrame; }

    // Para debug/UI: qual tecla foi pressionada por último
    const char* LastDiveKey() const { return m_lastKey; }

private:
    // Prediz a posição lateral da bola quando ela chega na profundidade do GK.
    // Retorna o deslocamento lateral em relação ao GK (positivo = direita, negativo = esquerda)
    float PredictLateralOffset(const GKState& gk, const BallState& ball) const;

    // Verifica se a bola está se aproximando do GK
    bool BallApproaching(const GKState& gk, const BallState& ball) const;

    void PressKey(WORD vk);

    std::chrono::steady_clock::time_point m_lastDiveTime;
    bool        m_firedThisFrame = false;
    bool        m_pendingFire    = false;
    float       m_pendingLateral = 0.f;
    float       m_pendingDelay   = 0.f;
    const char* m_lastKey        = "-";

    std::chrono::steady_clock::time_point m_pendingFireTime;
};
