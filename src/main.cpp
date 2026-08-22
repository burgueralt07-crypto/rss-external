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
#include <cstring>
#include <vector>
#include <algorithm>

static constexpr wchar_t TARGET_WINDOW[]  = L"Roblox";
static constexpr wchar_t TARGET_PROCESS[] = L"RobloxPlayerBeta.exe";

static Memory        g_mem;
static RobloxReader* g_rbx      = nullptr;
static bool          g_menuOpen = true;
static int           g_menuKey  = VK_HOME;

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
    static int  prevKey  = -1;
    static bool prevDown = false;

    bool down = (GetAsyncKeyState(g_menuKey) & 0x8000) != 0;
    if (down && !prevDown)
        g_menuOpen = !g_menuOpen;

    if (g_menuKey != prevKey) { prevDown = false; prevKey = g_menuKey; }
    else                        prevDown = down;
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

    float winOffsetX = 0.f, winOffsetY = 0.f;
    if (targetHwnd)
    {
        POINT pt{ 0, 0 };
        ClientToScreen(targetHwnd, &pt);
        winOffsetX = static_cast<float>(pt.x);
        winOffsetY = static_cast<float>(pt.y);
    }

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

        if (g_cfg.showHealth && p.maxHealth > 0.f)
        {
            float barX    = x1 - 5.f;
            float barFill = y2 - (y2 - y1) * (p.health / p.maxHealth);
            dl->AddRectFilled(ImVec2(barX-1, y1-1), ImVec2(barX+3, y2+1), IM_COL32(0,0,0,180));
            dl->AddRectFilled(ImVec2(barX, barFill), ImVec2(barX+2, y2),   HealthColor(p.health, p.maxHealth));
        }

        if (g_cfg.showName && !p.name.empty())
        {
            ImVec2 sz = ImGui::CalcTextSize(p.name.c_str());
            dl->AddText(ImVec2(screenCenter.x - sz.x*0.5f+1, y1-sz.y-3.f+1), IM_COL32(0,0,0,200),     p.name.c_str());
            dl->AddText(ImVec2(screenCenter.x - sz.x*0.5f,   y1-sz.y-3.f),   IM_COL32(255,255,255,255), p.name.c_str());
        }

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

// ==========================================================================
// Config — leitura/escrita em .ini com slots nomeados
// ==========================================================================

static std::string GetConfigDir()
{
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    char* last = strrchr(buf, '\\');
    if (last) *(last + 1) = '\0';
    return std::string(buf);
}

// Escreve todas as entradas de config num FILE já aberto
static void WriteConfigEntries(FILE* f)
{
    const AutoDive::Config& c = g_dive.cfg;

    fprintf(f, "[ESP]\n");
    fprintf(f, "esp_enabled=%d\n",      g_cfg.enabled     ? 1 : 0);
    fprintf(f, "esp_showBox=%d\n",      g_cfg.showBox      ? 1 : 0);
    fprintf(f, "esp_showHealth=%d\n",   g_cfg.showHealth   ? 1 : 0);
    fprintf(f, "esp_showName=%d\n",     g_cfg.showName     ? 1 : 0);
    fprintf(f, "esp_showDistance=%d\n", g_cfg.showDistance ? 1 : 0);
    fprintf(f, "esp_maxDistance=%.1f\n", g_cfg.maxDistance);

    fprintf(f, "\n[AutoDive]\n");
    fprintf(f, "ad_enabled=%d\n",              c.enabled       ? 1 : 0);
    fprintf(f, "ad_forceGK=%d\n",              c.forceGK       ? 1 : 0);
    fprintf(f, "ad_onlyInGoal=%d\n",           c.onlyInGoal    ? 1 : 0);
    fprintf(f, "ad_highJump=%d\n",             c.highJump      ? 1 : 0);
    fprintf(f, "ad_gameMode=%d\n",             static_cast<int>(c.gameMode));
    fprintf(f, "ad_triggerDistance=%.2f\n",    c.triggerDistance);
    fprintf(f, "ad_cooldownSec=%.2f\n",        c.cooldownSec);
    fprintf(f, "ad_minBallSpeed=%.2f\n",       c.minBallSpeed);
    fprintf(f, "ad_goalMargin=%.2f\n",         c.goalMargin);
    fprintf(f, "ad_diveXThreshold=%.2f\n",     c.diveXThreshold);
    fprintf(f, "ad_jumpYThreshold=%.2f\n",     c.jumpYThreshold);
    fprintf(f, "ad_jumpXMaxForPure=%.2f\n",    c.jumpXMaxForPure);
    fprintf(f, "ad_diveXThreshold7v7=%.2f\n",  c.diveXThreshold7v7);
    fprintf(f, "ad_jumpDiveXMin7v7=%.2f\n",    c.jumpDiveXMin7v7);
    fprintf(f, "ad_jumpPureXMax7v7=%.2f\n",    c.jumpPureXMax7v7);
    fprintf(f, "ad_jumpDiveDelayMs=%d\n",      c.jumpDiveDelayMs);
    fprintf(f, "ad_jumpDiveTimeWindow=%.2f\n", c.jumpDiveTimeWindow);
    fprintf(f, "ad_simSteps=%d\n",             c.simSteps);
    fprintf(f, "ad_simDt=%.4f\n",              c.simDt);
    fprintf(f, "ad_gravity=%.4f\n",            c.gravity);
    fprintf(f, "ad_magnusCoeff=%.4f\n",        c.magnusCoeff);
    fprintf(f, "ad_dragCoeff=%.4f\n",          c.dragCoeff);
    fprintf(f, "ad_watchRange=%.2f\n",         c.watchRange);
    fprintf(f, "ad_diveFireDistance=%.2f\n",   c.diveFireDistance);
    fprintf(f, "ad_scanRate=%d\n",             c.scanRate);

    fprintf(f, "\n[Misc]\n");
    fprintf(f, "misc_menuKey=%d\n", g_menuKey);
}

// Lê as entradas de um FILE já aberto e aplica nas structs globais
static void ReadConfigEntries(FILE* f)
{
    AutoDive::Config& c = g_dive.cfg;
    char line[256];

#define BOOL_KEY(k, field)  if (!strcmp(key, k)) { field = (val[0] == '1'); }
#define FLOAT_KEY(k, field) if (!strcmp(key, k)) { field = (float)atof(val); }
#define INT_KEY(k, field)   if (!strcmp(key, k)) { field = atoi(val); }

    while (fgets(line, sizeof(line), f))
    {
        // Pula linhas de seção ([ESP], [AutoDive], etc.) e linhas em branco
        if (line[0] == '[' || line[0] == '\n' || line[0] == '\r' || line[0] == '#')
            continue;

        char key[64] = {}, val[64] = {};
        if (sscanf(line, " %63[^=]=%63[^\r\n]", key, val) != 2) continue;

        // Remove espaços no fim da chave
        int klen = (int)strlen(key);
        while (klen > 0 && key[klen-1] == ' ') key[--klen] = '\0';

        BOOL_KEY("esp_enabled",      g_cfg.enabled)
        BOOL_KEY("esp_showBox",      g_cfg.showBox)
        BOOL_KEY("esp_showHealth",   g_cfg.showHealth)
        BOOL_KEY("esp_showName",     g_cfg.showName)
        BOOL_KEY("esp_showDistance", g_cfg.showDistance)
        FLOAT_KEY("esp_maxDistance", g_cfg.maxDistance)

        BOOL_KEY("ad_enabled",    c.enabled)
        BOOL_KEY("ad_forceGK",    c.forceGK)
        BOOL_KEY("ad_onlyInGoal", c.onlyInGoal)
        BOOL_KEY("ad_highJump",   c.highJump)
        if (!strcmp(key, "ad_gameMode")) c.gameMode = static_cast<GameMode>(atoi(val));
        FLOAT_KEY("ad_triggerDistance",    c.triggerDistance)
        FLOAT_KEY("ad_cooldownSec",        c.cooldownSec)
        FLOAT_KEY("ad_minBallSpeed",       c.minBallSpeed)
        FLOAT_KEY("ad_goalMargin",         c.goalMargin)
        FLOAT_KEY("ad_diveXThreshold",     c.diveXThreshold)
        FLOAT_KEY("ad_jumpYThreshold",     c.jumpYThreshold)
        FLOAT_KEY("ad_jumpXMaxForPure",    c.jumpXMaxForPure)
        FLOAT_KEY("ad_diveXThreshold7v7",  c.diveXThreshold7v7)
        FLOAT_KEY("ad_jumpDiveXMin7v7",    c.jumpDiveXMin7v7)
        FLOAT_KEY("ad_jumpPureXMax7v7",    c.jumpPureXMax7v7)
        INT_KEY("ad_jumpDiveDelayMs",      c.jumpDiveDelayMs)
        FLOAT_KEY("ad_jumpDiveTimeWindow", c.jumpDiveTimeWindow)
        INT_KEY("ad_simSteps",             c.simSteps)
        FLOAT_KEY("ad_simDt",              c.simDt)
        FLOAT_KEY("ad_gravity",            c.gravity)
        FLOAT_KEY("ad_magnusCoeff",        c.magnusCoeff)
        FLOAT_KEY("ad_dragCoeff",          c.dragCoeff)
        FLOAT_KEY("ad_watchRange",         c.watchRange)
        FLOAT_KEY("ad_diveFireDistance",   c.diveFireDistance)
        INT_KEY("ad_scanRate",             c.scanRate)
        INT_KEY("misc_menuKey",            g_menuKey)
    }

#undef BOOL_KEY
#undef FLOAT_KEY
#undef INT_KEY
}

// Salva no slot com nome dado (cria "configs/<nome>.ini")
static void SaveConfigSlot(const char* name)
{
    std::string dir = GetConfigDir() + "configs\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string path = dir + name + std::string(".ini");
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    WriteConfigEntries(f);
    fclose(f);
}

// Carrega do slot com nome dado
static bool LoadConfigSlot(const char* name)
{
    std::string path = GetConfigDir() + "configs\\" + name + std::string(".ini");
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    ReadConfigEntries(f);
    fclose(f);
    return true;
}

// Lista todos os .ini em configs/
static std::vector<std::string> ListConfigSlots()
{
    std::vector<std::string> slots;
    std::string pattern = GetConfigDir() + "configs\\*.ini";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return slots;
    do {
        std::string name = fd.cFileName;
        // Remove extensão .ini
        if (name.size() > 4) name = name.substr(0, name.size() - 4);
        slots.push_back(name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(slots.begin(), slots.end());
    return slots;
}

// Arquivo de meta-config (qual slot auto-carregar)
static std::string GetMetaPath() { return GetConfigDir() + "rss_meta.ini"; }

static void SaveMeta(const char* autoSlot, bool autoLoad)
{
    FILE* f = fopen(GetMetaPath().c_str(), "w");
    if (!f) return;
    fprintf(f, "autoLoad=%d\n", autoLoad ? 1 : 0);
    fprintf(f, "autoSlot=%s\n", autoSlot);
    fclose(f);
}

static void LoadMeta(char* outSlot, int slotBuf, bool& outAutoLoad)
{
    outSlot[0]  = '\0';
    outAutoLoad = false;
    FILE* f = fopen(GetMetaPath().c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char key[64]={}, val[64]={};
        if (sscanf(line, " %63[^=]=%63[^\r\n]", key, val) != 2) continue;
        if (!strcmp(key, "autoLoad")) outAutoLoad = (val[0] == '1');
        if (!strcmp(key, "autoSlot")) { strncpy(outSlot, val, slotBuf-1); outSlot[slotBuf-1]='\0'; }
    }
    fclose(f);
}

// ==========================================================================
// UI — nome da tecla para exibição
static const char* VKToName(int vk)
{
    UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    static char buf[32];
    if (GetKeyNameTextA(static_cast<LONG>(sc << 16), buf, sizeof(buf)) > 0) return buf;
    return "?";
}

// ==========================================================================
static void DrawMenu(Overlay& overlay)
{
    if (!g_menuOpen) return;

    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);

    char menuTitle[64];
    std::snprintf(menuTitle, sizeof(menuTitle), "RSS External  [%s]", VKToName(g_menuKey));

    if (ImGui::Begin(menuTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (g_mem.IsValid())
            ImGui::TextColored(ImVec4(0,1,0,1), "Conectado  PID: %lu", g_mem.GetPID());
        else
            ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "Aguardando Roblox...");

        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::Separator();

        if (ImGui::BeginTabBar("MainTabs"))
        {
            // ── Aba: Visuals ─────────────────────────────────────────────
            if (ImGui::BeginTabItem("Visuals"))
            {
                ImGui::Checkbox("ESP", &g_cfg.enabled);
                if (g_cfg.enabled)
                {
                    ImGui::Indent();
                    ImGui::Checkbox("Box",       &g_cfg.showBox);
                    ImGui::Checkbox("Vida",      &g_cfg.showHealth);
                    ImGui::Checkbox("Nome",      &g_cfg.showName);
                    ImGui::Checkbox("Distancia", &g_cfg.showDistance);
                    ImGui::SliderFloat("Max dist", &g_cfg.maxDistance, 50.f, 2000.f, "%.0f studs");
                    ImGui::Unindent();
                }
                ImGui::EndTabItem();
            }

            // ── Aba: Realistic Street Soccer ─────────────────────────────
            if (ImGui::BeginTabItem("Realistic Street Soccer"))
            {
                ImGui::Checkbox("Auto Dive (GK)", &g_dive.cfg.enabled);
                if (g_dive.cfg.enabled)
                {
                    ImGui::Indent();
                    ImGui::Checkbox("Forcar GK (Ignorar pasta Bools)", &g_dive.cfg.forceGK);
                    ImGui::Checkbox("Apenas No Alvo (Gol)",            &g_dive.cfg.onlyInGoal);

                    static const char* kModeNames[] = { "4v4  (gol pequeno)", "7v7  (gol grande)" };
                    int modeIdx = static_cast<int>(g_dive.cfg.gameMode);
                    if (ImGui::Combo("Modo", &modeIdx, kModeNames, 2))
                        g_dive.cfg.gameMode = static_cast<GameMode>(modeIdx);

                    ImGui::Separator();
                    ImGui::SliderFloat("Dist reacao",   &g_dive.cfg.triggerDistance, 5.f,  40.f, "%.0f studs");
                    ImGui::SliderFloat("Vel minima",    &g_dive.cfg.minBallSpeed,    0.f,  50.f, "%.0f studs/s");
                    ImGui::SliderFloat("Cooldown",      &g_dive.cfg.cooldownSec,     0.3f,  3.f, "%.1f s");
                    ImGui::SliderFloat("Margem gol",    &g_dive.cfg.goalMargin,      0.f,   8.f, "%.0f studs");

                    if (g_dive.cfg.gameMode == GameMode::Mode4v4)
                    {
                        ImGui::Separator();
                        ImGui::TextDisabled("-- 4v4 --");
                        ImGui::Checkbox("Pular em Bola Alta (Space)", &g_dive.cfg.highJump);
                        ImGui::SliderFloat("relX dive   [4v4]", &g_dive.cfg.diveXThreshold,  0.5f, 8.f,  "%.1f");
                        if (g_dive.cfg.highJump)
                        {
                            ImGui::SliderFloat("relY jump   [4v4]", &g_dive.cfg.jumpYThreshold,  2.f, 12.f, "%.1f");
                            ImGui::SliderFloat("|relX| max jump [4v4]", &g_dive.cfg.jumpXMaxForPure, 1.f, 10.f, "%.1f");
                        }
                    }
                    else
                    {
                        ImGui::Separator();
                        ImGui::TextDisabled("-- 7v7 --");
                        ImGui::Checkbox("Pular em Bola Alta (Space) [7v7]", &g_dive.cfg.highJump);
                        ImGui::SliderFloat("relX dive      [7v7]", &g_dive.cfg.diveXThreshold7v7,  1.f, 12.f, "%.1f");
                        ImGui::SliderFloat("|relX| Jump puro[7v7]",&g_dive.cfg.jumpPureXMax7v7,    0.f,  6.f, "%.1f");
                        ImGui::SliderFloat("|relX| min J+D [7v7]", &g_dive.cfg.jumpDiveXMin7v7,    0.f,  6.f, "%.1f");
                        ImGui::SliderInt("Delay Space->Q/E (ms)",  &g_dive.cfg.jumpDiveDelayMs,     0,  400);
                        ImGui::SliderFloat("Janela antecip. dive [7v7]", &g_dive.cfg.jumpDiveTimeWindow, 0.f, 1.5f, "%.2f s");
                    }

                    if (g_rbx)
                    {
                        const GKState&   gk   = g_rbx->GetGKState();
                        const GoalState& goal = g_rbx->GetGoal();
                        ImGui::Separator();
                        if (gk.isGK)
                            ImGui::TextColored(ImVec4(0.2f,1.f,0.2f,1.f), "GK: ATIVO");
                        else
                            ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f), "GK: nao detectado");
                        if (goal.exists)
                            ImGui::TextColored(ImVec4(0.2f,1.f,0.2f,1.f), "Gol: encontrado");
                        else
                            ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f), "Gol: NAO encontrado");
                    }
                    ImGui::Unindent();
                }
                ImGui::EndTabItem();
            }

            // ── Aba: Misc ─────────────────────────────────────────────────
            if (ImGui::BeginTabItem("Misc"))
            {
                // ── Streamproof ───────────────────────────────────────────
                static bool s_streamproof = false;
                if (ImGui::Checkbox("Streamproof", &s_streamproof))
                    overlay.SetStreamproof(s_streamproof);
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Oculta o overlay em gravacoes,\ntransmissoes (OBS, Discord, etc.)");

                ImGui::Separator();

                // ── Hotkey binding ────────────────────────────────────────
                // Clica no botão → fica aguardando a próxima tecla
                static bool s_waitingKey = false;

                ImGui::AlignTextToFramePadding();
                ImGui::Text("Abrir/fechar menu:");
                ImGui::SameLine();

                if (s_waitingKey)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.4f, 0.0f, 1.f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.5f, 0.0f, 1.f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.8f, 0.6f, 0.0f, 1.f));
                    ImGui::Button("Aguardando tecla... (Esc cancela)", ImVec2(-1, 0));
                    ImGui::PopStyleColor(3);

                    // Varre VKs para capturar a primeira tecla pressionada
                    // Ignora modificadores sozinhos e Escape cancela
                    for (int vk = 1; vk < 256; ++vk)
                    {
                        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)      continue;
                        if (vk == VK_LSHIFT || vk == VK_RSHIFT)                       continue;
                        if (vk == VK_LCONTROL || vk == VK_RCONTROL)                   continue;
                        if (vk == VK_LMENU || vk == VK_RMENU)                         continue;

                        if (GetAsyncKeyState(vk) & 0x8000)
                        {
                            if (vk == VK_ESCAPE)
                                s_waitingKey = false;
                            else {
                                g_menuKey    = vk;
                                s_waitingKey = false;
                            }
                            break;
                        }
                    }
                }
                else
                {
                    char btnLabel[64];
                    std::snprintf(btnLabel, sizeof(btnLabel), "[ %s ]", VKToName(g_menuKey));
                    if (ImGui::Button(btnLabel, ImVec2(-1, 0)))
                        s_waitingKey = true;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Clique para alterar a tecla");
                }

                ImGui::Separator();

                // ── Config slots ──────────────────────────────────────────
                static char  s_newSlotName[64]  = "default";
                static int   s_selectedSlot     = -1;
                static bool  s_autoLoad         = false;
                static char  s_autoSlot[64]     = {};
                static bool  s_metaLoaded       = false;
                static char  s_configMsg[128]   = {};
                static float s_configMsgTimer   = 0.f;

                // Carrega meta uma vez
                if (!s_metaLoaded)
                {
                    LoadMeta(s_autoSlot, sizeof(s_autoSlot), s_autoLoad);
                    s_metaLoaded = true;
                }

                // Lista slots disponíveis (re-varre a cada frame apenas se expanded)
                static std::vector<std::string> s_slots;
                static bool s_slotsStale = true;
                if (s_slotsStale) { s_slots = ListConfigSlots(); s_slotsStale = false; }

                ImGui::TextDisabled("Configs salvas:");

                // Listbox com os slots existentes
                {
                    float listH = ImGui::GetTextLineHeightWithSpacing() * 4.5f;
                    if (ImGui::BeginListBox("##slots", ImVec2(-1, listH)))
                    {
                        if (s_slots.empty())
                            ImGui::TextDisabled("  (nenhuma config salva)");
                        for (int i = 0; i < (int)s_slots.size(); ++i)
                        {
                            bool sel = (s_selectedSlot == i);
                            if (ImGui::Selectable(s_slots[i].c_str(), sel))
                            {
                                s_selectedSlot = i;
                                // Preenche o campo de nome com o slot selecionado
                                strncpy(s_newSlotName, s_slots[i].c_str(), sizeof(s_newSlotName)-1);
                            }
                        }
                        ImGui::EndListBox();
                    }
                }

                // Campo de nome para salvar/criar
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##slotname", s_newSlotName, sizeof(s_newSlotName));

                // Botões Salvar / Carregar / Deletar
                float bw = (ImGui::GetContentRegionAvail().x - 8.f) / 3.f;

                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.4f, 0.7f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.55f, 0.9f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.3f, 0.65f, 1.0f, 1.f));
                if (ImGui::Button("Salvar##cfg", ImVec2(bw, 0)) && s_newSlotName[0])
                {
                    SaveConfigSlot(s_newSlotName);
                    s_slotsStale = true;
                    std::snprintf(s_configMsg, sizeof(s_configMsg), "Salvo: %s", s_newSlotName);
                    s_configMsgTimer = 2.f;
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.5f, 0.2f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.7f, 0.3f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.2f, 0.9f, 0.4f, 1.f));
                if (ImGui::Button("Carregar##cfg", ImVec2(bw, 0)) && s_newSlotName[0])
                {
                    if (LoadConfigSlot(s_newSlotName))
                    {
                        // Reinicia o AutoDive para que gameMode, enabled e demais
                        // campos do cfg entrem em vigor imediatamente na thread de scan.
                        if (g_mem.IsValid() && g_rbx)
                        {
                            g_dive.Stop();
                            g_dive.Start(g_rbx);
                        }
                        std::snprintf(s_configMsg, sizeof(s_configMsg), "Carregado: %s", s_newSlotName);
                        s_configMsgTimer = 2.f;
                    }
                    else
                    {
                        std::snprintf(s_configMsg, sizeof(s_configMsg), "Nao encontrado: %s", s_newSlotName);
                        s_configMsgTimer = 2.f;
                    }
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.5f, 0.1f, 0.1f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.15f, 0.15f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.9f, 0.2f, 0.2f, 1.f));
                if (ImGui::Button("Deletar##cfg", ImVec2(-1, 0)) && s_selectedSlot >= 0 && s_selectedSlot < (int)s_slots.size())
                {
                    std::string path = GetConfigDir() + "configs\\" + s_slots[s_selectedSlot] + ".ini";
                    DeleteFileA(path.c_str());
                    std::snprintf(s_configMsg, sizeof(s_configMsg), "Deletado: %s", s_slots[s_selectedSlot].c_str());
                    s_configMsgTimer = 2.f;
                    s_selectedSlot = -1;
                    s_slotsStale   = true;
                }
                ImGui::PopStyleColor(3);

                if (s_configMsgTimer > 0.f)
                {
                    s_configMsgTimer -= ImGui::GetIO().DeltaTime;
                    ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "%s", s_configMsg);
                }

                ImGui::Separator();

                // ── Auto-load ─────────────────────────────────────────────
                if (ImGui::Checkbox("Auto-carregar config ao iniciar", &s_autoLoad))
                    SaveMeta(s_autoSlot, s_autoLoad);

                if (s_autoLoad)
                {
                    // Combo que lista todas as configs salvas para escolher qual
                    // será carregada automaticamente ao abrir o programa.
                    ImGui::TextDisabled("Config para auto-load:");
                    const char* comboPreview = (s_autoSlot[0]) ? s_autoSlot : "-- selecione --";
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##autoSlotCombo", comboPreview))
                    {
                        for (const auto& slot : s_slots)
                        {
                            bool isSel = (strcmp(s_autoSlot, slot.c_str()) == 0);
                            if (ImGui::Selectable(slot.c_str(), isSel))
                            {
                                strncpy(s_autoSlot, slot.c_str(), sizeof(s_autoSlot) - 1);
                                s_autoSlot[sizeof(s_autoSlot) - 1] = '\0';
                                SaveMeta(s_autoSlot, s_autoLoad);
                            }
                            if (isSel) ImGui::SetItemDefaultFocus();
                        }
                        if (s_slots.empty())
                            ImGui::TextDisabled("  (nenhuma config salva)");
                        ImGui::EndCombo();
                    }
                }

                ImGui::Separator();

                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.1f, 0.1f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 0.3f, 0.3f, 1.f));
                if (ImGui::Button("Fechar External", ImVec2(-1, 0)))
                    PostQuitMessage(0);
                ImGui::PopStyleColor(3);

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
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

    // Auto-load: lê meta e carrega o slot configurado se autoLoad=1
    {
        char  autoSlot[64] = {};
        bool  autoLoad     = false;
        LoadMeta(autoSlot, sizeof(autoSlot), autoLoad);
        if (autoLoad && autoSlot[0])
            LoadConfigSlot(autoSlot);
    }

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

        if (!wasValid && isValid)
        {
            g_dive.Stop();
            g_dive.Start(g_rbx);
            diveStarted = true;
        }
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

        DrawMenu(overlay);
        renderer.EndFrame();
    }

    g_dive.Stop();
    delete g_rbx;
    renderer.Shutdown();
    g_mem.Detach();
    return 0;
}
