#include "autodive.h"
#include <cmath>

// --------------------------------------------------------------------------
// PointToObjectSpace — equivalente a CFrame:PointToObjectSpace() do Roblox
//
// Converte worldPos para coordenadas locais do CFrame definido por:
//   origin (posição), right, up, look (vetores da rotação)
//
// Fórmula: localPos = R^T * (worldPos - origin)
//   onde R é a matriz de rotação com colunas [right, up, look]
// --------------------------------------------------------------------------
Vector3 AutoDive::PointToObjectSpace(const Vector3& origin,
                                     const Vector3& right,
                                     const Vector3& up,
                                     const Vector3& look,
                                     const Vector3& worldPos)
{
    Vector3 delta = worldPos - origin;
    return {
        delta.Dot(right),
        delta.Dot(up),
        delta.Dot(look)
    };
}

// --------------------------------------------------------------------------
// IsBallTargetingGoal — simulação física com gravidade
//
// Portado diretamente da função isBallTargetingGoal() do script Lua:
//
//   1. Se bola se afastando do gol (dot <= 0) → retorna false
//   2. Simula trajetória com gravidade (dt=0.035, até 45 steps = ~1.57s)
//   3. Em cada step converte simPos para espaço local do gol
//   4. Verifica se está dentro do volume da trave + margem
//
// Equivalência Lua:
//   gravity = Vector3.new(0, -workspace.Gravity * 0.8, 0)  → ~196 studs/s²
//   simPos += simVel * dt; simVel += gravity * dt
//   localPos = goalCF:PointToObjectSpace(simPos)
//   if |localPos.X| <= goalSize.X/2 + margin
//   and |localPos.Y| <= goalSize.Y/2 + margin
//   and |localPos.Z| <= goalSize.Z/2 + 3   → true
// --------------------------------------------------------------------------
bool AutoDive::IsBallTargetingGoal(const BallState& ball, const GoalState& goal) const
{
    if (!cfg.onlyInGoal) return true;
    if (!ball.exists || !goal.exists) return true; // sem dados → não bloqueia

    // 1. Se a bola está se afastando do gol → descarta imediatamente
    Vector3 toGoal = goal.position - ball.position;
    float   dot    = ball.velocity.Dot(toGoal);
    if (dot <= 0.f) return false;

    // 2. Simulação de trajetória com gravidade
    const float gravity = cfg.gravity; // ~196 studs/s² (Roblox Gravity ~245 * 0.8)
    const float margin  = cfg.goalMargin;
    const float dt      = cfg.simDt;
    const int   steps   = cfg.simSteps;

    Vector3 simPos = ball.position;
    Vector3 simVel = ball.velocity;

    for (int i = 0; i < steps; ++i)
    {
        simPos.x += simVel.x * dt;
        simPos.y += simVel.y * dt;
        simPos.z += simVel.z * dt;

        simVel.y -= gravity * dt; // gravidade só em Y

        // Converte simPos para espaço local do gol (PointToObjectSpace)
        Vector3 localPos = PointToObjectSpace(
            goal.position,
            goal.rightVec,
            goal.upVec,
            goal.lookVec,
            simPos
        );

        // Verifica se cruza o volume da trave + margem
        bool inX = std::fabsf(localPos.x) <= (goal.size.x * 0.5f + margin);
        bool inY = std::fabsf(localPos.y) <= (goal.size.y * 0.5f + margin);
        bool inZ = std::fabsf(localPos.z) <= (goal.size.z * 0.5f + 3.f);

        if (inX && inY && inZ) return true;
    }

    return false;
}

// --------------------------------------------------------------------------
// PressKey — envia keydown + keyup via SendInput com scan code
//
// Jogos como Roblox leem input via GetAsyncKeyState, que é populado por
// SendInput. Usamos KEYEVENTF_SCANCODE para maior compatibilidade — VK
// puro pode ser ignorado por alguns hooks de input.
//
// Scan codes (Set 1):
//   Q = 0x10,  E = 0x12,  F = 0x21,  Space = 0x39
// --------------------------------------------------------------------------
static WORD VkToScanCode(WORD vk)
{
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    return static_cast<WORD>(sc);
}

void AutoDive::PressKey(WORD vk)
{
    WORD sc = VkToScanCode(vk);

    INPUT inputs[2] = {};

    // Key Down
    inputs[0].type           = INPUT_KEYBOARD;
    inputs[0].ki.wVk         = vk;
    inputs[0].ki.wScan       = sc;
    inputs[0].ki.dwFlags     = KEYEVENTF_SCANCODE;
    inputs[0].ki.time        = 0;
    inputs[0].ki.dwExtraInfo = 0;

    // Key Up
    inputs[1].type           = INPUT_KEYBOARD;
    inputs[1].ki.wVk         = vk;
    inputs[1].ki.wScan       = sc;
    inputs[1].ki.dwFlags     = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    inputs[1].ki.time        = 0;
    inputs[1].ki.dwExtraInfo = 0;

    SendInput(2, inputs, sizeof(INPUT));
}

// --------------------------------------------------------------------------
// Update — loop principal do AutoDive, chamado a cada frame
//
// Ordem de checagem idêntica ao Lua attemptAutoDive():
//   1. enabled, isGK, !isDiving (cooldown), !penaltyBool (não temos → skip)
//   2. ball exists, !ball.isWelded
//   3. dist <= triggerDistance
//   4. ballSpeed >= minBallSpeed
//   5. isBallTargetingGoal (simulação física)
//   6. relPos = root.CFrame:PointToObjectSpace(ballPos)
//   7. Decisão: HighJump → Right → Left → Front
// --------------------------------------------------------------------------
void AutoDive::Update(const GKState& gk, const BallState& ball, const GoalState& goal)
{
    m_firedThisFrame = false;
    debug = {};

    // --- Pré-condições ---
    if (!cfg.enabled)  { debug.blockReason = "disabled";   return; }
    if (!gk.isGK)      { debug.blockReason = "not GK";     return; }
    if (!ball.exists)  { debug.blockReason = "no ball";     return; }
    if (ball.isWelded) { debug.blockReason = "ball welded"; return; }

    // --- Cooldown ---
    auto  now       = std::chrono::steady_clock::now();
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

    // --- Velocidade mínima da bola ---
    float ballSpeed = ball.velocity.Length();
    if (ballSpeed < cfg.minBallSpeed)
    {
        debug.blockReason = "ball too slow";
        return;
    }

    // --- Trajetória no gol (simulação física) ---
    bool targeting = IsBallTargetingGoal(ball, goal);
    debug.approaching = targeting;

    // Preenche debug info
    debug.ballPosX  = ball.position.x;
    debug.ballPosZ  = ball.position.z;
    debug.ballVelX  = ball.velocity.x;
    debug.ballVelZ  = ball.velocity.z;
    debug.goalPosX  = goal.position.x;
    debug.goalPosZ  = goal.position.z;
    debug.goalSizeX = goal.size.x;
    debug.goalSizeZ = goal.size.z;

    if (!targeting)
    {
        debug.blockReason = "not targeting goal";
        return;
    }

    // --- relPos = root.CFrame:PointToObjectSpace(ballPos) ---
    // Usa os vetores Right/Up/Look do HRP do GK
    Vector3 relPos = PointToObjectSpace(
        gk.position,
        gk.rightVec,
        gk.upVec,
        gk.lookVec,
        ball.position
    );

    debug.relPosX = relPos.x;
    debug.relPosY = relPos.y;
    debug.relPosZ = relPos.z;

    // -----------------------------------------------------------------------
    // Lógica de decisão — espelho exato do Lua:
    //
    //   if HighJump and relPos.Y >= 5.5 and |relPos.X| <= 6  → Jump  (Space)
    //   if relPos.X > 3                                        → Right (E)
    //   if relPos.X < -3                                       → Left  (Q)
    //   if |relPos.X| <= 3 and relPos.Z < 0                   → Front (F)
    // -----------------------------------------------------------------------

    // High Jump — bola alta no centro
    if (cfg.highJump && relPos.y >= 5.5f && std::fabsf(relPos.x) <= 6.f)
    {
        PressKey(VK_SPACE);
        m_lastKey        = "Space (Jump)";
        m_firedThisFrame = true;
        m_lastDiveTime   = now;
        debug.blockReason = "FIRED - Jump";
        return;
    }

    // Right Dive — bola à direita do GK
    if (relPos.x > 3.f)
    {
        PressKey('E');
        m_lastKey        = "E (Right)";
        m_firedThisFrame = true;
        m_lastDiveTime   = now;
        debug.blockReason = "FIRED - Right";
        return;
    }

    // Left Dive — bola à esquerda do GK
    if (relPos.x < -3.f)
    {
        PressKey('Q');
        m_lastKey        = "Q (Left)";
        m_firedThisFrame = true;
        m_lastDiveTime   = now;
        debug.blockReason = "FIRED - Left";
        return;
    }

    // Front Dive — bola no centro, vindo pela frente (Z < 0 = na frente do GK)
    if (std::fabsf(relPos.x) <= 3.f && relPos.z < 0.f)
    {
        PressKey('F');
        m_lastKey        = "F (Front)";
        m_firedThisFrame = true;
        m_lastDiveTime   = now;
        debug.blockReason = "FIRED - Front";
        return;
    }

    // Bola no centro mas atrás ou parada
    debug.blockReason = "ball not in dive zone";
}
