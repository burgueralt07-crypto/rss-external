#include "rbx.h"
#include <algorithm>
#include <cctype>

// --------------------------------------------------------------------------
// ReadRbxString — baseado em memory_t::read_string do base de referência
//
// threshold >= 16 → heap (ponteiro em addr+0x00)
// threshold <  16 → inline (bytes em addr+0x00)
// --------------------------------------------------------------------------
std::string RobloxReader::ReadRbxString(uintptr_t addr) const
{
    if (!addr) return {};

    int32_t len = ReadT<int32_t>(addr + 0x10);
    if (len <= 0 || len > 255) return {};

    uintptr_t dataPtr = (len >= 16) ? ReadPtr(addr) : addr;
    if (!dataPtr) return {};

    std::string result(len, '\0');
    m_mem.ReadRaw(dataPtr, result.data(), len);
    return result;
}

// --------------------------------------------------------------------------
// GetInstanceName — usa NameContainer + Name conforme indicado pelo fornecedor
//
// string GetName(uintptr_t Instance):
//   namePtr = read(Instance + NameContainer)  // 0x70
//   return ReadString(namePtr + Name)          // 0x8
// --------------------------------------------------------------------------
std::string RobloxReader::GetInstanceName(uintptr_t instance) const
{
    if (!instance) return {};
    uintptr_t nameContainer = ReadPtr(instance + Offsets::Instance::NameContainer); // 0x70
    if (!nameContainer) return {};
    return ReadRbxString(nameContainer + Offsets::Instance::Name); // + 0x8
}

// --------------------------------------------------------------------------
// GetInstanceClass — Instance + ClassDescriptor + ClassName → string
// --------------------------------------------------------------------------
std::string RobloxReader::GetInstanceClass(uintptr_t instance) const
{
    if (!instance) return {};
    uintptr_t classDesc = ReadPtr(instance + Offsets::Instance::ClassDescriptor);
    if (!classDesc) return {};
    uintptr_t namePtr = ReadPtr(classDesc + Offsets::Instance::ClassName);
    if (!namePtr) return {};
    return ReadRbxString(namePtr);
}

// --------------------------------------------------------------------------
// GetChildren — baseado em GetChildList() do base de referência
//
// Estrutura:
//   instance + ChildrenStart(0x78) → ptr para struct intermediária
//   struct[0x00] = begin (ptr para primeiro elemento)
//   struct[ChildrenEnd(0x08)] = end
//   cada elemento = 16 bytes (shared_ptr): [instancePtr(8)][refcount(8)]
// --------------------------------------------------------------------------
std::vector<uintptr_t> RobloxReader::GetChildren(uintptr_t instance) const
{
    std::vector<uintptr_t> result;
    if (!instance) return result;

    uintptr_t childStart = ReadPtr(instance + Offsets::Instance::ChildrenStart);
    if (!childStart) return result;

    uintptr_t childEnd = ReadPtr(childStart + Offsets::Instance::ChildrenEnd);
    uintptr_t current  = ReadPtr(childStart);

    if (!childEnd || !current || childEnd < current) return result;

    constexpr size_t maxChildren = 4096;
    size_t count = 0;

    for (uintptr_t ptr = current; ptr < childEnd && count < maxChildren; ptr += 0x10, ++count)
    {
        uintptr_t child = ReadPtr(ptr);
        if (child) result.push_back(child);
    }

    return result;
}

// --------------------------------------------------------------------------
// FindChild — busca por nome
// --------------------------------------------------------------------------
uintptr_t RobloxReader::FindChild(uintptr_t instance, const std::string& name) const
{
    for (uintptr_t child : GetChildren(instance))
    {
        if (GetInstanceName(child) == name)
            return child;
    }
    return 0;
}

// --------------------------------------------------------------------------
// FindChildByClass — busca por classe (mais robusto que por nome)
// --------------------------------------------------------------------------
uintptr_t RobloxReader::FindChildByClass(uintptr_t instance, const std::string& cls) const
{
    for (uintptr_t child : GetChildren(instance))
    {
        if (GetInstanceClass(child) == cls)
            return child;
    }
    return 0;
}

// --------------------------------------------------------------------------
// ReadPartPosition
// --------------------------------------------------------------------------
Vector3 RobloxReader::ReadPartPosition(uintptr_t basePart) const
{
    if (!basePart) return {};
    uintptr_t primitive = ReadPtr(basePart + Offsets::BasePart::Primitive);
    if (!primitive) return {};
    return ReadT<Vector3>(primitive + Offsets::Primitive::Position);
}

// --------------------------------------------------------------------------
// ReadPartSize
// --------------------------------------------------------------------------
Vector3 RobloxReader::ReadPartSize(uintptr_t basePart) const
{
    if (!basePart) return {};
    uintptr_t primitive = ReadPtr(basePart + Offsets::BasePart::Primitive);
    if (!primitive) return {};
    return ReadT<Vector3>(primitive + Offsets::Primitive::Size);
}

// --------------------------------------------------------------------------
// Update — cadeia principal
//
// Separado em dois estágios:
//   UpdateStructure() — resolve ponteiros estáveis (base, dataModel, workspace,
//                       players service). Caro (Toolhelp32Snapshot). Roda uma
//                       vez e só repete se os ponteiros ficarem inválidos.
//   Update()          — atualiza apenas dados que mudam a cada frame
//                       (posições, velocidades, ball, gk, goal). Barato.
// --------------------------------------------------------------------------
bool RobloxReader::UpdateStructure()
{
    // Base do módulo — caro, cacheia
    m_base = m_mem.GetModuleBase(L"RobloxPlayerBeta.exe");
    if (!m_base) return false;

    // DataModel
    uintptr_t fakeDataModel = ReadPtr(m_base + Offsets::FakeDataModel::Pointer);
    if (!fakeDataModel) return false;
    m_dataModel = ReadPtr(fakeDataModel + Offsets::FakeDataModel::RealDataModel);
    if (!m_dataModel) return false;

    // Workspace
    m_workspace = FindChildByClass(m_dataModel, "Workspace");
    if (!m_workspace) return false;

    // Camera (viewport estável)
    m_camera = FindChildByClass(m_workspace, "Camera");

    // Players service
    m_playersService = FindChildByClass(m_dataModel, "Players");
    if (!m_playersService) return false;

    m_localPlayer = ReadPtr(m_playersService + Offsets::Player::LocalPlayer);

    m_structureValid = true;
    return true;
}

bool RobloxReader::Update()
{
    m_players.clear();

    // Refresh estrutura se necessário (primeira vez ou ponteiros inválidos)
    if (!m_structureValid || !m_dataModel || !m_workspace || !m_playersService)
    {
        if (!UpdateStructure()) return false;
    }

    // Detecta troca de jogo: re-lê o DataModel e compara com o cacheado.
    // Quando o jogador troca de partida, o Roblox mantém o mesmo PID mas
    // substitui o DataModel — os ponteiros antigos viram lixo.
    if (m_base)
    {
        uintptr_t fakeDataModel = ReadPtr(m_base + Offsets::FakeDataModel::Pointer);
        if (fakeDataModel)
        {
            uintptr_t currentDataModel = ReadPtr(fakeDataModel + Offsets::FakeDataModel::RealDataModel);
            if (currentDataModel && currentDataModel != m_dataModel)
            {
                // DataModel mudou → re-escaneia tudo do zero
                InvalidateStructure();
                if (!UpdateStructure()) return false;
            }
        }
    }

    // VisualEngine — lê ViewMatrix (ponteiro estático, leve)
    uintptr_t visualEngine = ReadPtr(m_base + Offsets::VisualEngine::Pointer);
    if (visualEngine)
        m_viewMatrix = ReadT<Matrix4x4>(visualEngine + Offsets::VisualEngine::ViewMatrix);

    // Viewport
    if (m_camera)
        m_viewport = ReadT<Vector2>(m_camera + Offsets::Camera::ViewportSize);

    // LocalPlayer pode mudar (reconnect)
    m_localPlayer = ReadPtr(m_playersService + Offsets::Player::LocalPlayer);

    // Itera jogadores
    for (uintptr_t playerInst : GetChildren(m_playersService))
    {
        if (playerInst == m_localPlayer) continue;

        PlayerData pd;
        pd.name = GetInstanceName(playerInst);
        if (pd.name.empty()) continue;

        // Modelo do personagem
        uintptr_t model = ReadPtr(playerInst + Offsets::Player::ModelInstance);
        if (!model) continue;

        // Humanoid
        uintptr_t humanoid = FindChildByClass(model, "Humanoid");
        if (!humanoid) continue;

        pd.health    = ReadT<float>(humanoid + Offsets::Humanoid::Health);
        pd.maxHealth = ReadT<float>(humanoid + Offsets::Humanoid::MaxHealth);
        pd.isAlive   = pd.health > 0.f;

        // HumanoidRootPart
        uintptr_t hrp = ReadPtr(humanoid + Offsets::Humanoid::HumanoidRootPart);
        if (!hrp) hrp = FindChild(model, "HumanoidRootPart");

        pd.position = ReadPartPosition(hrp);

        m_players.push_back(pd);
    }

    // Lê bola e estado do GK para AutoDive
    ReadBallState();
    ReadGKState();
    ReadGoalState();

    return true;
}

// --------------------------------------------------------------------------
// GetGoalPartsDebug — lista todas as parts de HomeGoal ou AwayGoal
// com nome, classe, posição e tamanho para identificar a part correta
// --------------------------------------------------------------------------
std::vector<RobloxReader::GoalPartInfo> RobloxReader::GetGoalPartsDebug(bool awayGoal) const
{
    std::vector<GoalPartInfo> result;
    if (!m_workspace) return result;

    uintptr_t goalModel = 0;
    if (awayGoal)
    {
        goalModel = FindChild(m_workspace, "AwayGoal");
        if (!goalModel) goalModel = FindChild(m_workspace, "AwayGoalDetector");
    }
    else
    {
        goalModel = FindChild(m_workspace, "HomeGoal");
        if (!goalModel) goalModel = FindChild(m_workspace, "HomeGoalDetector");
    }
    if (!goalModel) return result;

    for (uintptr_t child : GetChildren(goalModel))
    {
        GoalPartInfo info;
        info.name = GetInstanceName(child);
        info.cls  = GetInstanceClass(child);

        uintptr_t prim = ReadPtr(child + Offsets::BasePart::Primitive);
        if (prim)
        {
            info.position = ReadT<Vector3>(prim + Offsets::Primitive::Position);
            info.size     = ReadT<Vector3>(prim + Offsets::Primitive::Size);
        }
        result.push_back(info);
    }
    return result;
}
//
// Versões públicas para uso direto pela ScanLoop do AutoDive (240 Hz).
// Não atualizam m_ball/m_gkState/m_goalState — retornam o estado como valor.
// Reutilizam ponteiros cacheados (m_workspace, m_localPlayer, m_cachedBall)
// que são mantidos atualizados pelo render loop via Update().
// --------------------------------------------------------------------------
BallState RobloxReader::ReadBallDirect()
{
    BallState ball{};
    if (!m_workspace) return ball;

    // Valida cache da instância da bola
    if (m_cachedBall)
    {
        std::string name = GetInstanceName(m_cachedBall);
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower != "ball" && lower != "football" && lower != "soccerball" && lower != "soccer_ball")
            m_cachedBall = 0;
    }

    if (!m_cachedBall)
    {
        for (uintptr_t child : GetChildren(m_workspace))
        {
            std::string name = GetInstanceName(child);
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower == "ball" || lower == "football" || lower == "soccerball" || lower == "soccer_ball")
            {
                m_cachedBall = child;
                break;
            }
        }
    }

    if (!m_cachedBall) return ball;

    uintptr_t primitive = ReadPtr(m_cachedBall + Offsets::BasePart::Primitive);
    if (!primitive) { m_cachedBall = 0; return ball; }

    ball.exists   = true;
    ball.isWelded = (FindChild(m_cachedBall, "playerWeld") != 0);
    ball.position = ReadT<Vector3>(primitive + Offsets::Primitive::Position);
    ball.velocity = ReadT<Vector3>(primitive + Offsets::Primitive::AssemblyLinearVelocity);
    ball.angularVelocity = ReadT<Vector3>(primitive + Offsets::Primitive::AssemblyAngularVelocity);
    return ball;
}

GKState RobloxReader::ReadGKDirect()
{
    GKState gk{};

    if (!m_workspace || !m_localPlayer) return gk;

    // Verifica se o render loop já confirmou que somos GK — evita re-escanear
    // pasta Bools a cada scan de 240 Hz (caro). A confirmação de GK muda raramente.
    {
        std::lock_guard<std::mutex> lk(m_stateMtx);
        if (!m_gkState.isGK) return gk;
        gk.isGK = true;
    }

    uintptr_t model = ReadPtr(m_localPlayer + Offsets::Player::ModelInstance);
    if (!model) return gk;

    uintptr_t hrp = FindChild(model, "HumanoidRootPart");
    if (!hrp) return gk;

    gk.position = ReadPartPosition(hrp);

    uintptr_t hrpPrim = ReadPtr(hrp + Offsets::BasePart::Primitive);
    if (hrpPrim)
    {
        Matrix3x3 rot = ReadT<Matrix3x3>(hrpPrim + Offsets::Primitive::Rotation);
        gk.rightVec = rot.Right();
        gk.upVec    = rot.Up();
        gk.lookVec  = rot.Look();
    }

    // Lê hitbox física do GK (part "Hitbox" dentro do model)
    uintptr_t hitboxPart = FindChild(model, "Hitbox");
    if (hitboxPart)
    {
        gk.hitboxPos  = ReadPartPosition(hitboxPart);
        gk.hitboxSize = ReadPartSize(hitboxPart);
    }

    return gk;
}

GoalState RobloxReader::ReadGoalDirect()
{
    GoalState goal{};
    if (!m_workspace) return goal;

    bool isAPG;
    {
        std::lock_guard<std::mutex> lk(m_stateMtx);
        if (!m_gkState.isGK) return goal;
        isAPG = m_isAPG;
    }

    // Away: Workspace.AwayAntiOwnGoal (direto no workspace)
    // Home: Workspace.HomePosition.HomeAntiOwnGoal
    uintptr_t part = 0;
    if (isAPG)
    {
        part = FindChild(m_workspace, "AwayAntiOwnGoal");
    }
    else
    {
        uintptr_t homePosition = FindChild(m_workspace, "HomePosition");
        if (homePosition)
            part = FindChild(homePosition, "HomeAntiOwnGoal");
    }
    if (!part) return goal;

    uintptr_t primitive = ReadPtr(part + Offsets::BasePart::Primitive);
    if (!primitive) return goal;

    goal.exists   = true;
    goal.position = ReadT<Vector3>(primitive + Offsets::Primitive::Position);
    goal.size     = ReadT<Vector3>(primitive + Offsets::Primitive::Size);
    Matrix3x3 rot = ReadT<Matrix3x3>(primitive + Offsets::Primitive::Rotation);
    goal.rightVec = rot.Right();
    goal.upVec    = rot.Up();
    goal.lookVec  = rot.Look();
    return goal;
}

// --------------------------------------------------------------------------
// ReadBallState — lê posição e velocidade da bola no Workspace
//
// Cacheia o ponteiro da instância igual ao Lua (cachedBall).
// Só refaz o scan de GetChildren se o ponteiro cacheado ficar inválido.
// --------------------------------------------------------------------------
bool RobloxReader::ReadBallState()
{
    m_ball = {};
    if (!m_workspace) return false;

    // Valida cache: tenta ler o nome da instância cacheada
    if (m_cachedBall)
    {
        std::string name = GetInstanceName(m_cachedBall);
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower != "ball" && lower != "football" && lower != "soccerball" && lower != "soccer_ball")
            m_cachedBall = 0; // cache inválido, refaz busca
    }

    // Só escaneia filhos se não tem cache válido
    if (!m_cachedBall)
    {
        for (uintptr_t child : GetChildren(m_workspace))
        {
            std::string name = GetInstanceName(child);
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower == "ball" || lower == "football" || lower == "soccerball" || lower == "soccer_ball")
            {
                m_cachedBall = child;
                break;
            }
        }
    }

    if (!m_cachedBall) return false;

    BallState newBall{};
    newBall.isWelded = (FindChild(m_cachedBall, "playerWeld") != 0);

    uintptr_t primitive = ReadPtr(m_cachedBall + Offsets::BasePart::Primitive);
    if (!primitive) { m_cachedBall = 0; return false; }

    newBall.exists   = true;
    newBall.position = ReadT<Vector3>(primitive + Offsets::Primitive::Position);
    newBall.velocity = ReadT<Vector3>(primitive + Offsets::Primitive::AssemblyLinearVelocity);
    newBall.angularVelocity = ReadT<Vector3>(primitive + Offsets::Primitive::AssemblyAngularVelocity);

    { std::lock_guard<std::mutex> lk(m_stateMtx); m_ball = newBall; }
    return true;
}

// --------------------------------------------------------------------------
// ReadGKState — verifica se o local player é goleiro
//
// APG = Away PenaltyGoalie, HPG = Home PenaltyGoalie
// Se m_forceGK estiver ativado, assume que somos GK e determina gol mais próximo.
// --------------------------------------------------------------------------
bool RobloxReader::ReadGKState()
{
    GKState newGK{};
    bool    newIsAPG = false;

    if (!m_workspace || !m_localPlayer)
    {
        std::lock_guard<std::mutex> lk(m_stateMtx); m_gkState = newGK; m_isAPG = newIsAPG;
        return false;
    }

    bool foundInBools = false;
    uintptr_t boolsFolder = FindChild(m_workspace, "Bools");
    if (boolsFolder)
    {
        uintptr_t apgObj = FindChild(boolsFolder, "APG");
        uintptr_t hpgObj = FindChild(boolsFolder, "HPG");

        uintptr_t apgPlayer = apgObj ? ReadPtr(apgObj + Offsets::Misc::Value) : 0;
        uintptr_t hpgPlayer = hpgObj ? ReadPtr(hpgObj + Offsets::Misc::Value) : 0;

        if (apgPlayer == m_localPlayer)      { newGK.isGK = true; newIsAPG = true;  foundInBools = true; }
        else if (hpgPlayer == m_localPlayer) { newGK.isGK = true; newIsAPG = false; foundInBools = true; }
    }

    if (!foundInBools)
    {
        if (m_forceGK) { newGK.isGK = true; }
        else
        {
            std::lock_guard<std::mutex> lk(m_stateMtx); m_gkState = newGK; m_isAPG = newIsAPG;
            return false;
        }
    }

    uintptr_t model = ReadPtr(m_localPlayer + Offsets::Player::ModelInstance);
    if (!model) { std::lock_guard<std::mutex> lk(m_stateMtx); m_gkState = newGK; m_isAPG = newIsAPG; return false; }

    uintptr_t hrp = FindChild(model, "HumanoidRootPart");
    if (!hrp)   { std::lock_guard<std::mutex> lk(m_stateMtx); m_gkState = newGK; m_isAPG = newIsAPG; return false; }

    newGK.position = ReadPartPosition(hrp);

    uintptr_t hrpPrim = ReadPtr(hrp + Offsets::BasePart::Primitive);
    if (hrpPrim)
    {
        Matrix3x3 rot = ReadT<Matrix3x3>(hrpPrim + Offsets::Primitive::Rotation);
        newGK.rightVec = rot.Right();
        newGK.upVec    = rot.Up();
        newGK.lookVec  = rot.Look();
    }

    // Lê hitbox física do GK (part "Hitbox" dentro do model)
    uintptr_t hitboxPart = FindChild(model, "Hitbox");
    if (hitboxPart)
    {
        newGK.hitboxPos  = ReadPartPosition(hitboxPart);
        newGK.hitboxSize = ReadPartSize(hitboxPart);
    }

    if (!foundInBools && m_forceGK)
    {
        uintptr_t awayGoal = FindChild(m_workspace, "AwayGoal");
        uintptr_t homeGoal = FindChild(m_workspace, "HomeGoal");

        Vector3 awayPos, homePos;
        bool hasAway = false, hasHome = false;

        auto tryGetPos = [&](uintptr_t goalModel, Vector3& outPos) -> bool {
            if (!goalModel) return false;
            uintptr_t pp = ReadPtr(goalModel + Offsets::Model::PrimaryPart);
            if (!pp) { for (uintptr_t c : GetChildren(goalModel)) { if (ReadPtr(c + Offsets::BasePart::Primitive)) { pp = c; break; } } }
            if (!pp) return false;
            uintptr_t prim = ReadPtr(pp + Offsets::BasePart::Primitive);
            if (!prim) return false;
            outPos = ReadT<Vector3>(prim + Offsets::Primitive::Position);
            return true;
        };

        hasAway = tryGetPos(awayGoal, awayPos);
        hasHome = tryGetPos(homeGoal, homePos);

        if (hasAway && hasHome)
            newIsAPG = (newGK.position - awayPos).Length() < (newGK.position - homePos).Length();
        else
            newIsAPG = hasAway;
    }

    { std::lock_guard<std::mutex> lk(m_stateMtx); m_gkState = newGK; m_isAPG = newIsAPG; }
    return true;
}

// --------------------------------------------------------------------------
// ReadGoalState — lê posição, tamanho e rotação do gol que o GK defende
//
// Away: Workspace.AwayAntiOwnGoal
// Home: Workspace.HomePosition.HomeAntiOwnGoal
// --------------------------------------------------------------------------
bool RobloxReader::ReadGoalState()
{
    GoalState newGoal{};

    if (!m_workspace || !m_gkState.isGK)
    {
        std::lock_guard<std::mutex> lk(m_stateMtx); m_goalState = newGoal;
        return false;
    }

    uintptr_t part = 0;
    if (m_isAPG)
    {
        part = FindChild(m_workspace, "AwayAntiOwnGoal");
    }
    else
    {
        uintptr_t homePosition = FindChild(m_workspace, "HomePosition");
        if (homePosition)
            part = FindChild(homePosition, "HomeAntiOwnGoal");
    }

    if (!part) { std::lock_guard<std::mutex> lk(m_stateMtx); m_goalState = newGoal; return false; }

    uintptr_t primitive = ReadPtr(part + Offsets::BasePart::Primitive);
    if (!primitive) { std::lock_guard<std::mutex> lk(m_stateMtx); m_goalState = newGoal; return false; }

    newGoal.exists   = true;
    newGoal.position = ReadT<Vector3>(primitive + Offsets::Primitive::Position);
    newGoal.size     = ReadT<Vector3>(primitive + Offsets::Primitive::Size);
    Matrix3x3 rot    = ReadT<Matrix3x3>(primitive + Offsets::Primitive::Rotation);
    newGoal.rightVec = rot.Right();
    newGoal.upVec    = rot.Up();
    newGoal.lookVec  = rot.Look();

    { std::lock_guard<std::mutex> lk(m_stateMtx); m_goalState = newGoal; }
    return true;
}
