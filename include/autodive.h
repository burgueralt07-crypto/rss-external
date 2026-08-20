#pragma once
#include "rmath.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

// --------------------------------------------------------------------------
// GKState
// --------------------------------------------------------------------------
struct GKState {
    bool    isGK     = false;
    Vector3 position;
    Vector3 rightVec  = { 1.f, 0.f,  0.f };
    Vector3 upVec     = { 0.f, 1.f,  0.f };
    Vector3 lookVec   = { 0.f, 0.f, -1.f };
};

// --------------------------------------------------------------------------
// BallState
// --------------------------------------------------------------------------
struct BallState {
    bool    exists   = false;
    bool    isWelded = false;
    Vector3 position;
    Vector3 velocity;
};

// --------------------------------------------------------------------------
// GoalState
// --------------------------------------------------------------------------
struct GoalState {
    bool    exists   = false;
    Vector3 position;
    Vector3 size;
    Vector3 rightVec  = { 1.f, 0.f, 0.f };
    Vector3 upVec     = { 0.f, 1.f, 0.f };
    Vector3 lookVec   = { 0.f, 0.f, 1.f };
};

// Forward declaration
class RobloxReader;

// --------------------------------------------------------------------------
// AutoDive
// --------------------------------------------------------------------------
class AutoDive {
public:
    AutoDive()  = default;
    ~AutoDive() { Stop(); }

    AutoDive(const AutoDive&)            = delete;
    AutoDive& operator=(const AutoDive&) = delete;

    struct Config {
        bool  enabled         = false;
        bool  forceGK         = false;
        bool  onlyInGoal      = true;
        bool  highJump        = true;
        float triggerDistance = 18.f;
        float cooldownSec     = 1.2f;
        float minBallSpeed    = 8.f;
        float goalMargin      = 2.f;
        int   scanRate        = 240;
    };

    Config cfg;

    void Start(RobloxReader* rbx);
    void Stop();

    void DispatchPendingKeys(HWND targetHwnd)
    {
        WORD vk = static_cast<WORD>(m_pendingKey.exchange(0));
        if (!vk) return;
        SendVKey(targetHwnd, vk);
    }

    const char* LastDiveKey() const { return m_lastKey; }
    bool        DiveFired()   const { return m_firedThisFrame; }

    struct DebugInfo {
        float       distToBall  = 0.f;
        float       relPosX     = 0.f;
        float       relPosY     = 0.f;
        float       relPosZ     = 0.f;
        float       impactX     = 0.f;
        float       impactY     = 0.f;
        bool        approaching = false;
        std::string blockReason;
    };

    mutable DebugInfo debug;

private:
    static void SendVKey(HWND targetHwnd, WORD vk)
    {
        // PostMessage não requer foco e não é bloqueado por UIPI entre processos
        // do mesmo nível de integridade. Funciona mesmo com o overlay em foreground.
        if (!targetHwnd || !IsWindow(targetHwnd)) return;

        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        LPARAM lparamDown = 1 | (sc << 16);
        LPARAM lparamUp   = 1 | (sc << 16) | (1 << 30) | (1 << 31);

        PostMessageW(targetHwnd, WM_KEYDOWN, vk, lparamDown);
        PostMessageW(targetHwnd, WM_KEYUP,   vk, lparamUp);
    }

    void PressKey(WORD vk) { m_pendingKey.store(vk); }

    static Vector3 PointToObjectSpace(const Vector3& origin,
                                      const Vector3& right,
                                      const Vector3& up,
                                      const Vector3& look,
                                      const Vector3& worldPos);

    bool IsBallTargetingGoal(const BallState& ball, const GoalState& goal) const;
    void ScanLoop(RobloxReader* rbx);
    void Evaluate(const GKState& gk, const BallState& ball, const GoalState& goal);

    std::chrono::steady_clock::time_point m_lastDiveTime;
    bool        m_firedThisFrame = false;
    const char* m_lastKey        = "-";

    std::atomic<uint16_t> m_pendingKey{ 0 };
    std::thread           m_thread;
    std::atomic<bool>     m_running{ false };
};
