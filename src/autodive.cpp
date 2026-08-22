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
// SimulateBallPath — integração de Euler com gravidade, drag e Magnus force
//
// Modelo físico:
//   F_gravity = (0, -g, 0)
//   F_drag    = -dragCoeff * vel        (resistência do ar, linear)
//   F_magnus  = magnusCoeff * (omega × vel)   (força de Magnus — spin curva a bola)
//
// Cross-product (omega × vel):
//   omega = angularVelocity da bola (rad/s no Roblox)
//   Resultado: força perpendicular ao plano spin-velocidade
//
// Detecção de cruzamento:
//   O plano frontal do gol fica onde localZ == 0 no espaço local do gol.
//   A cada passo verificamos se localZ mudou de sinal → interpolamos para
//   encontrar o ponto exato de cruzamento.
// --------------------------------------------------------------------------
AutoDive::SimResult AutoDive::SimulateBallPath(const BallState& ball,
                                                const GoalState& goal) const
{
    SimResult res{};
    if (!ball.exists || !goal.exists) return res;

    // Estado inicial
    Vector3 pos = ball.position;
    Vector3 vel = ball.velocity;
    Vector3 omega = ball.angularVelocity; // spin — constante durante o voo (simplificação)

    const float dt   = cfg.simDt;
    const float g    = cfg.gravity;
    const float drag = cfg.dragCoeff;
    const float mag  = cfg.magnusCoeff;

    // Posição inicial no espaço local do gol
    auto toLocal = [&](const Vector3& wp) -> Vector3 {
        return PointToObjectSpace(goal.position, goal.rightVec, goal.upVec, goal.lookVec, wp);
    };

    float prevLocalZ = toLocal(pos).z;
    float timeAcc    = 0.f;

    for (int i = 0; i < cfg.simSteps; ++i)
    {
        // ── Magnus force: F_m = magnusCoeff * (omega × vel) ──────────────
        // omega × vel em coordenadas mundo:
        //   x = omega.y*vel.z - omega.z*vel.y
        //   y = omega.z*vel.x - omega.x*vel.z
        //   z = omega.x*vel.y - omega.y*vel.x
        Vector3 magnus = {
            omega.y * vel.z - omega.z * vel.y,
            omega.z * vel.x - omega.x * vel.z,
            omega.x * vel.y - omega.y * vel.x
        };

        // ── Aceleração total ──────────────────────────────────────────────
        // drag remove uma fração constante da velocidade por segundo
        Vector3 acc = {
             mag * magnus.x - drag * vel.x,
            -g   + mag * magnus.y - drag * vel.y,
             mag * magnus.z - drag * vel.z
        };

        // ── Integração de Euler ───────────────────────────────────────────
        vel.x += acc.x * dt;
        vel.y += acc.y * dt;
        vel.z += acc.z * dt;

        pos.x += vel.x * dt;
        pos.y += vel.y * dt;
        pos.z += vel.z * dt;
        timeAcc += dt;

        // ── Teste de cruzamento do plano do gol ───────────────────────────
        Vector3 localPos = toLocal(pos);
        float   localZ   = localPos.z;

        // Mudança de sinal em localZ → cruzou o plano
        if (prevLocalZ * localZ <= 0.f && i > 0)
        {
            // Interpolação linear para encontrar o ponto exato
            float alpha = (std::fabsf(prevLocalZ) < 0.001f) ? 0.f
                          : prevLocalZ / (prevLocalZ - localZ);

            res.hit       = true;
            res.crossX    = localPos.x - alpha * (localPos.x);   // interpola
            // Refaz a interpolação corretamente com passo anterior
            // pos_prev = pos - vel*dt (aproximado, já que vel mudou, é suficiente para nosso caso)
            Vector3 prevPos = { pos.x - vel.x * dt, pos.y - vel.y * dt, pos.z - vel.z * dt };
            Vector3 prevLocal = toLocal(prevPos);
            res.crossX    = prevLocal.x + alpha * (localPos.x - prevLocal.x);
            res.crossY    = prevLocal.y + alpha * (localPos.y - prevLocal.y);
            res.timeToGoal = timeAcc - dt + alpha * dt;
            res.velAtCross = vel;
            return res;
        }

        prevLocalZ = localZ;
    }

    // Não cruzou o plano dentro da janela de simulação
    return res;
}

// --------------------------------------------------------------------------
// IsBallTargetingGoal — usa SimulateBallPath para verificar trajetória com curva
//
// Substitui a projeção linear anterior. Quando onlyInGoal está desativado
// sempre retorna true (compatibilidade com configurações antigas).
// outSim (opcional) recebe os detalhes da simulação para uso no Evaluate.
// --------------------------------------------------------------------------
bool AutoDive::IsBallTargetingGoal(const BallState& ball, const GoalState& goal,
                                   SimResult* outSim) const
{
    if (!cfg.onlyInGoal) return true;
    if (!ball.exists || !goal.exists) return true;

    SimResult sim = SimulateBallPath(ball, goal);
    if (outSim) *outSim = sim;

    if (!sim.hit) return false;   // bola não alcança o plano do gol

    float halfW = goal.size.x * 0.5f + cfg.goalMargin;
    float halfH = goal.size.y * 0.5f + cfg.goalMargin;

    return std::fabsf(sim.crossX) <= halfW && std::fabsf(sim.crossY) <= halfH;
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
// Novo fluxo com watchRange:
//
//   dist > watchRange      → ignora (muito longe)
//   dist <= watchRange     → "watch mode": roda simulação toda iteração,
//                            atualiza m_trajectoryOK
//   dist <= diveFireDistance
//     && m_trajectoryOK    → dispara o dive
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

    // Spin para debug
    debug.spinX = ball.angularVelocity.x;
    debug.spinY = ball.angularVelocity.y;
    debug.spinZ = ball.angularVelocity.z;

    float dist = (ball.position - gk.position).Length();
    debug.distToBall = dist;

    // ── Fora do watchRange: não faz nada ─────────────────────────────────
    if (dist > cfg.watchRange)
    {
        m_watchActive  = false;
        m_trajectoryOK = false;
        debug.blockReason  = "out of watch range";
        debug.inWatchRange = false;
        return;
    }

    debug.inWatchRange = true;
    m_watchActive = true;

    // ── Filtros básicos ───────────────────────────────────────────────────
    float ballSpeed = ball.velocity.Length();
    if (ballSpeed < cfg.minBallSpeed)
    {
        m_trajectoryOK = false;
        debug.blockReason = "ball too slow";
        return;
    }

    // ── Simulação de trajetória (roda toda iteração dentro do watchRange) ─
    SimResult sim;
    bool targeting = IsBallTargetingGoal(ball, goal, &sim);

    // Atualiza campos de debug da simulação
    debug.simValid   = sim.hit;
    debug.predGoalX  = sim.crossX;
    debug.predGoalY  = sim.crossY;
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

    // Atualiza o estado de trajetória confirmada
    m_trajectoryOK   = targeting;
    debug.trajectoryOK = m_trajectoryOK;

    if (!targeting)
    {
        debug.blockReason = "trajectory not targeting goal";
        return;
    }

    // ── Dentro do watchRange mas ainda longe demais para disparar ─────────
    if (dist > cfg.diveFireDistance)
    {
        debug.blockReason = "watching... waiting for fire distance";
        return;
    }

    // ── A partir daqui: dist <= diveFireDistance E trajetória confirmada ──

    auto  now       = std::chrono::steady_clock::now();
    float sinceLast = std::chrono::duration<float>(now - m_lastDiveTime).count();
    if (sinceLast < cfg.cooldownSec) { debug.blockReason = "cooldown"; return; }

    // Se a bola já vai colidir com a hitbox do GK, não faz nada
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

        float goalLocalY = 0.f;
        if (goal.exists)
        {
            Vector3 ballInGoal = PointToObjectSpace(
                goal.position, goal.rightVec, goal.upVec, goal.lookVec, ball.position);
            goalLocalY = ballInGoal.y;
        }

        bool ballHigh = (goalLocalY > 0.f);
        debug.blockReason = ballHigh ? "[zona alta]" : "[zona baixa]";

        if (ballHigh)
        {
            // ── Metade SUPERIOR → Jump+Dive ───────────────────────────────
            if (cfg.highJump && absX <= cfg.jumpPureXMax7v7)
            {
                PressKey(VK_SPACE);
                m_lastKey = "Space (Jump 7v7)"; m_firedThisFrame = true; m_lastDiveTime = now;
                debug.blockReason = "FIRED - Jump 7v7";
                return;
            }

            if (absX >= cfg.jumpDiveXMin7v7)
            {
                WORD        diveKey = (relPos.x > 0.f) ? 'E' : 'Q';
                const char* keyName = (relPos.x > 0.f) ? "Space+E (Jump+Right)" : "Space+Q (Jump+Left)";
                int         delayMs = cfg.jumpDiveDelayMs;

                PressKey(VK_SPACE);

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
        }
        else
        {
            // ── Metade INFERIOR → dive normal ────────────────────────────
            if (relPos.x > cfg.diveXThreshold7v7)
            {
                PressKey('E');
                m_lastKey = "E (Right 7v7)"; m_firedThisFrame = true; m_lastDiveTime = now;
                debug.blockReason = "FIRED - Right 7v7";
                return;
            }
            if (relPos.x < -cfg.diveXThreshold7v7)
            {
                PressKey('Q');
                m_lastKey = "Q (Left 7v7)"; m_firedThisFrame = true; m_lastDiveTime = now;
                debug.blockReason = "FIRED - Left 7v7";
                return;
            }

            debug.blockReason = "ball not in dive zone (baixo)";
        }
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
