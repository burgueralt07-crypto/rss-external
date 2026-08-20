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
// Update — cadeia principal baseada no base de referência
// --------------------------------------------------------------------------
bool RobloxReader::Update()
{
    m_players.clear();

    // 1. Base do módulo
    m_base = m_mem.GetModuleBase(L"RobloxPlayerBeta.exe");
    if (!m_base) return false;

    // 2. DataModel via FakeDataModel estático
    uintptr_t fakeDataModel = ReadPtr(m_base + Offsets::FakeDataModel::Pointer);
    if (!fakeDataModel) return false;

    m_dataModel = ReadPtr(fakeDataModel + Offsets::FakeDataModel::RealDataModel);
    if (!m_dataModel) return false;

    // 3. VisualEngine — ponteiro estático separado, ViewMatrix lida diretamente
    uintptr_t visualEngine = ReadPtr(m_base + Offsets::VisualEngine::Pointer);
    if (visualEngine)
    {
        m_viewMatrix = ReadT<Matrix4x4>(visualEngine + Offsets::VisualEngine::ViewMatrix);
    }

    // 4. Workspace → Camera → Viewport
    m_workspace = FindChildByClass(m_dataModel, "Workspace");
    if (m_workspace)
    {
        m_camera = FindChildByClass(m_workspace, "Camera");
        if (m_camera)
            m_viewport = ReadT<Vector2>(m_camera + Offsets::Camera::ViewportSize);
    }

    // 5. Players service
    m_playersService = FindChildByClass(m_dataModel, "Players");
    if (!m_playersService) return false;

    m_localPlayer = ReadPtr(m_playersService + Offsets::Player::LocalPlayer);

    // 6. Itera jogadores
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
// ReadBallState — lê posição e velocidade da bola no Workspace (robusto)
// --------------------------------------------------------------------------
bool RobloxReader::ReadBallState()
{
    m_ball = {};
    if (!m_workspace) return false;

    // Busca pela bola com comparação case-insensitive
    uintptr_t ballInst = 0;
    for (uintptr_t child : GetChildren(m_workspace))
    {
        std::string name = GetInstanceName(child);
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerName == "ball" || lowerName == "football" || lowerName == "soccerball" || lowerName == "soccer_ball")
        {
            ballInst = child;
            break;
        }
    }

    if (!ballInst) return false;

    m_ball.isWelded = (FindChild(ballInst, "playerWeld") != 0);

    uintptr_t primitive = ReadPtr(ballInst + Offsets::BasePart::Primitive);
    if (!primitive) return false;

    m_ball.exists   = true;
    m_ball.position = ReadT<Vector3>(primitive + Offsets::Primitive::Position);
    m_ball.velocity = ReadT<Vector3>(primitive + Offsets::Primitive::AssemblyLinearVelocity);
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
    m_gkState = {};
    m_isAPG   = false;

    if (!m_workspace || !m_localPlayer) return false;

    bool foundInBools = false;
    uintptr_t boolsFolder = FindChild(m_workspace, "Bools");
    if (boolsFolder)
    {
        uintptr_t apgObj = FindChild(boolsFolder, "APG");
        uintptr_t hpgObj = FindChild(boolsFolder, "HPG");

        uintptr_t apgPlayer = apgObj ? ReadPtr(apgObj + Offsets::Misc::Value) : 0;
        uintptr_t hpgPlayer = hpgObj ? ReadPtr(hpgObj + Offsets::Misc::Value) : 0;

        if (apgPlayer == m_localPlayer)
        {
            m_gkState.isGK = true;
            m_isAPG        = true;
            foundInBools   = true;
        }
        else if (hpgPlayer == m_localPlayer)
        {
            m_gkState.isGK = true;
            m_isAPG        = false;
            foundInBools   = true;
        }
    }

    if (!foundInBools)
    {
        if (m_forceGK)
        {
            m_gkState.isGK = true;
        }
        else
        {
            return false;
        }
    }

    // Posição e rotação do GK (HumanoidRootPart)
    uintptr_t model = ReadPtr(m_localPlayer + Offsets::Player::ModelInstance);
    if (!model) return false;

    uintptr_t hrp = FindChild(model, "HumanoidRootPart");
    if (!hrp) return false;

    m_gkState.position = ReadPartPosition(hrp);

    // Lê a matriz de rotação do Primitive para extrair Right/Up/Look
    uintptr_t hrpPrim = ReadPtr(hrp + Offsets::BasePart::Primitive);
    if (hrpPrim)
    {
        Matrix3x3 rot = ReadT<Matrix3x3>(hrpPrim + Offsets::Primitive::Rotation);
        m_gkState.rightVec = rot.Right();
        m_gkState.upVec    = rot.Up();
        m_gkState.lookVec  = rot.Look();
    }

    // Se forçado (não encontrado na pasta Bools), determina gol por proximidade
    if (!foundInBools && m_forceGK)
    {
        uintptr_t awayGoal = FindChild(m_workspace, "AwayGoal");
        uintptr_t homeGoal = FindChild(m_workspace, "HomeGoal");

        Vector3 awayPos, homePos;
        bool hasAway = false, hasHome = false;

        if (awayGoal)
        {
            uintptr_t primaryPart = ReadPtr(awayGoal + Offsets::Model::PrimaryPart);
            if (!primaryPart)
            {
                for (uintptr_t child : GetChildren(awayGoal))
                {
                    if (ReadPtr(child + Offsets::BasePart::Primitive)) { primaryPart = child; break; }
                }
            }
            if (primaryPart)
            {
                uintptr_t primitive = ReadPtr(primaryPart + Offsets::BasePart::Primitive);
                if (primitive)
                {
                    awayPos = ReadT<Vector3>(primitive + Offsets::Primitive::Position);
                    hasAway = true;
                }
            }
        }

        if (homeGoal)
        {
            uintptr_t primaryPart = ReadPtr(homeGoal + Offsets::Model::PrimaryPart);
            if (!primaryPart)
            {
                for (uintptr_t child : GetChildren(homeGoal))
                {
                    if (ReadPtr(child + Offsets::BasePart::Primitive)) { primaryPart = child; break; }
                }
            }
            if (primaryPart)
            {
                uintptr_t primitive = ReadPtr(primaryPart + Offsets::BasePart::Primitive);
                if (primitive)
                {
                    homePos = ReadT<Vector3>(primitive + Offsets::Primitive::Position);
                    hasHome = true;
                }
            }
        }

        if (hasAway && hasHome)
        {
            float distAway = (m_gkState.position - awayPos).Length();
            float distHome = (m_gkState.position - homePos).Length();
            m_isAPG = (distAway < distHome);
        }
        else if (hasAway)
        {
            m_isAPG = true;
        }
        else
        {
            m_isAPG = false;
        }
    }

    return true;
}

// --------------------------------------------------------------------------
// ReadGoalState — lê posição, tamanho e rotação do gol que o GK defende
//
// APG defende AwayGoal, HPG defende HomeGoal.
// Busca pelo goalModel; tenta PrimaryPart, senão primeiro BasePart filho.
// Também lê a Matrix3x3 de rotação para PointToObjectSpace correto.
// --------------------------------------------------------------------------
bool RobloxReader::ReadGoalState()
{
    m_goalState = {};
    if (!m_workspace || !m_gkState.isGK) return false;

    // Tenta nome principal e alternativo (GoalDetector)
    uintptr_t goalModel = 0;
    if (m_isAPG)
    {
        goalModel = FindChild(m_workspace, "AwayGoal");
        if (!goalModel) goalModel = FindChild(m_workspace, "AwayGoalDetector");
    }
    else
    {
        goalModel = FindChild(m_workspace, "HomeGoal");
        if (!goalModel) goalModel = FindChild(m_workspace, "HomeGoalDetector");
    }
    if (!goalModel) return false;

    // Tenta PrimaryPart do Model primeiro
    uintptr_t primaryPart = ReadPtr(goalModel + Offsets::Model::PrimaryPart);

    // Se não tiver PrimaryPart, pega o primeiro BasePart filho com Primitive válido
    if (!primaryPart)
    {
        for (uintptr_t child : GetChildren(goalModel))
        {
            uintptr_t prim = ReadPtr(child + Offsets::BasePart::Primitive);
            if (prim)
            {
                primaryPart = child;
                break;
            }
        }
    }

    if (!primaryPart) return false;

    uintptr_t primitive = ReadPtr(primaryPart + Offsets::BasePart::Primitive);
    if (!primitive) return false;

    Vector3    pos = ReadT<Vector3>(primitive + Offsets::Primitive::Position);
    Vector3    sz  = ReadT<Vector3>(primitive + Offsets::Primitive::Size);
    Matrix3x3  rot = ReadT<Matrix3x3>(primitive + Offsets::Primitive::Rotation);

    m_goalState.exists   = true;
    m_goalState.position = pos;
    m_goalState.size     = sz;
    m_goalState.rightVec = rot.Right();
    m_goalState.upVec    = rot.Up();
    m_goalState.lookVec  = rot.Look();
    return true;
}
