#include "autodive.h"
#include <cmath>

// --------------------------------------------------------------------------
// BallApproaching — verifica se a bola está se aproximando do GK
// --------------------------------------------------------------------------
bool AutoDive::BallApproaching(const GKState& gk, const BallState& ball) const
{
    Vector3 toBall = ball.position - gk.position;
    float   dist   = toBall.Length();

    if (dist < 0.001f) return false;

    // Normaliza
    Vector3 dir    = toBall * (1.f / dist);

    // Dot entre velocidade da bola e direção bola→GK
    // Se positivo, a bola está se movendo em direção ao GK
    Vector3 toGK   = { -dir.x, -dir.y, -dir.z };
    float   dot    = ball.velocity.Dot(toGK);

    return dot > cfg.approachMinSpeed;
}

// --------------------------------------------------------------------------
// PredictBallAtGoalLine
//
// O campo do RSS é orientado no eixo Z — o gol fica numa posição Z fixa.
// Prevê a posição X da bola quando ela cruzar o Z do gol.
//
// Se a velocidade Z for quase zero, usa a posição X atual da bola.
// --------------------------------------------------------------------------
float AutoDive::PredictBallAtGoalLine(const BallState& ball, const GoalState& goal) const
{
    float goalZ = goal.position.z;
    float dz    = goalZ - ball.position.z;

    // Velocidade Z da bola
    float vz = ball.velocity.z;

    if (std::fabsf(vz) < 0.5f)
    {
        // Bola quase sem velocidade Z — usa posição X atual
        return ball.position.x;
    }

    // Tempo até cruzar a linha Z do gol
    float t = dz / vz;

    // Se tempo negativo, a bola já passou ou está indo pra direção errada
    // Clamp: máximo 3 segundos de predição pra não exagerar
    if (t < 0.f) t = 0.f;
    if (t > 3.f) t = 3.f;

    // Posição X prevista
    return ball.position.x + ball.velocity.x * t;
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
    debug.goalCenterX = goal.position.x;

    if (dist > 0.001f)
    {
        Vector3 dir  = toBall * (1.f / dist);
        Vector3 toGK = { -dir.x, -dir.y, -dir.z };
        debug.approachDot = ball.velocity.Dot(toGK);
    }

    debug.approaching = BallApproaching(gk, ball);

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
    float predictedX = PredictBallAtGoalLine(ball, goal);
    float lateral    = predictedX - goal.position.x; // positivo = direita do gol, negativo = esquerda

    debug.predictedX    = predictedX;
    debug.lateralOffset = lateral;

    // Threshold mínimo — ignora se a bola vai pro centro do gol
    // Usa metade da largura do gol como referência para "lado"
    float halfGoalWidth = goal.size.x * 0.5f;
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
