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

            // ── Aceleração diferencial — captura a curva REAL do jogo ────
            // Idêntico ao script Lua: aceleracao = (velAtual - velAnterior) / dt
            // Só calcula se a bola está em voo livre (existe, não está presa e
            // velocidade acima do mínimo — mesma guarda do Lua: velAtual.Magnitude >= 15).
            auto now = std::chrono::steady_clock::now();
            if (ball.exists && !ball.isWelded && ball.velocity.Length() >= cfg.minBallSpeed)
            {
                if (m_prevBallValid)
                {
                    float dt = std::chrono::duration<float>(now - m_prevBallTime).count();
                    if (dt > 0.001f && dt < 0.5f)   // ignora deltas inválidos
                    {
                        ball.measuredAccel = (ball.velocity - m_prevBallVel) * (1.f / dt);
                    }
                    else
                    {
                        ball.measuredAccel = {};
                    }
                }
                else
                {
                    ball.measuredAccel = {};
                }
                m_prevBallVel   = ball.velocity;
                m_prevBallTime  = now;
                m_prevBallValid = true;
            }
            else
            {
                // Bola parada ou presa → reseta histórico para não contaminar
                // o próximo chute com aceleração do chute anterior
                m_prevBallValid    = false;
                ball.measuredAccel = {};
            }

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
// SimulateBallPath — integração de Euler com gravidade, drag e curva medida
//
// Modelo físico:
//   F_gravity  = (0, -g, 0)                         sempre aplicada
//   F_drag     = -dragCoeff * vel                   resistência linear do ar
//   F_curve    = measuredAccel - (0, -g, 0)         aceleração lateral REAL
//                                                    (medida da derivada de vel)
//
// Se measuredAccel estiver disponível (módulo > 0.5), usa ele para capturar
// qualquer curva real do jogo (Magnus scriptado, forças internas, etc.).
// Subtrai a componente de gravidade para não duplar o -g.
//
// Se measuredAccel não estiver disponível (primeiro frame, bola parada),
// cai back para Magnus teórico com angularVelocity como anteriormente.
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
    Vector3 pos   = ball.position;
    Vector3 vel   = ball.velocity;
    Vector3 omega = ball.angularVelocity;

    const float dt   = cfg.simDt;
    const float g    = cfg.gravity;
    const float drag = cfg.dragCoeff;
    const float mag  = cfg.magnusCoeff;

    // ── Aceleração de curva lateral ───────────────────────────────────────
    // Se temos aceleração medida com magnitude razoável, extraímos a
    // componente de curva pura removendo a gravidade e o drag aproximado.
    // Isso é o equivalente C++ do: aceleracao = (velAtual - velAnterior) / dt
    // do script Lua, propagado como constante durante o voo simulado.
    Vector3 curveAccel = {};
    const float measuredMag = ball.measuredAccel.Length();
    if (measuredMag > 0.5f)
    {
        // measuredAccel = gravity + drag_instantaneo + curva
        // Removemos só a componente Y de gravidade para isolar a curva lateral.
        // O drag é pequeno (0.006 * vel) e já está implícito na medição —
        // não somamos drag separado sobre a parte de curva para evitar dupla contagem.
        curveAccel = {
            ball.measuredAccel.x,
            ball.measuredAccel.y + g,   // remove o -g para isolar curva no Y
            ball.measuredAccel.z
        };
    }

    // Posição inicial no espaço local do gol
    auto toLocal = [&](const Vector3& wp) -> Vector3 {
        return PointToObjectSpace(goal.position, goal.rightVec, goal.upVec, goal.lookVec, wp);
    };

    float prevLocalZ = toLocal(pos).z;
    float timeAcc    = 0.f;

    for (int i = 0; i < cfg.simSteps; ++i)
    {
        Vector3 acc;

        if (measuredMag > 0.5f)
        {
            // ── Modo curva medida ─────────────────────────────────────────
            // gravity + drag (linear) + curva lateral medida (constante)
            acc = {
                curveAccel.x - drag * vel.x,
                -g + curveAccel.y - drag * vel.y,
                curveAccel.z - drag * vel.z
            };
        }
        else
        {
            // ── Fallback: Magnus teórico ──────────────────────────────────
            // Usado apenas quando não há medição (primeiro frame do chute)
            Vector3 magnus = {
                omega.y * vel.z - omega.z * vel.y,
                omega.z * vel.x - omega.x * vel.z,
                omega.x * vel.y - omega.y * vel.x
            };
            acc = {
                 mag * magnus.x - drag * vel.x,
                -g   + mag * magnus.y - drag * vel.y,
                 mag * magnus.z - drag * vel.z
            };
        }

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

        if (prevLocalZ * localZ <= 0.f && i > 0)
        {
            float alpha = (std::fabsf(prevLocalZ) < 0.001f) ? 0.f
                          : prevLocalZ / (prevLocalZ - localZ);

            Vector3 prevPos   = { pos.x - vel.x * dt, pos.y - vel.y * dt, pos.z - vel.z * dt };
            Vector3 prevLocal = toLocal(prevPos);

            res.hit        = true;
            res.crossX     = prevLocal.x + alpha * (localPos.x - prevLocal.x);
            res.crossY     = prevLocal.y + alpha * (localPos.y - prevLocal.y);
            res.timeToGoal = timeAcc - dt + alpha * dt;
            res.velAtCross = vel;
            return res;
        }

        prevLocalZ = localZ;
    }

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

    debug.spinX = ball.angularVelocity.x;
    debug.spinY = ball.angularVelocity.y;
    debug.spinZ = ball.angularVelocity.z;
    debug.measuredAccelX = ball.measuredAccel.x;
    debug.measuredAccelY = ball.measuredAccel.y;
    debug.measuredAccelZ = ball.measuredAccel.z;

    auto  now       = std::chrono::steady_clock::now();
    float sinceLast = std::chrono::duration<float>(now - m_lastDiveTime).count();
    if (sinceLast < cfg.cooldownSec) { debug.blockReason = "cooldown"; return; }

    float dist = (ball.position - gk.position).Length();
    debug.distToBall = dist;
    debug.inWatchRange = (dist <= cfg.watchRange);

    float ballSpeed = ball.velocity.Length();
    if (ballSpeed < cfg.minBallSpeed) { debug.blockReason = "ball too slow"; return; }

    // Simulação — usada tanto para verificar trajetória quanto para timeToGoal
    SimResult sim;
    bool targeting = IsBallTargetingGoal(ball, goal, &sim);

    debug.simValid    = sim.hit;
    debug.predGoalX   = sim.crossX;
    debug.predGoalY   = sim.crossY;
    debug.approaching = targeting;
    debug.trajectoryOK = targeting;
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

    // Trigger: dist <= diveFireDistance  OU  sim.timeToGoal <= jumpDiveTimeWindow
    // O segundo permite disparar antecipado em chutes rápidos em ângulo.
    const bool is7v7      = (cfg.gameMode == GameMode::Mode7v7);
    const bool timedTrigger = is7v7 && cfg.jumpDiveTimeWindow > 0.f &&
                              sim.hit && sim.timeToGoal <= cfg.jumpDiveTimeWindow;
    const bool distTrigger  = (dist <= cfg.diveFireDistance);

    if (!timedTrigger && !distTrigger)
    {
        debug.blockReason = "too far (dist=" + std::to_string((int)dist) + ")";
        return;
    }

    if (IsBallHittingGK(ball, gk)) { debug.blockReason = "ball hitting GK hitbox"; return; }

    Vector3 relPos = PointToObjectSpace(
        gk.position, gk.rightVec, gk.upVec, gk.lookVec, ball.position);
    debug.relPosX = relPos.x;
    debug.relPosY = relPos.y;
    debug.relPosZ = relPos.z;

    if (is7v7)
    {
        float absX = std::fabsf(relPos.x);

        // Ponto de cruzamento previsto no espaço do gol — usado como referência
        // de direção quando a simulação com curva está disponível.
        //
        // ATENÇÃO: o rightVec do gol aponta para a direita do gol visto de FORA
        // (mesma direção que um atacante vê). O GK está dentro olhando para fora,
        // então seu rightVec é oposto. crossX > 0 = bola vai para a direita do
        // atacante = esquerda do GK → deve dar Q.
        // Invertemos o sinal para alinhar com o espaço do GK.
        //
        // Fallback para relPos.x (espaço do GK) se simulação não encontrou cruzamento.
        float decisionX = sim.hit ? -sim.crossX : relPos.x;
        float absDecisionX = std::fabsf(decisionX);

        // Usa sim.crossY (onde a bola VAI cruzar o plano do gol) para decidir
        // se o chute é alto. Fallback para posição atual se simulação falhou.
        float goalLocalY = 0.f;
        if (sim.hit)
        {
            goalLocalY = sim.crossY;
        }
        else if (goal.exists)
        {
            Vector3 ballInGoal = PointToObjectSpace(
                goal.position, goal.rightVec, goal.upVec, goal.lookVec, ball.position);
            goalLocalY = ballInGoal.y;
        }

        // jumpMinCrossY: altura mínima de cruzamento para considerar "alto"
        // Evita Jump+Dive em chutes que sobem levemente mas entram embaixo do gol.
        bool ballHigh = (goalLocalY >= cfg.jumpMinCrossY);
        debug.blockReason = ballHigh ? "[zona alta]" : "[zona baixa]";

        if (ballHigh)
        {
            // ── Metade SUPERIOR do gol → Jump+Dive (Space+Q/E) ───────────
            // Usa decisionX (crossX com curva) para decidir a direção
            if (cfg.highJump && absDecisionX <= cfg.jumpPureXMax7v7)
            {
                PressKey(VK_SPACE);
                m_lastKey = "Space (Jump 7v7)"; m_firedThisFrame = true; m_lastDiveTime = now;
                debug.blockReason = "FIRED - Jump 7v7";
                return;
            }
            if (absDecisionX >= cfg.jumpDiveXMin7v7)
            {
                WORD        diveKey = (decisionX > 0.f) ? 'E' : 'Q';
                const char* keyName = (decisionX > 0.f) ? "Space+E (Jump+Right)" : "Space+Q (Jump+Left)";
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
            // Zona morta: fallback Jump puro
            if (cfg.highJump)
            {
                PressKey(VK_SPACE);
                m_lastKey = "Space (Jump 7v7)"; m_firedThisFrame = true; m_lastDiveTime = now;
                debug.blockReason = "FIRED - Jump 7v7 fallback";
                return;
            }
        }
        else
        {
            // ── Metade INFERIOR do gol → dive normal (Q/E) ───────────────
            // Usa decisionX (crossX com curva) para saber lado real de chegada
            if (decisionX > cfg.diveXThreshold7v7)
            {
                PressKey('E');
                m_lastKey = "E (Right 7v7)"; m_firedThisFrame = true; m_lastDiveTime = now;
                debug.blockReason = "FIRED - Right 7v7";
                return;
            }
            if (decisionX < -cfg.diveXThreshold7v7)
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
        // ── 4v4 ──────────────────────────────────────────────────────────
        if (cfg.highJump && relPos.y >= cfg.jumpYThreshold && std::fabsf(relPos.x) <= cfg.jumpXMaxForPure)
        {
            PressKey(VK_SPACE);
            m_lastKey = "Space (Jump)"; m_firedThisFrame = true; m_lastDiveTime = now;
            debug.blockReason = "FIRED - Jump";
            return;
        }
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
