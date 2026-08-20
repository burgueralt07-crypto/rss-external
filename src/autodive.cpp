#include "autodive.h"
#include <cmath>

// --------------------------------------------------------------------------
// BallApproaching — verifica se a bola está se aproximando do gol que o GK defende
// --------------------------------------------------------------------------
bool AutoDive::BallApproaching(const GKState& gk, const BallState& ball, const GoalState& goal) const
{
    if (!ball.exists || !goal.exists) return false;

    // Se o gol é mais largo no eixo X (orientação padrão do RSS)
    if (goal.size.x >= goal.size.z)
    {
        float dz = goal.position.z - ball.position.z;
        float vz = ball.velocity.z;

        if (std::fabsf(vz) < cfg.approachMinSpeed) return false;

        // Se dz e vz têm o mesmo sinal, a bola está indo em direção à linha do gol (Z)
        return (dz * vz > 0.f);
    }
    else // O gol é mais largo no eixo Z (campo orientado em X)
    {
        float dx = goal.position.x - ball.position.x;
        float vx = ball.velocity.x;

        if (std::fabsf(vx) < cfg.approachMinSpeed) return false;

        // Se dx e vx têm o mesmo sinal, a bola está indo em direção à linha do gol (X)
        return (dx * vx > 0.f);
    }
}

// --------------------------------------------------------------------------
// PredictBallAtGoalLine
//
// Prevê a posição lateral da bola quando ela cruzar a linha de gol.
// Suporta de forma robusta campos orientados tanto em Z quanto em X.
// --------------------------------------------------------------------------
float AutoDive::PredictBallAtGoalLine(const BallState& ball, const GoalState& goal) const
{
    if (!ball.exists || !goal.exists) return 0.f;

    // Se o gol é mais largo no eixo X (campo orientado em Z, gol no plano Z fixo)
    if (goal.size.x >= goal.size.z)
    {
        float goalZ = goal.position.z;
        float dz    = goalZ - ball.position.z;
        float vz    = ball.velocity.z;

        if (std::fabsf(vz) < 0.5f)
        {
            return ball.position.x;
        }

        float t = dz / vz;
        if (t < 0.f) t = 0.f;
        if (t > 3.f) t = 3.f;

        return ball.position.x + ball.velocity.x * t;
    }
    else // O gol é mais largo no eixo Z (campo orientado em X, gol no plano X fixo)
    {
        float goalX = goal.position.x;
        float dx    = goalX - ball.position.x;
        float vx    = ball.velocity.x;

        if (std::fabsf(vx) < 0.5f)
        {
            return ball.position.z;
        }

        float t = dx / vx;
        if (t < 0.f) t = 0.f;
        if (t > 3.f) t = 3.f;

        return ball.position.z + ball.velocity.z * t;
    }
}

// --------------------------------------------------------------------------
// PressKey
// --------------------------------------------------------------------------
void AutoDive::PressKey(WORD vk)
{
    INPUT inputs[2] = {};

    inputs[0].type       = INPUT_KEYBOARD;
    inputs[0].ki.wVk     = vk;
    inputs[0].ki.dwFlags = 0;

    inputs[1].type       = INPUT_KEYBOARD;
    inputs[1].ki.wVk     = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));
}

// --------------------------------------------------------------------------
// Update
// --------------------------------------------------------------------------
void AutoDive::Update(const GKState& gk, const BallState& ball, const GoalState& goal)
{
    m_firedThisFrame = false;
    debug = {};

    if (!cfg.enabled)    { debug.blockReason = "disabled";     return; }
    if (!gk.isGK)        { debug.blockReason = "not GK";       return; }
    if (!ball.exists)    { debug.blockReason = "no ball";       return; }
    if (!goal.exists)    { debug.blockReason = "no goal";       return; }

    auto now = std::chrono::steady_clock::now();

    // --- Processa dive pendente (delay de reação) ---
    if (m_pendingFire)
    {
        float elapsed = std::chrono::duration<float>(now - m_pendingFireTime).count();
        if (elapsed >= cfg.reactionDelay)
        {
            m_pendingFire    = false;
            m_firedThisFrame = true;
            m_lastDiveTime   = now;

            if (m_pendingLateral > 0.f)
            {
                PressKey('E');
                m_lastKey = "E (Right)";
            }
            else
            {
                PressKey('Q');
                m_lastKey = "Q (Left)";
            }
        }
        debug.blockReason = "pending fire...";
        return;
    }

    // --- Cooldown ---
    float sinceLast = std::chrono::duration<float>(now - m_lastDiveTime).count();
    if (sinceLast < cfg.cooldownSec)
    {
        debug.blockReason = "cooldown";
        return;
    }

    // --- Preenche debug ---
    Vector3 toBall = ball.position - gk.position;
    float   dist   = toBall.Length();
    debug.distToBall = dist;

    // Pega o centro do gol dependendo da orientação
    if (goal.size.x >= goal.size.z)
    {
        debug.goalCenterX = goal.position.x;
    }
    else
    {
        debug.goalCenterX = goal.position.z;
    }

    if (dist > 0.001f)
    {
        Vector3 dir  = toBall * (1.f / dist);
        Vector3 toGK = { -dir.x, -dir.y, -dir.z };
        debug.approachDot = ball.velocity.Dot(toGK);
    }

    debug.approaching = BallApproaching(gk, ball, goal);

    // --- Verifica distância ---
    if (dist > cfg.triggerDistance)
    {
        debug.blockReason = "too far";
        return;
    }

    // --- Verifica aproximação ---
    if (!debug.approaching)
    {
        debug.blockReason = "not approaching";
        return;
    }

    // --- Prediz lado ---
    float predictedLateral = PredictBallAtGoalLine(ball, goal);
    float lateral = 0.f;

    if (goal.size.x >= goal.size.z)
    {
        lateral = predictedLateral - goal.position.x; // positivo = direita do gol, negativo = esquerda
    }
    else
    {
        lateral = predictedLateral - goal.position.z;
    }

    debug.predictedX    = predictedLateral;
    debug.lateralOffset = lateral;

    // Threshold mínimo — ignora se a bola vai pro centro do gol
    // Usa metade da largura do gol como referência para "lado"
    float halfGoalWidth = (goal.size.x >= goal.size.z) ? goal.size.x * 0.5f : goal.size.z * 0.5f;
    float threshold     = halfGoalWidth * 0.15f; // 15% da metade = evita false positives centrais
    if (threshold < 0.5f) threshold = 0.5f;

    if (std::fabsf(lateral) < threshold)
    {
        debug.blockReason = "ball going center";
        return;
    }

    // --- Arma o disparo ---
    m_pendingFire     = true;
    m_pendingLateral  = lateral;
    m_pendingFireTime = now;
    debug.blockReason = "FIRED";
}
