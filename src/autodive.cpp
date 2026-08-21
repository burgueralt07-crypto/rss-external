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

// --------------------------------------------------------------------------
// IsBallHittingGK — verifica se a trajetória linear da bola vai interceptar
// a hitbox AABB do GK (part "Hitbox" do model).
//
// Projeta a posição da bola no tempo t = d/v até cada face do AABB.
// Se o ponto de cruzamento estiver dentro da caixa, a bola vai bater no GK.
// Retorna true → não precisa de ação (o corpo já vai defender).
// --------------------------------------------------------------------------
bool AutoDive::IsBallHittingGK(const BallState& ball, const GKState& gk) const
{
    // Sem hitbox lida → não bloqueia
    if (gk.hitboxSize.x <= 0.f && gk.hitboxSize.y <= 0.f && gk.hitboxSize.z <= 0.f)
        return false;

    // AABB min/max com margem pequena de 0.3 studs para compensar latência
    constexpr float margin = 0.3f;
    Vector3 hmin = {
        gk.hitboxPos.x - gk.hitboxSize.x * 0.5f - margin,
        gk.hitboxPos.y - gk.hitboxSize.y * 0.5f - margin,
        gk.hitboxPos.z - gk.hitboxSize.z * 0.5f - margin
    };
    Vector3 hmax = {
        gk.hitboxPos.x + gk.hitboxSize.x * 0.5f + margin,
        gk.hitboxPos.y + gk.hitboxSize.y * 0.5f + margin,
        gk.hitboxPos.z + gk.hitboxSize.z * 0.5f + margin
    };

    // Ray-AABB intersection (slab method)
    // Se a bola já está dentro da hitbox, também retorna true
    Vector3 p = ball.position;
    Vector3 v = ball.velocity;

    float tmin = 0.f;
    float tmax = 3.f; // só olha 3 segundos à frente

    auto checkAxis = [&](float pos, float vel, float bmin, float bmax) -> bool {
        if (std::fabsf(vel) < 0.001f) {
            // Sem movimento nesse eixo — verifica se já está dentro
            return pos >= bmin && pos <= bmax;
        }
        float t1 = (bmin - pos) / vel;
        float t2 = (bmax - pos) / vel;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
        tmin = std::fmaxf(tmin, t1);
        tmax = std::fminf(tmax, t2);
        return tmin <= tmax;
    };

    if (!checkAxis(p.x, v.x, hmin.x, hmax.x)) return false;
    if (!checkAxis(p.y, v.y, hmin.y, hmax.y)) return false;
    if (!checkAxis(p.z, v.z, hmin.z, hmax.z)) return false;

    return tmin <= tmax && tmax >= 0.f;
}

// --------------------------------------------------------------------------
// Evaluate — lógica de decisão (chamada pelo ScanLoop)
//
// 4v4:  Jump → Right (E) → Left (Q)
// 7v7:  Jump puro central → Jump+Dive lateral (Space→Q/E) → Right (E) → Left (Q)
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

    if (goal.exists && ball.exists)
    {
        Vector3 lp = PointToObjectSpace(goal.position, goal.rightVec, goal.upVec, goal.lookVec, ball.position);
        debug.ballLocalZ = lp.z;
    }

    if (!targeting) { debug.blockReason = "not targeting goal"; return; }

    // Se a bola já vai colidir com a hitbox do GK, não faz nada —
    // o corpo do goleiro já vai defender sem precisar de dive
    if (IsBallHittingGK(ball, gk))
    {
        debug.blockReason = "ball hitting GK hitbox";
        return;
    }

    Vector3 relPos = PointToObjectSpace(
        gk.position, gk.rightVec, gk.upVec, gk.lookVec, ball.position);
    debug.relPosX = relPos.x;
    debug.relPosY = relPos.y;
    debug.relPosZ = relPos.z;

    const bool is7v7 = (cfg.gameMode == GameMode::Mode7v7);

    if (is7v7)
    {
        float absX = std::fabsf(relPos.x);

        // ── 7v7: Jump puro central ────────────────────────────────────────
        // Bola muito alta E no centro → só Space
        if (cfg.highJump && relPos.y >= cfg.jumpPureYThreshold && absX < cfg.jumpDiveXMin7v7)
        {
            PressKey(VK_SPACE);
            m_lastKey = "Space (Jump 7v7)"; m_firedThisFrame = true; m_lastDiveTime = now;
            debug.blockReason = "FIRED - Jump 7v7";
            return;
        }

        // ── 7v7: Jump+Dive lateral ────────────────────────────────────────
        // Bola alta (Y ≥ jumpYThreshold7v7) E lateral (jumpDiveXMin ≤ |X| ≤ jumpDiveXMax)
        // → Space primeiro, depois Q (esquerda) ou E (direita) com delay
        if (relPos.y >= cfg.jumpYThreshold7v7 && absX >= cfg.jumpDiveXMin7v7 && absX <= cfg.jumpDiveXMax7v7)
        {
            WORD  diveKey  = (relPos.x > 0.f) ? 'E' : 'Q';
            const char* keyName = (relPos.x > 0.f) ? "Space+E (Jump+Right)" : "Space+Q (Jump+Left)";
            int delayMs = cfg.jumpDiveDelayMs;

            // Dispara Space imediatamente
            PressKey(VK_SPACE);

            // Dispara Q/E após delay em thread separada (não trava o scan loop)
            std::thread([diveKey, delayMs]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                WORD sc = static_cast<WORD>(MapVirtualKeyW(diveKey, MAPVK_VK_TO_VSC));
                INPUT down = {};
                down.type = INPUT_KEYBOARD; down.ki.wScan = sc;
                down.ki.dwFlags = KEYEVENTF_SCANCODE;
                SendInput(1, &down, sizeof(INPUT));
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                INPUT up = down;
                up.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
                SendInput(1, &up, sizeof(INPUT));
            }).detach();

            m_lastKey = keyName; m_firedThisFrame = true; m_lastDiveTime = now;
            debug.blockReason = std::string("FIRED - ") + keyName;
            return;
        }

        // ── 7v7: Dives laterais normais (bola baixa E fora da faixa de Jump+Dive) ──
        // Se a bola está na faixa X do Jump+Dive mas Y ainda não chegou, aguarda
        // (não dispara dive normal pra não desperdiçar o cooldown antes do pulo)
        bool inJumpDiveXRange = (absX >= cfg.jumpDiveXMin7v7 && absX <= cfg.jumpDiveXMax7v7);

        if (!inJumpDiveXRange && relPos.x > cfg.diveXThreshold7v7)
        {
            PressKey('E');
            m_lastKey = "E (Right 7v7)"; m_firedThisFrame = true; m_lastDiveTime = now;
            debug.blockReason = "FIRED - Right 7v7";
            return;
        }
        if (!inJumpDiveXRange && relPos.x < -cfg.diveXThreshold7v7)
        {
            PressKey('Q');
            m_lastKey = "Q (Left 7v7)"; m_firedThisFrame = true; m_lastDiveTime = now;
            debug.blockReason = "FIRED - Left 7v7";
            return;
        }

        // Bola na faixa X do Jump+Dive mas Y ainda não chegou — aguarda
        if (inJumpDiveXRange)
            debug.blockReason = "aguardando Y p/ Jump+Dive";
    }
    else
    {
        // ── 4v4: High Jump ────────────────────────────────────────────────
        if (cfg.highJump && relPos.y >= cfg.jumpYThreshold && std::fabsf(relPos.x) <= cfg.jumpXMaxForPure)
        {
            PressKey(VK_SPACE);
            m_lastKey = "Space (Jump)"; m_firedThisFrame = true; m_lastDiveTime = now;
            debug.blockReason = "FIRED - Jump";
            return;
        }

        // ── 4v4: Dives laterais ───────────────────────────────────────────
        if (relPos.x > cfg.diveXThreshold)
        {
            PressKey('E');
            m_lastKey = "E (Right)"; m_firedThisFrame = true; m_lastDiveTime = now;
            debug.blockReason = "FIRED - Right";
            return;
        }
        if (relPos.x < -cfg.diveXThreshold)
        {
            PressKey('Q');
            m_lastKey = "Q (Left)"; m_firedThisFrame = true; m_lastDiveTime = now;
            debug.blockReason = "FIRED - Left";
            return;
        }
    }

    debug.blockReason = "ball not in dive zone";
}
