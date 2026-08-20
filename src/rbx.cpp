#include "rbx.h"
#include <algorithm>
#include <cstring>

// --------------------------------------------------------------------------
// ReadRbxString
//
// std::string do MSVC (x64):
//   [+0x00] union { char buf[16]; char* ptr; }  — dados inline se len<=15, heap se len>15
//   [+0x10] size_t size
//   [+0x18] size_t capacity
//
// Se capacity <= 15 os bytes ficam inline em [addr+0x00].
// Se capacity >  15 existe um ponteiro heap em [addr+0x00].
// --------------------------------------------------------------------------
std::string RobloxReader::ReadRbxString(uintptr_t addr) const
{
    if (!addr) return {};

    size_t len = ReadT<size_t>(addr + 0x10);   // size
    if (len == 0 || len > 512) return {};

    size_t cap = ReadT<size_t>(addr + 0x18);   // capacity

    std::string result(len, '\0');

    if (cap > 15)
    {
        // heap — [addr+0x00] é ponteiro para os dados
        uintptr_t dataPtr = ReadPtr(addr);
        if (!dataPtr) return {};
        m_mem.ReadRaw(dataPtr, result.data(), len);
    }
    else
    {
        // inline — os bytes começam em addr+0x00
        m_mem.ReadRaw(addr, result.data(), len);
    }

    return result;
}

// --------------------------------------------------------------------------
// FindChild — percorre a lista de filhos de uma Instance pelo nome
//
// Estrutura do vetor de filhos no Roblox:
//   instance + 0x70  = NameContainer (ptr para std::string do nome)
//   instance + 0x78  = ptr para início do vetor de filhos (std::vector begin)
//   instance + 0x80  = ptr para fim do vetor de filhos   (std::vector end)
//
// Cada elemento do vetor é um ponteiro de 8 bytes para outra Instance.
// --------------------------------------------------------------------------
uintptr_t RobloxReader::FindChild(uintptr_t instance, const std::string& name) const
{
    if (!instance) return 0;

    // O vetor de filhos está em instance+0x78 (begin) e instance+0x80 (end)
    uintptr_t vecBegin = ReadPtr(instance + Offsets::Instance::ChildrenStart);
    uintptr_t vecEnd   = ReadPtr(instance + Offsets::Instance::ChildrenStart + 0x8);

    if (!vecBegin || !vecEnd || vecEnd <= vecBegin) return 0;

    size_t count = (vecEnd - vecBegin) / sizeof(uintptr_t);
    if (count == 0 || count > 1024) return 0;

    for (size_t i = 0; i < count; ++i)
    {
        uintptr_t child = ReadPtr(vecBegin + i * sizeof(uintptr_t));
        if (!child) continue;

        // Nome da instância: child + 0x70 = NameContainer, NameContainer + 0x8 = std::string
        uintptr_t nameContainer = ReadPtr(child + Offsets::Instance::NameContainer);
        if (!nameContainer) continue;

        std::string childName = ReadRbxString(nameContainer);
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
