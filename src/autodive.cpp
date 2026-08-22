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

            // ── Aceleração diferencial com filtro EMA ───────────────────
            // rawAccel = (velAtual - velAnterior) / dt  (igual ao Lua)
            // filteredAccel = rawAccel * alpha + filteredAccel * (1 - alpha)
            //
            // O EMA suaviza o ruído de Δv/Δt a 240 Hz sem introduzir delay
            // perceptível para o integrador RK4 posterior.
            // Na primeira amostra inicializa diretamente (sem suavização) —
            // idêntico ao bloco "if filteredAccel.Magnitude < 0.1" do Lua.
            auto now = std::chrono::steady_clock::now();
            if (ball.exists && !ball.isWelded && ball.velocity.Length() >= cfg.minBallSpeed)
            {
                if (m_prevBallValid)
                {
                    float dt = std::chrono::duration<float>(now - m_prevBallTime).count();
                    if (dt > 0.001f && dt < 0.5f)   // ignora deltas inválidos
                    {
                        Vector3 rawAccel = (ball.velocity - m_prevBallVel) * (1.f / dt);

                        if (!m_filteredAccelValid || m_filteredAccel.Length() < 0.1f)
                        {
                            // Primeira amostra — inicializa diretamente (sem suavização)
                            m_filteredAccel      = rawAccel;
                            m_filteredAccelValid = true;
                        }
                        else
                        {
                            // EMA: novo = raw * alpha + antigo * (1 - alpha)
                            const float a = cfg.emaAlpha;
                            m_filteredAccel = rawAccel * a + m_filteredAccel * (1.f - a);
                        }

                        ball.measuredAccel = m_filteredAccel;
                    }
                    else
                    {
                        ball.measuredAccel = m_filteredAccel;   // mantém último valor válido
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
                m_prevBallValid      = false;
                m_filteredAccelValid = false;
                m_filteredAccel      = {};
                ball.measuredAccel   = {};
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
// SimulateBallPath — RK4 com decaimento exponencial da curva + quique no chão
//
// Modelo físico (equivalente ao Lua):
//   curveAccel = componente de curva lateral pura extraída do measuredAccel
//                (remove Y de gravidade se |accelY| > 5, senão usa só X/Z)
//   getAcceleration(vel, t):
//     decay = exp(-curveDecayRate * t)          ← curva enfraquece com o tempo
//     acc   = -gravity_Y + curveAccel*decay - drag*vel
//
//   RK4 padrão:
//     k1 = f(vel,          t)
//     k2 = f(vel + k1*½dt, t + ½dt)
//     k3 = f(vel + k2*½dt, t + ½dt)
//     k4 = f(vel + k3*dt,  t + dt)
//     vel += (k1 + 2k2 + 2k3 + k4) * dt/6
//     pos += (v1 + 2v2 + 2v3 + v4) * dt/6   (velocidades intermediárias)
//
//   Quique no chão: se pos.Y <= 0.6 && vel.Y < 0  →  vel.Y = -vel.Y * 0.65
//
// Detecção de cruzamento do plano do gol:
//   Mesmo critério do Lua: localZ >= -1 && localZ <= 1 (tolerância de 1 stud).
//   Interpolação linear para encontrar crossX/crossY exatos — mantém as
//   mesmas saídas que a Evaluate já consome (crossX, crossY, timeToGoal,
//   worldCross, velAtCross).
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

    const float dt       = cfg.simDt;
    const float g        = cfg.gravity;
    const float drag     = cfg.dragCoeff;
    const float mag      = cfg.magnusCoeff;
    const float decay    = cfg.curveDecayRate;

    // ── Componente de curva lateral (equivalente ao Lua) ─────────────────
    // Se |accelY| > 5 → a bola tem sustentação vertical real, inclui Y.
    // Senão → usa só X e Z (curva lateral pura sem dobrar gravidade).
    const float measuredMag = ball.measuredAccel.Length();
    Vector3 curveAccel = {};

    if (measuredMag > 0.5f)
    {
        if (std::fabsf(ball.measuredAccel.y) > 5.f)
        {
            // Inclui Y — remove componente de gravidade para não duplar
            curveAccel = {
                ball.measuredAccel.x,
                ball.measuredAccel.y + g,
                ball.measuredAccel.z
            };
        }
        else
        {
            // Só curva lateral (X e Z), Y permanece zero
            curveAccel = { ball.measuredAccel.x, 0.f, ball.measuredAccel.z };
        }
    }

    // ── Função de aceleração instantânea (capturada por valor no lambda) ──
    // Recebe velocidade e tempo acumulado → retorna acc com decaimento
    auto getAcceleration = [&](const Vector3& v, float t) -> Vector3
    {
        const float d = std::expf(-decay * t);   // fator de decaimento
        // Fallback Magnus teórico se não houver medição
        Vector3 lateralAccel = curveAccel;
        if (measuredMag <= 0.5f)
        {
            lateralAccel = {
                mag * (omega.y * v.z - omega.z * v.y),
                mag * (omega.z * v.x - omega.x * v.z),
                mag * (omega.x * v.y - omega.y * v.x)
            };
        }

        return {
            lateralAccel.x * d - drag * v.x,
            -g + lateralAccel.y * d - drag * v.y,
            lateralAccel.z * d - drag * v.z
        };
    };

    // Espaço local do gol
    auto toLocal = [&](const Vector3& wp) -> Vector3 {
        return PointToObjectSpace(goal.position, goal.rightVec, goal.upVec, goal.lookVec, wp);
    };

    float timeAcc    = 0.f;
    float prevLocalZ = toLocal(pos).z;   // Z inicial no espaço do gol (antes do loop)

    for (int i = 0; i < cfg.simSteps; ++i)
    {
        // ── RK4 ──────────────────────────────────────────────────────────
        const Vector3 a1 = getAcceleration(vel,              timeAcc);
        const Vector3 v1 = vel;

        const Vector3 v2 = vel + a1 * (dt * 0.5f);
        const Vector3 a2 = getAcceleration(v2, timeAcc + dt * 0.5f);

        const Vector3 v3 = vel + a2 * (dt * 0.5f);
        const Vector3 a3 = getAcceleration(v3, timeAcc + dt * 0.5f);

        const Vector3 v4 = vel + a3 * dt;
        const Vector3 a4 = getAcceleration(v4, timeAcc + dt);

        const Vector3 prevPos = pos;   // salva ANTES de avançar — usado na interpolação

        vel = vel + (a1 + a2 * 2.f + a3 * 2.f + a4) * (dt / 6.f);
        pos = pos + (v1 + v2 * 2.f + v3 * 2.f + v4) * (dt / 6.f);
        timeAcc += dt;

        // ── Quique no chão ────────────────────────────────────────────────
        // Equivale ao bloco do Lua: if pos.Y <= 0.6 and vel.Y < 0
        constexpr float GROUND_Y    = 0.6f;
        constexpr float BOUNCE_COEF = 0.65f;
        if (pos.y <= GROUND_Y && vel.y < 0.f)
        {
            vel.y = -vel.y * BOUNCE_COEF;
            pos.y = GROUND_Y;
        }

        // ── Cruzamento do plano do gol ────────────────────────────────────
        // Detecção por mudança de sinal de localZ — imune a tunelamento.
        // Mesmo que a bola pule de +5 para -5 em um único passo, o produto
        // prevLocalZ * localZ fica negativo e o cruzamento é detectado.
        // Interpolação linear (alpha) encontra o ponto exato entre os dois
        // frames e devolve crossX/crossY/timeToGoal precisos.
        Vector3 localPos = toLocal(pos);
        float   localZ   = localPos.z;

        if (i > 0 && prevLocalZ * localZ <= 0.f)
        {
            // alpha = fração do passo em que localZ = 0
            float alpha = (std::fabsf(prevLocalZ) < 0.001f)
                          ? 0.f
                          : prevLocalZ / (prevLocalZ - localZ);

            Vector3 prevLocal = toLocal(prevPos);

            res.hit        = true;
            res.crossX     = prevLocal.x + alpha * (localPos.x - prevLocal.x);
            res.crossY     = prevLocal.y + alpha * (localPos.y - prevLocal.y);
            res.timeToGoal = timeAcc - dt + alpha * dt;
            res.velAtCross = vel;
            res.worldCross = prevPos + (pos - prevPos) * alpha;
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
        // Ponto de cruzamento previsto no espaço do GK — referência de direção correta
        // independente do time (Home ou Away).
        //
        // Antes usávamos -sim.crossX (espaço do gol), que exigia saber se o rightVec
        // do gol estava alinhado ou oposto ao rightVec do GK. Isso causava inversão
        // de lado ao jogar como Away GK, já que a part AntiOwnGoal tem rotação 180°
        // em relação ao Home.
        //
        // Solução: projetar sim.worldCross no espaço local do GK. O eixo direito do GK
        // (gk.rightVec) aponta para a direita do GK independente do time.
        // Se worldCrossInGK.x > 0 → bola vai à direita do GK → E.
        // Se worldCrossInGK.x < 0 → bola vai à esquerda do GK → Q.
        //
        // Fallback para relPos.x se simulação não encontrou cruzamento.
        float decisionX = relPos.x;   // fallback: posição atual no espaço do GK
        if (sim.hit)
        {
            Vector3 worldCrossInGK = PointToObjectSpace(
                gk.position, gk.rightVec, gk.upVec, gk.lookVec, sim.worldCross);
            decisionX = worldCrossInGK.x;
        }
        float absDecisionX = std::fabsf(decisionX);

        // Usa sim.crossY (onde a bola VAI cruzar o plano do gol) para decidir
        // se o chute é alto. Quando a simulação não encontra cruzamento (sim.hit=false),
        // usa 0.f como fallback (centro do gol) para não penalizar chutes rasteiros
        // cuja trajetória não convergiu nos passos disponíveis.
        float goalLocalY = 0.f;
        if (sim.hit)
        {
            goalLocalY = sim.crossY;
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
