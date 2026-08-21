#include "autodive.h"
#include "rbx.h"
#include <cmath>

// --------------------------------------------------------------------------
void AutoDive::Start(RobloxReader* rbx)
{
    Stop();
    m_running = true;
    m_thread  = std::thread(&AutoDive::ScanLoop, this, rbx);
}

void AutoDive::Stop()
{
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();
}

// --------------------------------------------------------------------------
// ScanLoop — roda em thread dedicada
//
// Lê memória diretamente a cada iteração (ReadBallDirect / ReadGKDirect /
// ReadGoalDirect) sem depender do render loop. Isso garante dados frescos
// e disparo de tecla no microsegundo exato da detecção.
// --------------------------------------------------------------------------
void AutoDive::ScanLoop(RobloxReader* rbx)
{
    while (m_running)
    {
        if (cfg.enabled)
        {
            GKState   gk   = rbx->ReadGKDirect();
            BallState ball = rbx->ReadBallDirect();
            GoalState goal = rbx->ReadGoalDirect();
            Evaluate(gk, ball, goal);
        }

        int rate = cfg.scanRate > 0 ? cfg.scanRate : 240;
        std::this_thread::sleep_for(std::chrono::microseconds(1'000'000 / rate));
    }
}

// --------------------------------------------------------------------------
// PointToObjectSpace — equivalente a CFrame:PointToObjectSpace() do Roblox
// --------------------------------------------------------------------------
Vector3 AutoDive::PointToObjectSpace(const Vector3& origin,
                                     const Vector3& right,
                                     const Vector3& up,
                                     const Vector3& look,
                                     const Vector3& worldPos)
{
    Vector3 delta = worldPos - origin;
    return { delta.Dot(right), delta.Dot(up), delta.Dot(look) };
}

// --------------------------------------------------------------------------
// IsBallTargetingGoal — projeção linear da trajetória até o plano do gol
//
// Projeta a velocidade da bola e calcula onde ela cruza o plano frontal
// do gol (localZ == 0 no espaço local do gol). Se o ponto de cruzamento
// estiver dentro da abertura (com margem), a bola vai bater no gol.
// --------------------------------------------------------------------------
bool AutoDive::IsBallTargetingGoal(const BallState& ball, const GoalState& goal) const
{
    if (!cfg.onlyInGoal) return true;
    if (!ball.exists || !goal.exists) return true;

    // Posição e velocidade da bola no espaço local do gol
    Vector3 localPos = PointToObjectSpace(
        goal.position, goal.rightVec, goal.upVec, goal.lookVec, ball.position);

    // Velocidade projetada nos eixos locais do gol
    float velX = ball.velocity.Dot(goal.rightVec);
    float velY = ball.velocity.Dot(goal.upVec);
    float velZ = ball.velocity.Dot(goal.lookVec);

    // Bola precisa estar se movendo em direção ao plano (velZ e localZ com sinais opostos)
    if (velZ == 0.f) return false;
    float t = -localPos.z / velZ;
    if (t <= 0.f) return false;   // cruzamento no passado

    // Ponto de cruzamento no plano do gol
    float crossX = localPos.x + velX * t;
    float crossY = localPos.y + velY * t;

    float halfW = goal.size.x * 0.5f + cfg.goalMargin;
    float halfH = goal.size.y * 0.5f + cfg.goalMargin;

    return std::fabsf(crossX) <= halfW && std::fabsf(crossY) <= halfH;
}
}

// --------------------------------------------------------------------------
// Evaluate — lógica de decisão (chamada pelo ScanLoop)
//
// Espelho exato do Lua attemptAutoDive():
//   HighJump → Right (E) → Left (Q) → Front (F)
// --------------------------------------------------------------------------
void AutoDive::Evaluate(const GKState& gk, const BallState& ball, const GoalState& goal)
{
    m_firedThisFrame = false;
    debug = {};

    if (!cfg.enabled)  { debug.blockReason = "disabled";   return; }
    if (!gk.isGK)      { debug.blockReason = "not GK";     return; }
    if (!ball.exists)  { debug.blockReason = "no ball";     return; }
    if (ball.isWelded) { debug.blockReason = "ball welded"; return; }

    auto  now       = std::chrono::steady_clock::now();
    float sinceLast = std::chrono::duration<float>(now - m_lastDiveTime).count();
    if (sinceLast < cfg.cooldownSec) { debug.blockReason = "cooldown"; return; }

    float dist = (ball.position - gk.position).Length();
    debug.distToBall = dist;
    if (dist > cfg.triggerDistance) { debug.blockReason = "too far"; return; }

    float ballSpeed = ball.velocity.Length();
    if (ballSpeed < cfg.minBallSpeed) { debug.blockReason = "ball too slow"; return; }

    bool targeting = IsBallTargetingGoal(ball, goal);
    debug.approaching = targeting;
    debug.ballPosX  = ball.position.x; debug.ballPosZ  = ball.position.z;
    debug.ballVelX  = ball.velocity.x; debug.ballVelZ  = ball.velocity.z;
    debug.goalPosX  = goal.position.x; debug.goalPosZ  = goal.position.z;
    debug.goalSizeX = goal.size.x;     debug.goalSizeZ = goal.size.z;

    // LocalZ inicial da bola no espaço do gol (útil para debug do plano de cruzamento)
    if (goal.exists && ball.exists)
    {
        Vector3 lp = PointToObjectSpace(goal.position, goal.rightVec, goal.upVec, goal.lookVec, ball.position);
        debug.ballLocalZ = lp.z;
    }

    if (!targeting) { debug.blockReason = "not targeting goal"; return; }

    Vector3 relPos = PointToObjectSpace(
        gk.position, gk.rightVec, gk.upVec, gk.lookVec, ball.position);
    debug.relPosX = relPos.x;
    debug.relPosY = relPos.y;
    debug.relPosZ = relPos.z;

    // High Jump
    if (cfg.highJump && relPos.y >= 5.5f && std::fabsf(relPos.x) <= 6.f)
    {
        PressKey(VK_SPACE);
        m_lastKey = "Space (Jump)"; m_firedThisFrame = true; m_lastDiveTime = now;
        debug.blockReason = "FIRED - Jump";
        return;
    }

    // Right
    if (relPos.x > 3.f)
    {
        PressKey('E');
        m_lastKey = "E (Right)"; m_firedThisFrame = true; m_lastDiveTime = now;
        debug.blockReason = "FIRED - Right";
        return;
    }

    // Left
    if (relPos.x < -3.f)
    {
        PressKey('Q');
        m_lastKey = "Q (Left)"; m_firedThisFrame = true; m_lastDiveTime = now;
        debug.blockReason = "FIRED - Left";
        return;
    }

    // Front
    if (std::fabsf(relPos.x) <= 3.f && relPos.z < 0.f)
    {
        PressKey('F');
        m_lastKey = "F (Front)"; m_firedThisFrame = true; m_lastDiveTime = now;
        debug.blockReason = "FIRED - Front";
        return;
    }

    debug.blockReason = "ball not in dive zone";
}
