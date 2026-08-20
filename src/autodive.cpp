#include "autodive.h"
#include <cmath>

// --------------------------------------------------------------------------
// BallApproaching
//
// A bola está "chegando" se o dot product entre a velocidade dela e o vetor
// do GK até a bola for negativo — ou seja, a bola vai na direção do GK.
//
// Também verifica distância mínima pra não agir quando a bola está longe.
// --------------------------------------------------------------------------
bool AutoDive::BallApproaching(const GKState& gk, const BallState& ball) const
{
    Vector3 toBall = ball.position - gk.position;
    float   dist   = toBall.Length();

    if (dist > cfg.triggerDistance || dist < 2.f)
        return false;

    // Normaliza o vetor GK→bola
    if (dist < 0.001f) return false;
    Vector3 dir = toBall * (1.f / dist);

    // dot(velocidade_da_bola, direção_bola→GK)
    // Se positivo, a bola está indo em direção ao GK
    Vector3 toGK = { -dir.x, -dir.y, -dir.z };
    float   dot  = ball.velocity.Dot(toGK);

    return dot > 2.f; // mínimo de 2 studs/s em direção ao GK
}

// --------------------------------------------------------------------------
// PredictLateralOffset
//
// Usa cinemática simples (sem gravidade lateral) pra prever onde a bola
// vai estar lateralmente quando chegar na "linha de defesa" do GK.
//
// A "linha de defesa" é definida como a posição do GK projetada no look vector:
// prediz o tempo até a bola cruzar essa linha e aplica a velocidade lateral.
//
// Retorna o deslocamento lateral (positivo = direita do GK, negativo = esquerda).
// --------------------------------------------------------------------------
float AutoDive::PredictLateralOffset(const GKState& gk, const BallState& ball) const
{
    // Vetor do GK até a bola
    Vector3 delta = ball.position - gk.position;

    // Componente da velocidade da bola ao longo do lookVector do GK
    // (velocidade de aproximação na profundidade)
    float   approachSpeed = ball.velocity.Dot({ -gk.lookVector.x, -gk.lookVector.y, -gk.lookVector.z });
    if (approachSpeed < 0.1f) approachSpeed = 0.1f; // evita divisão por zero

    // Distância de profundidade (quanto falta pra bola chegar na linha do GK)
    float depthDist = delta.Dot({ -gk.lookVector.x, -gk.lookVector.y, -gk.lookVector.z });
    if (depthDist < 0.f) depthDist = 0.f;

    // Tempo previsto de chegada
    float timeToArrive = depthDist / approachSpeed;

    // Posição lateral futura da bola
    // lateral = posição lateral atual + velocidade lateral * tempo
    float currentLateral = delta.Dot(gk.rightVector);
    float lateralVelocity = ball.velocity.Dot(gk.rightVector);
    float predictedLateral = currentLateral + lateralVelocity * timeToArrive;

    return predictedLateral;
}

// --------------------------------------------------------------------------
// PressKey — simula keydown + keyup via SendInput
// --------------------------------------------------------------------------
void AutoDive::PressKey(WORD vk)
{
    INPUT inputs[2] = {};

    inputs[0].type       = INPUT_KEYBOARD;
    inputs[0].ki.wVk     = vk;
    inputs[0].ki.dwFlags = 0; // KEYDOWN

    inputs[1].type       = INPUT_KEYBOARD;
    inputs[1].ki.wVk     = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));
}

// --------------------------------------------------------------------------
// Update — lógica principal, chamada a cada frame
// --------------------------------------------------------------------------
void AutoDive::Update(const GKState& gk, const BallState& ball)
{
    m_firedThisFrame = false;

    if (!cfg.enabled)           return;
    if (!gk.isGK)               return;
    if (!ball.exists)           return;

    auto now = std::chrono::steady_clock::now();

    // --- Verifica se há um dive pendente esperando o delay de reação ---
    if (m_pendingFire)
    {
        float elapsed = std::chrono::duration<float>(now - m_pendingFireTime).count();
        if (elapsed >= m_pendingDelay)
        {
            m_pendingFire = false;

            // Decide a direção com base no offset lateral calculado
            if (m_pendingLateral > 0.5f)
            {
                PressKey('E'); // right dive
                m_lastKey = "E (Right)";
            }
            else if (m_pendingLateral < -0.5f)
            {
                PressKey('Q'); // left dive
                m_lastKey = "Q (Left)";
            }
            else
            {
                // Bola indo pro centro — não diva, ou pode ser um front dive
                // Por ora não faz nada (evita false positives)
                m_lastKey = "Center (skip)";
            }

            m_lastDiveTime  = now;
            m_firedThisFrame = true;
        }
        return; // enquanto pendente, não analisa novos frames
    }

    // --- Cooldown ---
    float sinceLast = std::chrono::duration<float>(now - m_lastDiveTime).count();
    if (sinceLast < cfg.cooldownSec) return;

    // --- Verifica se a bola está chegando ---
    if (!BallApproaching(gk, ball)) return;

    // --- Prediz o lado ---
    float lateral = PredictLateralOffset(gk, ball);

    // Ignora se lateral for muito pequeno (bola indo central)
    if (std::fabsf(lateral) < 0.5f) return;

    // --- Arma o disparo com delay de reação ---
    m_pendingFire     = true;
    m_pendingLateral  = lateral;
    m_pendingDelay    = cfg.reactionDelay;
    m_pendingFireTime = now;
}
