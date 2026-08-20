#pragma once
#include "memory.h"
#include "rmath.h"
#include "autodive.h"
#include "offsets.h"
#include <vector>
#include <string>

struct PlayerData {
    std::string name;
    float       health    = 0.f;
    float       maxHealth = 0.f;
    Vector3     position;
    bool        isAlive   = false;
};

class RobloxReader {
public:
    explicit RobloxReader(Memory& mem) : m_mem(mem) {}

    bool Update();

    const std::vector<PlayerData>& GetPlayers()    const { return m_players; }
    const Matrix4x4&               GetViewMatrix() const { return m_viewMatrix; }
    Vector2                        GetViewport()   const { return m_viewport; }
    const BallState&               GetBall()       const { return m_ball; }
    const GKState&                 GetGKState()    const { return m_gkState; }

private:
    template<typename T>
    T ReadT(uintptr_t addr, T def = {}) const {
        auto v = m_mem.Read<T>(addr);
        return v ? *v : def;
    }

    uintptr_t ReadPtr(uintptr_t addr) const {
        return ReadT<uintptr_t>(addr);
    }

    std::string            ReadRbxString(uintptr_t addr) const;
    std::string            GetInstanceName(uintptr_t instance) const;
    std::string            GetInstanceClass(uintptr_t instance) const;
    std::vector<uintptr_t> GetChildren(uintptr_t instance) const;
    uintptr_t              FindChild(uintptr_t instance, const std::string& name) const;
    uintptr_t              FindChildByClass(uintptr_t instance, const std::string& cls) const;
    Vector3                ReadPartPosition(uintptr_t basePart) const;
    bool                   ReadBallState();
    bool                   ReadGKState();

    Memory&                  m_mem;
    std::vector<PlayerData>  m_players;
    Matrix4x4                m_viewMatrix;
    Vector2                  m_viewport;
    uintptr_t                m_base           = 0;
    uintptr_t                m_dataModel      = 0;
    uintptr_t                m_workspace      = 0;
    uintptr_t                m_camera         = 0;
    uintptr_t                m_playersService = 0;
    uintptr_t                m_localPlayer    = 0;
    BallState                m_ball;
    GKState                  m_gkState;
};
