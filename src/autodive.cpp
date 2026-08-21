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
// IsBallTargetingGoal — previsão de trajetória até o plano do gol
//
// Lógica: simula a física da bola passo a passo e verifica se ela vai
// cruzar o plano frontal do gol (localZ passa de positivo para negativo
// no espaço local do gol). Quando esse cruzamento é detectado, interpola
// a posição exata no plano (localZ == 0) e verifica se está dentro da
// abertura do gol (com margem). Isso permite disparar o dive com
// antecedência — quando a bola ainda está a caminho, não quando já chegou.
// --------------------------------------------------------------------------
bool AutoDive::IsBallTargetingGoal(const BallState& ball, const GoalState& goal) const
{
    if (!cfg.onlyInGoal) return true;
    if (!ball.exists || !goal.exists) return true;

    // Descarta se a bola está se afastando do gol
    Vector3 toGoal = goal.position - ball.position;
    if (ball.velocity.Dot(toGoal) <= 0.f) return false;

    const float gravity = cfg.gravity;
    const float margin  = cfg.goalMargin;
    const float dt      = cfg.simDt;
    const int   steps   = cfg.simSteps;

    // Half-sizes da abertura do gol
    const float halfW = goal.size.x * 0.5f + margin;   // lateral
    const float halfH = goal.size.y * 0.5f + margin;   // vertical

    Vector3 simPos = ball.position;
    Vector3 simVel = ball.velocity;

    // localZ da posição inicial no espaço local do gol
    Vector3 localPrev = PointToObjectSpace(
        goal.position, goal.rightVec, goal.upVec, goal.lookVec, simPos);
    float prevLocalZ = localPrev.z;

    for (int i = 0; i < steps; ++i)
    {
        simPos.x += simVel.x * dt;
        simPos.y += simVel.y * dt;
        simPos.z += simVel.z * dt;
        simVel.y -= gravity * dt;

        Vector3 localPos = PointToObjectSpace(
            goal.position, goal.rightVec, goal.upVec, goal.lookVec, simPos);

        float currLocalZ = localPos.z;

        // Detecta cruzamento do plano frontal (localZ de + para -)
        if (prevLocalZ >= 0.f && currLocalZ < 0.f)
        {
            // Interpola a fração do passo onde localZ == 0
            float t = prevLocalZ / (prevLocalZ - currLocalZ);  // [0,1]

            // Posição interpolada no espaço local no momento do cruzamento
            Vector3 localPrevFull = PointToObjectSpace(
                goal.position, goal.rightVec, goal.upVec, goal.lookVec,
                { simPos.x - simVel.x * dt,
                  simPos.y - simVel.y * dt,
                  simPos.z - simVel.z * dt });

            float crossX = localPrevFull.x + (localPos.x - localPrevFull.x) * t;
            float crossY = localPrevFull.y + (localPos.y - localPrevFull.y) * t;

            if (std::fabsf(crossX) <= halfW && std::fabsf(crossY) <= halfH)
                return true;

            // Já cruzou o plano mas fora da abertura — não vai entrar
            return false;
        }

        prevLocalZ = currLocalZ;
    }

    return false;
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
