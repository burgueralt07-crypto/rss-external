#include "autodive.h"
#include "rbx.h"
#include <cmath>

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

void AutoDive::ScanLoop(RobloxReader* rbx)
{
    while (m_running)
    {
        if (cfg.enabled)
        {
            GKState   gk   = rbx->GetGKCopy();
            BallState ball = rbx->GetBallCopy();
            GoalState goal = rbx->GetGoalCopy();
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
// IsBallTargetingGoal — projeção linear simples
//
// Calcula onde a bola estará quando cruzar o plano Z do gol (lookVec) e
// verifica se esse ponto cai dentro da trave + margem.
// Sem simulação física, sem gravidade — direto ao ponto.
// --------------------------------------------------------------------------
bool AutoDive::IsBallTargetingGoal(const BallState& ball, const GoalState& goal) const
{
    if (!cfg.onlyInGoal) return true;
    if (!ball.exists || !goal.exists) return true;

    // Bola se afastando do gol → descarta
    Vector3 toGoal = goal.position - ball.position;
    if (ball.velocity.Dot(toGoal) <= 0.f) return false;

    // Posição e velocidade da bola no espaço local do gol
    Vector3 localPos = PointToObjectSpace(goal.position, goal.rightVec, goal.upVec, goal.lookVec, ball.position);
    Vector3 localVel = { ball.velocity.Dot(goal.rightVec),
                         ball.velocity.Dot(goal.upVec),
                         ball.velocity.Dot(goal.lookVec) };

    // Sem velocidade no eixo Z (profundidade) → não vai cruzar o plano do gol
    if (std::fabsf(localVel.z) < 0.001f) return false;

    // Tempo até cruzar o plano Z=0 do gol
    float t = -localPos.z / localVel.z;
    if (t < 0.f) return false; // já passou

    // Ponto de impacto no plano do gol
    float impactX = localPos.x + localVel.x * t;
    float impactY = localPos.y + localVel.y * t;

    float halfW = goal.size.x * 0.5f + cfg.goalMargin;
    float halfH = goal.size.y * 0.5f + cfg.goalMargin;

    debug.impactX = impactX;
    debug.impactY = impactY;

    return std::fabsf(impactX) <= halfW && impactY >= -cfg.goalMargin && impactY <= halfH;
}

// --------------------------------------------------------------------------
// Evaluate — decide o dive com base no ponto de impacto projetado
// --------------------------------------------------------------------------
void AutoDive::Evaluate(const GKState& gk, const BallState& ball, const GoalState& goal)
{
    m_firedThisFrame = false;
    debug = {};

    if (!cfg.enabled)  { debug.blockReason = "disabled";    return; }
    if (!gk.isGK)      { debug.blockReason = "not GK";      return; }
    if (!ball.exists)  { debug.blockReason = "no ball";      return; }
    if (ball.isWelded) { debug.blockReason = "ball welded";  return; }

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

    if (!targeting) { debug.blockReason = "not targeting goal"; return; }

    // Posição da bola no espaço local do GK
    Vector3 relPos = PointToObjectSpace(
        gk.position, gk.rightVec, gk.upVec, gk.lookVec, ball.position);
    debug.relPosX = relPos.x;
    debug.relPosY = relPos.y;
    debug.relPosZ = relPos.z;

    // Usa o ponto de impacto projetado (impactX) para decidir o lado,
    // mais preciso do que a posição atual da bola
    float sideX = debug.impactX; // X no espaço do gol → mesmo que X relativo ao GK se GK estiver centrado

    // High Jump — bola alta no centro
    if (cfg.highJump && relPos.y >= 5.5f && std::fabsf(relPos.x) <= 6.f)
    {
        PressKey(VK_SPACE);
        m_lastKey = "Space (Jump)"; m_firedThisFrame = true; m_lastDiveTime = now;
        debug.blockReason = "FIRED - Jump";
        return;
    }

    // Direita
    if (sideX > 1.5f)
    {
        PressKey('E');
        m_lastKey = "E (Right)"; m_firedThisFrame = true; m_lastDiveTime = now;
        debug.blockReason = "FIRED - Right";
        return;
    }

    // Esquerda
    if (sideX < -1.5f)
    {
        PressKey('Q');
        m_lastKey = "Q (Left)"; m_firedThisFrame = true; m_lastDiveTime = now;
        debug.blockReason = "FIRED - Left";
        return;
    }

    // Centro/frente
    PressKey('F');
    m_lastKey = "F (Front)"; m_firedThisFrame = true; m_lastDiveTime = now;
    debug.blockReason = "FIRED - Front";
}
