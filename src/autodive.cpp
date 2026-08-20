#include "autodive.h"
#include <cmath>

// --------------------------------------------------------------------------
// GetGoalCenter — retorna o centro do gol
// --------------------------------------------------------------------------
Vector3 AutoDive::GetGoalCenter(const GoalState& goal) const
{
    return goal.position;
}

// --------------------------------------------------------------------------
// IsBallTargetingGoal — verifica se a bola está indo em direção ao gol
// Baseado na lógica do script Lua: dot product entre velocidade da bola
// e vetor do gol para a bola deve ser > 0
// --------------------------------------------------------------------------
bool AutoDive::IsBallTargetingGoal(const GKState& gk, const BallState& ball, const GoalState& goal) const
{
    if (!ball.exists || !goal.exists) return false;

    Vector3 goalCenter = GetGoalCenter(goal);
    
    // Vetor do gol para a bola
    Vector3 toBall = ball.position - goalCenter;
    
    // Se a velocidade da bola tem componente na direção do gol (dot > 0)
    // significa que a bola está indo PRO gol
    float dot = ball.velocity.Dot(toBall);
    
    // Também verifica velocidade mínima
    float speed = std::sqrtf(ball.velocity.x * ball.velocity.x + 
                             ball.velocity.y * ball.velocity.y + 
                             ball.velocity.z * ball.velocity.z);
    
    return (dot > 0.f) && (speed >= cfg.minBallSpeed);
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

    // --- Cooldown ---
    float sinceLast = std::chrono::duration<float>(now - m_lastDiveTime).count();
    if (sinceLast < cfg.cooldownSec)
    {
        debug.blockReason = "cooldown";
        return;
    }

    // --- Distância GK-Bola ---
    Vector3 toBall = ball.position - gk.position;
    float   dist   = toBall.Length();
    debug.distToBall = dist;

    if (dist > cfg.triggerDistance)
    {
        debug.blockReason = "too far";
        return;
    }

    // --- Velocidade da bola ---
    float ballSpeed = std::sqrtf(ball.velocity.x * ball.velocity.x + 
                                 ball.velocity.y * ball.velocity.y + 
                                 ball.velocity.z * ball.velocity.z);
    
    if (ballSpeed < cfg.minBallSpeed)
    {
        debug.blockReason = "ball too slow";
        return;
    }

    // --- Verifica se bola está indo pro gol ---
    bool targetingGoal = IsBallTargetingGoal(gk, ball, goal);
    debug.approaching = targetingGoal;
    
    if (!targetingGoal)
    {
        debug.blockReason = "not targeting goal";
        return;
    }

    // --- Converte posição da bola para espaço LOCAL do GK ---
    // O GK olha para o gol, então:
    // - Forward (LookVector) = direção do GK para o gol
    // - Right = perpendicular ao forward no plano XZ
    // - Up = Y
    
    Vector3 goalCenter = GetGoalCenter(goal);
    Vector3 gkToGoal = goalCenter - gk.position;
    float gkToGoalLen = gkToGoal.Length();
    
    if (gkToGoalLen < 0.001f)
    {
        debug.blockReason = "gk at goal center?";
        return;
    }
    
    // Forward = direção normalizada do GK para o gol
    Vector3 forward = { gkToGoal.x / gkToGoalLen, 0.f, gkToGoal.z / gkToGoalLen };
    
    // Right = perpendicular no plano XZ (rotaciona 90 graus no sentido horário)
    Vector3 right = { forward.z, 0.f, -forward.x };
    
    // Up = Y
    Vector3 up = { 0.f, 1.f, 0.f };
    
    // Posição relativa da bola no espaço do GK
    Vector3 relPos = {
        toBall.Dot(right),   // X local: positivo = direita, negativo = esquerda
        toBall.Dot(up),      // Y local: altura
        toBall.Dot(forward)  // Z local: positivo = frente (em direção ao gol), negativo = atrás
    };
    
    debug.relPosX = relPos.x;
    debug.relPosY = relPos.y;
    debug.relPosZ = relPos.z;

    // --- Debug info ---
    debug.ballPosX = ball.position.x;
    debug.ballPosZ = ball.position.z;
    debug.ballVelX = ball.velocity.x;
    debug.ballVelZ = ball.velocity.z;
    debug.goalPosX = goal.position.x;
    debug.goalPosZ = goal.position.z;
    debug.goalSizeX = goal.size.x;
    debug.goalSizeZ = goal.size.z;

    // --- Lógica de decisão baseada no script Lua ---
    // Thresholds do Lua:
    // - relPos.X > 3  → Right Dive (E)
    // - relPos.X < -3 → Left Dive (Q)
    // - |relPos.X| ≤ 3 e relPos.Z < 0 → Front Dive
    // - relPos.Y >= 5.5 e |relPos.X| ≤ 6 → High Jump

    const float DIVE_THRESHOLD_X = 3.0f;
    const float HIGH_JUMP_Y = 5.5f;
    const float HIGH_JUMP_X_MAX = 6.0f;

    // High Jump - bola alta no centro
    if (relPos.y >= HIGH_JUMP_Y && std::fabsf(relPos.x) <= HIGH_JUMP_X_MAX)
    {
        // Por enquanto não implementamos Jump (precisaria de tecla Space)
        // Mas logamos para debug
        debug.blockReason = "high jump (not implemented)";
        return;
    }

    // Right Dive (E)
    if (relPos.x > DIVE_THRESHOLD_X)
    {
        PressKey('E');
        m_lastKey = "E (Right)";
        m_firedThisFrame = true;
        m_lastDiveTime = now;
        debug.blockReason = "FIRED - Right";
        return;
    }

    // Left Dive (Q)
    if (relPos.x < -DIVE_THRESHOLD_X)
    {
        PressKey('Q');
        m_lastKey = "Q (Left)";
        m_firedThisFrame = true;
        m_lastDiveTime = now;
        debug.blockReason = "FIRED - Left";
        return;
    }

    // Front Dive - bola vindo no centro (|X| <= 3) e na frente (Z < 0)
    if (std::fabsf(relPos.x) <= DIVE_THRESHOLD_X && relPos.z < 0.f)
    {
        // Front dive seria uma tecla diferente, por enquanto logamos
        debug.blockReason = "front dive (not implemented)";
        return;
    }

    // Se chegou aqui, bola está no centro mas não na frente (atrás ou parado)
    debug.blockReason = "ball not in dive zone";
}
