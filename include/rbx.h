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

private:
    // Helpers de leitura tipada com checagem de null
    template<typename T>
    T ReadT(uintptr_t addr, T def = {}) const {
        auto v = m_mem.Read<T>(addr);
        return v ? *v : def;
    }

    uintptr_t ReadPtr(uintptr_t addr) const {
        return ReadT<uintptr_t>(addr);
    }

    // Lê string Roblox (std::string armazenada como {ptr, len, cap})
    std::string ReadRbxString(uintptr_t addr) const;

    // Encontra um filho de Instance pelo nome
    uintptr_t FindChild(uintptr_t instance, const std::string& name) const;

    // Lê posição 3D do Primitive de uma BasePart
    Vector3 ReadPartPosition(uintptr_t basePart) const;

    // Lê saúde de um Humanoid
    bool ReadHumanoid(uintptr_t humanoid, float& health, float& maxHealth) const;

    Memory&                  m_mem;
    std::vector<PlayerData>  m_players;
    Matrix4x4                m_viewMatrix;
    Vector2                  m_viewport;
    uintptr_t                m_localPlayer = 0;
    uintptr_t                m_dataModel   = 0;
};
