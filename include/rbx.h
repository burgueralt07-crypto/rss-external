#pragma once
#include "memory.h"
#include "rmath.h"
#include "offsets.h"
#include <vector>
#include <string>

// --------------------------------------------------------------------------
// Estrutura com os dados de um jogador já processados
// --------------------------------------------------------------------------
struct PlayerData {
    std::string name;
    float       health    = 0.f;
    float       maxHealth = 0.f;
    Vector3     position;       // posição 3D do HumanoidRootPart
    bool        isAlive  = false;
};

// --------------------------------------------------------------------------
// RobloxReader — lê a estrutura do Roblox via RPM usando os offsets
//
// Cadeia de ponteiros principal:
//   FakeDataModel::Pointer (estático)
//     → + FakeDataModel::RealDataModel
//       → DataModel
//         → + DataModel::Workspace → Workspace
//             → + Workspace::CurrentCamera → Camera
//         → + DataModel::ScriptContext → Players (serviço)
//             → filhos = lista de Player
//               → + Player::ModelInstance → Model
//                   → + Model::PrimaryPart → BasePart (HRP)
//                       → + BasePart::Primitive → Primitive
//                           → + Primitive::Position → Vector3
//                   → filho "Humanoid"
//                       → + Humanoid::Health
//                       → + Humanoid::MaxHealth
// --------------------------------------------------------------------------
class RobloxReader {
public:
    explicit RobloxReader(Memory& mem) : m_mem(mem) {}

    // Carrega DataModel, camera e lista de jogadores.
    // Retorna false se qualquer etapa crítica falhar.
    bool Update();

    const std::vector<PlayerData>& GetPlayers()    const { return m_players; }
    const Matrix4x4&               GetViewMatrix() const { return m_viewMatrix; }
    Vector2                        GetViewport()   const { return m_viewport; }
    uintptr_t                      GetLocalPlayer()const { return m_localPlayer; }

    // Debug — expõe estado interno da cadeia de ponteiros
    uintptr_t  GetBase()        const { return m_base; }
    uintptr_t  GetDataModel()   const { return m_dataModel; }
    uintptr_t  GetWorkspace()   const { return m_workspace; }
    uintptr_t  GetCamera()      const { return m_camera; }
    uintptr_t  GetPlayers_()    const { return m_playersService; }
    uintptr_t  GetLocalPtr()    const { return m_localPlayer; }

private:
    template<typename T>
    T ReadT(uintptr_t addr, T def = {}) const {
        auto v = m_mem.Read<T>(addr);
        return v ? *v : def;
    }

    uintptr_t ReadPtr(uintptr_t addr) const {
        return ReadT<uintptr_t>(addr);
    }

    std::string ReadRbxString(uintptr_t addr) const;
    std::string GetInstanceName(uintptr_t instance) const;
    std::string GetInstanceClass(uintptr_t instance) const;
    std::vector<uintptr_t> GetChildren(uintptr_t instance) const;
    uintptr_t FindChild(uintptr_t instance, const std::string& name) const;
    uintptr_t FindChildByClass(uintptr_t instance, const std::string& cls) const;
    Vector3   ReadPartPosition(uintptr_t basePart) const;

    Memory&                  m_mem;
    std::vector<PlayerData>  m_players;
    Matrix4x4                m_viewMatrix;
    Vector2                  m_viewport;
    uintptr_t                m_localPlayer    = 0;
    uintptr_t                m_dataModel      = 0;
    uintptr_t                m_base           = 0;
    uintptr_t                m_workspace      = 0;
    uintptr_t                m_camera         = 0;
    uintptr_t                m_playersService = 0;
};
