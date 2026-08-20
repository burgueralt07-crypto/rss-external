#include "overlay.h"
#include "renderer.h"
#include "memory.h"
#include "rbx.h"
#include "rmath.h"
#include "autodive.h"

#include <imgui.h>
#include <Windows.h>
#include <string>
#include <cstdio>

static constexpr wchar_t TARGET_WINDOW[]  = L"Roblox";
static constexpr wchar_t TARGET_PROCESS[] = L"RobloxPlayerBeta.exe";

static Memory        g_mem;
static RobloxReader* g_rbx      = nullptr;
static bool          g_menuOpen = true;

struct ESPConfig {
    bool  enabled      = true;
    bool  showBox      = true;
    bool  showHealth   = true;
    bool  showName     = true;
    bool  showDistance = true;
    float maxDistance  = 500.f;
};
static ESPConfig g_cfg;
static AutoDive  g_dive;

// --------------------------------------------------------------------------
static ImU32 HealthColor(float hp, float maxHp)
{
    float ratio = (maxHp > 0.f) ? (hp / maxHp) : 0.f;
    int r = static_cast<int>((1.f - ratio) * 255.f);
    int g = static_cast<int>(ratio          * 255.f);
    return IM_COL32(r, g, 0, 255);
}

// --------------------------------------------------------------------------
static void PollHotkeys()
{
    static bool homePrev = false;
    bool homeNow = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    if (homeNow && !homePrev)
        g_menuOpen = !g_menuOpen;
    homePrev = homeNow;
}

// --------------------------------------------------------------------------
static bool IsRobloxForeground(HWND targetHwnd)
{
    if (!targetHwnd || !IsWindow(targetHwnd)) return false;
    if (GetForegroundWindow() != targetHwnd)  return false;
    WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
    GetWindowPlacement(targetHwnd, &wp);
    return wp.showCmd == SW_SHOWMAXIMIZED || wp.showCmd == SW_SHOWNORMAL;
}

// --------------------------------------------------------------------------
static void DrawESP(ImDrawList* dl,
                    const std::vector<PlayerData>& players,
                    const Matrix4x4& viewMatrix,
                    const Vector2&   viewport,
                    HWND             targetHwnd)
{
    if (!g_cfg.enabled || players.empty()) return;

    // Offset da janela do Roblox na tela — necessário quando não está em tela cheia
    float winOffsetX = 0.f, winOffsetY = 0.f;
    if (targetHwnd)
    {
        // ClientToScreen converte o ponto (0,0) da área cliente para coordenadas de tela
        // Isso é exato independente de bordas, título ou DPI
        POINT pt{ 0, 0 };
        ClientToScreen(targetHwnd, &pt);
        winOffsetX = static_cast<float>(pt.x);
        winOffsetY = static_cast<float>(pt.y);
    }

    // HumanoidRootPart fica na cintura do personagem Roblox
    constexpr float OFFSET_TOP    =  2.8f;
    constexpr float OFFSET_BOTTOM = -2.5f;

    for (const auto& p : players)
    {
        Vector3 posTop    = { p.position.x, p.position.y + OFFSET_TOP,    p.position.z };
        Vector3 posBottom = { p.position.x, p.position.y + OFFSET_BOTTOM, p.position.z };

        Vector2 screenCenter, screenTop, screenBottom;
        if (!WorldToScreen(viewMatrix, p.position, viewport, screenCenter)) continue;
        if (!WorldToScreen(viewMatrix, posTop,      viewport, screenTop))    continue;
        if (!WorldToScreen(viewMatrix, posBottom,   viewport, screenBottom)) continue;

        // Aplica offset da posição da janela no monitor
        screenCenter.x += winOffsetX; screenCenter.y += winOffsetY;
        screenTop.x    += winOffsetX; screenTop.y    += winOffsetY;
        screenBottom.x += winOffsetX; screenBottom.y += winOffsetY;

        float boxH = screenBottom.y - screenTop.y;
        if (boxH < 4.f || boxH > 2000.f) continue;

        float boxW = boxH * 0.4f;
        float x1   = screenCenter.x - boxW * 0.5f;
        float y1   = screenTop.y;
        float x2   = screenCenter.x + boxW * 0.5f;
        float y2   = screenBottom.y;

        ImU32 boxColor = p.maxHealth > 0.f
            ? HealthColor(p.health, p.maxHealth)
            : IM_COL32(255, 255, 255, 220);

        // Corner box
        if (g_cfg.showBox)
        {
            float cw = boxW * 0.25f;
            float ch = boxH * 0.20f;

            dl->AddRect(ImVec2(x1-1,y1-1), ImVec2(x2+1,y2+1), IM_COL32(0,0,0,180), 0.f, 0, 3.f);

            dl->AddLine(ImVec2(x1,    y1),    ImVec2(x1+cw, y1),    boxColor, 2.f);
            dl->AddLine(ImVec2(x1,    y1),    ImVec2(x1,    y1+ch), boxColor, 2.f);
            dl->AddLine(ImVec2(x2-cw, y1),    ImVec2(x2,    y1),    boxColor, 2.f);
            dl->AddLine(ImVec2(x2,    y1),    ImVec2(x2,    y1+ch), boxColor, 2.f);
            dl->AddLine(ImVec2(x1,    y2-ch), ImVec2(x1,    y2),    boxColor, 2.f);
            dl->AddLine(ImVec2(x1,    y2),    ImVec2(x1+cw, y2),    boxColor, 2.f);
            dl->AddLine(ImVec2(x2,    y2-ch), ImVec2(x2,    y2),    boxColor, 2.f);
            dl->AddLine(ImVec2(x2-cw, y2),    ImVec2(x2,    y2),    boxColor, 2.f);
        }

        // Barra de vida
        if (g_cfg.showHealth && p.maxHealth > 0.f)
        {
            float barX    = x1 - 5.f;
            float barFill = y2 - (y2 - y1) * (p.health / p.maxHealth);
            dl->AddRectFilled(ImVec2(barX-1, y1-1), ImVec2(barX+3, y2+1), IM_COL32(0,0,0,180));
            dl->AddRectFilled(ImVec2(barX, barFill), ImVec2(barX+2, y2),   HealthColor(p.health, p.maxHealth));
        }

        // Nome
        if (g_cfg.showName && !p.name.empty())
        {
            ImVec2 sz = ImGui::CalcTextSize(p.name.c_str());
            dl->AddText(ImVec2(screenCenter.x - sz.x*0.5f+1, y1-sz.y-3.f+1), IM_COL32(0,0,0,200),     p.name.c_str());
            dl->AddText(ImVec2(screenCenter.x - sz.x*0.5f,   y1-sz.y-3.f),   IM_COL32(255,255,255,255), p.name.c_str());
        }

        // Distância
        if (g_cfg.showDistance)
        {
            char buf[32];
            float dist = (p.position - Vector3{}).Length();
            std::snprintf(buf, sizeof(buf), "%.0f studs", dist);
            ImVec2 sz = ImGui::CalcTextSize(buf);
            dl->AddText(ImVec2(screenCenter.x - sz.x*0.5f, y2+2.f), IM_COL32(180,180,180,220), buf);
        }
    }
}

// --------------------------------------------------------------------------
static void DrawMenu()
{
    if (!g_menuOpen) return;

    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);

    if (ImGui::Begin("RSS External  [HOME]", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (g_mem.IsValid())
            ImGui::TextColored(ImVec4(0,1,0,1), "Conectado  PID: %lu", g_mem.GetPID());
        else
            ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "Aguardando Roblox...");

        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::Separator();

        ImGui::Checkbox("ESP",       &g_cfg.enabled);
        if (g_cfg.enabled)
        {
            ImGui::Indent();
            ImGui::Checkbox("Box",        &g_cfg.showBox);
            ImGui::Checkbox("Vida",       &g_cfg.showHealth);
            ImGui::Checkbox("Nome",       &g_cfg.showName);
            ImGui::Checkbox("Distancia",  &g_cfg.showDistance);
            ImGui::SliderFloat("Max dist", &g_cfg.maxDistance, 50.f, 2000.f, "%.0f studs");
            ImGui::Unindent();
        }

        ImGui::Separator();

        // --- AutoDive ---
        ImGui::Checkbox("Auto Dive (GK)", &g_dive.cfg.enabled);
        if (g_dive.cfg.enabled)
        {
            ImGui::Indent();
            ImGui::Checkbox("Forcar GK (Ignorar pasta Bools)", &g_dive.cfg.forceGK);
            ImGui::Checkbox("Apenas No Alvo (Gol)",            &g_dive.cfg.onlyInGoal);
            ImGui::Checkbox("Pular em Bola Alta",               &g_dive.cfg.highJump);
            ImGui::SliderFloat("Dist reacao",   &g_dive.cfg.triggerDistance, 5.f,  35.f, "%.0f studs");
            ImGui::SliderFloat("Vel minima",    &g_dive.cfg.minBallSpeed,    0.f,  50.f, "%.0f studs/s");
            ImGui::SliderFloat("Cooldown",      &g_dive.cfg.cooldownSec,     0.5f,  3.f, "%.1f s");
            ImGui::SliderFloat("Margem gol",    &g_dive.cfg.goalMargin,      0.f,   6.f, "%.0f studs");

            if (g_rbx)
            {
                const GKState&             gk   = g_rbx->GetGKState();
                const BallState&           ball = g_rbx->GetBall();
                const GoalState&           goal = g_rbx->GetGoal();
                const AutoDive::DebugInfo& dbg  = g_dive.debug;

                ImGui::Separator();

                // GK status
                if (gk.isGK)
                    ImGui::TextColored(ImVec4(0.2f,1.f,0.2f,1.f), "GK: ATIVO");
                else
                    ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f), "GK: nao detectado");

                // Bola
                ImGui::Text("Bola: %s%s  pos=(%.1f,%.1f,%.1f)",
                    ball.exists ? "sim" : "NAO",
                    ball.isWelded ? " [welded]" : "",
                    ball.position.x, ball.position.y, ball.position.z);
                ImGui::Text("      vel=(%.1f,%.1f,%.1f)  spd=%.1f",
                    ball.velocity.x, ball.velocity.y, ball.velocity.z, ball.velocity.Length());

                // Gol
                if (goal.exists)
                    ImGui::Text("Gol:  pos=(%.1f,%.1f,%.1f) sz=(%.1f,%.1f,%.1f)",
                        goal.position.x, goal.position.y, goal.position.z,
                        goal.size.x, goal.size.y, goal.size.z);
                else
                    ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f), "Gol: NAO encontrado");

                // Debug do AutoDive
                ImGui::Separator();
                ImGui::Text("Dist bola: %.1f", dbg.distToBall);
                ImGui::Text("Bola->Gol: %s", dbg.approaching ? "SIM (no alvo)" : "nao");
                ImGui::Text("RelPos (GK space): X=%.1f  Y=%.1f  Z=%.1f",
                    dbg.relPosX, dbg.relPosY, dbg.relPosZ);
                ImGui::Text("Bola pos: (%.1f, %.1f)  vel: (%.1f, %.1f)",
                    dbg.ballPosX, dbg.ballPosZ, dbg.ballVelX, dbg.ballVelZ);
                ImGui::Text("Gol pos: (%.1f, %.1f)  tam: (%.1f, %.1f)",
                    dbg.goalPosX, dbg.goalPosZ, dbg.goalSizeX, dbg.goalSizeZ);

                ImGui::Spacing();
                ImGui::Text("Status: %s", dbg.blockReason.c_str());
                ImGui::Text("Ultimo dive: %s", g_dive.LastDiveKey());
            }
            ImGui::Unindent();
        }
    }
    ImGui::End();
}

// --------------------------------------------------------------------------
static bool TryAttach()
{
    if (g_mem.IsValid()) return true;
    return g_mem.Attach(TARGET_PROCESS);
}

// --------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    Overlay  overlay;
    Renderer renderer;

    if (!overlay.Init(TARGET_WINDOW))
    {
        MessageBoxW(nullptr, L"Falha ao criar janela overlay.", L"Erro", MB_ICONERROR);
        return 1;
    }

    if (!renderer.Init(overlay.GetHWND(), overlay.GetWidth(), overlay.GetHeight()))
    {
        MessageBoxW(nullptr, L"Falha ao inicializar DirectX 11.", L"Erro", MB_ICONERROR);
        return 1;
    }

    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    g_rbx = new RobloxReader(g_mem);
    TryAttach();

    // Se já conectou antes do loop, inicia a thread imediatamente
    bool diveStarted = false;
    if (g_mem.IsValid())
    {
        g_dive.Start(g_rbx);
        diveStarted = true;
    }

    while (overlay.IsRunning())
    {
        if (!overlay.ProcessMessages()) break;

        overlay.SyncWithTarget();
        renderer.Resize(overlay.GetWidth(), overlay.GetHeight());

        bool wasValid = g_mem.IsValid();
        TryAttach();
        bool isValid  = g_mem.IsValid();

        // Attach recém-obtido → inicia thread de scan
        if (!wasValid && isValid)
        {
            g_dive.Stop();
            g_dive.Start(g_rbx);
            diveStarted = true;
        }
        // Conexão perdida → para thread de scan
        else if (wasValid && !isValid)
        {
            g_dive.Stop();
            diveStarted = false;
        }

        bool robloxActive = IsRobloxForeground(overlay.GetTargetHWND());
        overlay.SetVisible(robloxActive);

        if (!robloxActive) { Sleep(50); continue; }

        PollHotkeys();
        overlay.SetClickThrough(!g_menuOpen);

        if (g_mem.IsValid())
        {
            g_rbx->SetForceGK(g_dive.cfg.forceGK);
            g_rbx->Update();
        }

        renderer.BeginFrame();

        if (g_mem.IsValid() && g_cfg.enabled)
            DrawESP(ImGui::GetBackgroundDrawList(),
                    g_rbx->GetPlayers(),
                    g_rbx->GetViewMatrix(),
                    g_rbx->GetViewport(),
                    overlay.GetTargetHWND());

        DrawMenu();
        renderer.EndFrame();
    }

    g_dive.Stop();
    delete g_rbx;
    renderer.Shutdown();
    g_mem.Detach();
    return 0;
}
