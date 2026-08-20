#include "rbx.h"
#include <algorithm>
#include <cstring>

// --------------------------------------------------------------------------
// ReadRbxString
//
// O Roblox armazena strings como uma struct:
//   [0x00] char* data  (ou buffer inline se len <= 15)
//   [0x10] size_t len
//   [0x18] size_t cap
//
// Se cap > 15, 'data' é um ponteiro heap; caso contrário, inline no próprio slot.
// --------------------------------------------------------------------------
std::string RobloxReader::ReadRbxString(uintptr_t addr) const
{
    constexpr size_t INLINE_CAP = 15;

    size_t len = ReadT<size_t>(addr + Offsets::Misc::StringLength);
    if (len == 0 || len > 256)
        return {};

    std::string result(len, '\0');

    size_t cap = ReadT<size_t>(addr + 0x18);
    if (cap > INLINE_CAP)
    {
        // string heap-allocated: lê o ponteiro e depois os bytes
        uintptr_t dataPtr = ReadPtr(addr);
        if (!dataPtr) return {};
        m_mem.ReadRaw(dataPtr, result.data(), len);
    }
    else
    {
        // string inline: os bytes ficam no próprio slot (addr + 0x00)
        m_mem.ReadRaw(addr, result.data(), len);
    }

    return result;
}

// --------------------------------------------------------------------------
// FindChild — percorre a lista de filhos de uma Instance pelo nome
//
// Instance::ChildrenStart → ponteiro para vetor de filhos
// Cada elemento: [ptr para Instance]
// Instance::NameContainer + Instance::Name → std::string
// --------------------------------------------------------------------------
uintptr_t RobloxReader::FindChild(uintptr_t instance, const std::string& name) const
{
    if (!instance) return 0;

    uintptr_t childrenStart = ReadPtr(instance + Offsets::Instance::ChildrenStart);
    uintptr_t childrenEnd   = ReadPtr(instance + Offsets::Instance::ChildrenEnd + 0x70); // ajuste

    // Lê ponteiro para o vetor interno
    uintptr_t vecPtr = ReadPtr(instance + Offsets::Instance::ChildrenStart);
    uintptr_t vecEnd = ReadPtr(instance + Offsets::Instance::ChildrenStart + 0x8);

    if (!vecPtr || !vecEnd || vecEnd <= vecPtr)
        return 0;

    size_t count = (vecEnd - vecPtr) / sizeof(uintptr_t);
    count = std::min(count, static_cast<size_t>(512)); // limite de segurança

    for (size_t i = 0; i < count; ++i)
    {
        uintptr_t child = ReadPtr(vecPtr + i * sizeof(uintptr_t));
        if (!child) continue;

        uintptr_t nameContainer = ReadPtr(child + Offsets::Instance::NameContainer);
        if (!nameContainer) continue;

        std::string childName = ReadRbxString(nameContainer + Offsets::Instance::Name);
        if (childName == name)
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
// ReadHumanoid
// --------------------------------------------------------------------------
bool RobloxReader::ReadHumanoid(uintptr_t humanoid, float& health, float& maxHealth) const
{
    if (!humanoid) return false;
    health    = ReadT<float>(humanoid + Offsets::Humanoid::Health);
    maxHealth = ReadT<float>(humanoid + Offsets::Humanoid::MaxHealth);
    return maxHealth > 0.f;
}

// --------------------------------------------------------------------------
// Update — cadeia principal de leitura
// --------------------------------------------------------------------------
bool RobloxReader::Update()
{
    m_players.clear();

    // 1. DataModel via FakeDataModel estático
    m_base = m_mem.GetModuleBase(L"RobloxPlayerBeta.exe");
    if (!m_base) return false;

    uintptr_t fakeDataModel = ReadPtr(m_base + Offsets::FakeDataModel::Pointer);
    if (!fakeDataModel) return false;

    m_dataModel = ReadPtr(fakeDataModel + Offsets::FakeDataModel::RealDataModel);
    if (!m_dataModel) return false;

    // 2. Workspace → Camera → ViewMatrix + Viewport
    m_workspace = ReadPtr(m_dataModel + Offsets::DataModel::Workspace);
    if (m_workspace)
    {
        m_camera = ReadPtr(m_workspace + Offsets::Workspace::CurrentCamera);
        if (m_camera)
        {
            m_viewMatrix = ReadT<Matrix4x4>(m_camera + Offsets::Camera::Rotation);
            m_viewport   = ReadT<Vector2>  (m_camera + Offsets::Camera::ViewportSize);
        }
    }

    // 3. Players service
    m_playersService = FindChild(m_dataModel, "Players");
    if (!m_playersService) return false;

    m_localPlayer = ReadPtr(m_playersService + Offsets::Player::LocalPlayer);

    uintptr_t vecPtr = ReadPtr(m_playersService + Offsets::Instance::ChildrenStart);
    uintptr_t vecEnd = ReadPtr(m_playersService + Offsets::Instance::ChildrenStart + 0x8);
    if (!vecPtr || !vecEnd || vecEnd <= vecPtr) return false;

    size_t count = (vecEnd - vecPtr) / sizeof(uintptr_t);
    count = std::min(count, static_cast<size_t>(100));

    for (size_t i = 0; i < count; ++i)
    {
        uintptr_t playerInst = ReadPtr(vecPtr + i * sizeof(uintptr_t));
        if (!playerInst) continue;

        if (playerInst == m_localPlayer) continue;

        PlayerData pd;

        uintptr_t nameContainer = ReadPtr(playerInst + Offsets::Instance::NameContainer);
        if (nameContainer)
            pd.name = ReadRbxString(nameContainer + Offsets::Instance::Name);
        if (pd.name.empty()) continue;

        uintptr_t model = ReadPtr(playerInst + Offsets::Player::ModelInstance);
        if (!model) continue;

        uintptr_t humanoid = FindChild(model, "Humanoid");
        if (!humanoid) continue;

        ReadHumanoid(humanoid, pd.health, pd.maxHealth);
        pd.isAlive = pd.health > 0.f;
        if (!pd.isAlive) continue;

        uintptr_t hrp = ReadPtr(humanoid + Offsets::Humanoid::HumanoidRootPart);
        if (!hrp)
            hrp = FindChild(model, "HumanoidRootPart");

        pd.position = ReadPartPosition(hrp);

        m_players.push_back(pd);
    }

    return true;
}
