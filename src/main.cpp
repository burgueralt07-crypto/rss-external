#include "overlay.h"
#include "renderer.h"
#include "memory.h"
#include "rbx.h"
#include "rmath.h"
#include "autodive.h"
#include "offsets.h"


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
static int           g_diveKey  = 0; // 0 = sem keybind

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
static bool      g_streamproof = true;
static int           g_activeTab = 0;

// Sinaliza re-scan da lista de configs ao reabrir o menu
static bool s_menuSlotsStale = true;

// --------------------------------------------------------------------------
static ImU32 HealthColor(float hp, float maxHp)
{
    float ratio = (maxHp > 0.f) ? (hp / maxHp) : 0.f;
    int r = static_cast<int>((1.f - ratio) * 255.f);
    int g = static_cast<int>(ratio          * 255.f);
    return IM_COL32(r, g, 0, 255);
}

// ==========================================================================
// Toast — notificações no canto inferior direito
// ==========================================================================
struct Toast {
    std::string text;
    float       ttl;       // segundos restantes
    ImVec4      color;
};
static std::vector<Toast> g_toasts;

static void PushToast(const std::string& text,
                      float duration = 2.5f,
                      ImVec4 color = { 0.3f, 1.f, 0.3f, 1.f })
{
    g_toasts.push_back({ text, duration, color });
}

static void DrawToasts()
{
    if (g_toasts.empty()) return;

    const float pad    = 12.f;
    const float dt     = ImGui::GetIO().DeltaTime;
    ImVec2      screen = ImGui::GetIO().DisplaySize;

    // Calcula altura total para empilhar de baixo para cima
    float totalH = 0.f;
    for (auto& t : g_toasts)
        totalH += ImGui::GetTextLineHeightWithSpacing() + pad * 2.f + 4.f;

    float y = screen.y - pad - totalH;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    for (auto it = g_toasts.begin(); it != g_toasts.end(); )
    {
        it->ttl -= dt;
        if (it->ttl <= 0.f) { it = g_toasts.erase(it); continue; }

        float alpha = 1.f;
        if (it->ttl < 0.4f) alpha = it->ttl / 0.4f;   // fade-out suave

        ImVec2 sz  = ImGui::CalcTextSize(it->text.c_str());
        float  w   = sz.x + pad * 2.f;
        float  h   = sz.y + pad * 2.f;
        float  x   = screen.x - w - pad;

        // Fundo arredondado semi-transparente
        ImU32 bg  = IM_COL32(20, 20, 20, (int)(200 * alpha));
        ImU32 bdr = IM_COL32(60, 60, 60, (int)(255 * alpha));
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), bg, 6.f);
        dl->AddRect      (ImVec2(x, y), ImVec2(x + w, y + h), bdr, 6.f);

        ImU32 col = IM_COL32(
            (int)(it->color.x * 255.f),
            (int)(it->color.y * 255.f),
            (int)(it->color.z * 255.f),
            (int)(255 * alpha));
        dl->AddText(ImVec2(x + pad, y + pad), col, it->text.c_str());

        y += h + 4.f;
        ++it;
    }
}

// --------------------------------------------------------------------------
static void PollHotkeys()
{
    // ── Menu toggle ───────────────────────────────────────────────────────
    static int  prevMenuKey  = -1;
    static bool prevMenuDown = false;

    bool menuDown = (GetAsyncKeyState(g_menuKey) & 0x8000) != 0;
    if (menuDown && !prevMenuDown)
        g_menuOpen = !g_menuOpen;

    if (g_menuKey != prevMenuKey) { prevMenuDown = false; prevMenuKey = g_menuKey; }
    else                           prevMenuDown = menuDown;

    // ── AutoDive toggle ───────────────────────────────────────────────────
    if (g_diveKey != 0)
    {
        static int  prevDiveKey  = -1;
        static bool prevDiveDown = false;

        bool diveDown = (GetAsyncKeyState(g_diveKey) & 0x8000) != 0;
        if (diveDown && !prevDiveDown)
        {
            g_dive.cfg.enabled = !g_dive.cfg.enabled;
            if (g_dive.cfg.enabled)
                PushToast("Auto Dive: LIGADO",  2.f, { 0.3f, 1.f, 0.3f, 1.f });
            else
                PushToast("Auto Dive: DESLIGADO", 2.f, { 1.f, 0.4f, 0.4f, 1.f });
        }

        if (g_diveKey != prevDiveKey) { prevDiveDown = false; prevDiveKey = g_diveKey; }
        else                           prevDiveDown = diveDown;
    }
}

// --------------------------------------------------------------------------
static bool IsRobloxForeground(HWND targetHwnd, HWND overlayHwnd)
{
    if (!targetHwnd || !IsWindow(targetHwnd)) return false;

    HWND fg = GetForegroundWindow();
    // Aceita também o próprio overlay em foreground (menu aberto com foco)
    if (fg != targetHwnd && fg != overlayHwnd) return false;

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

    // Cache do offset client→screen — recalcula só quando o HWND muda ou
    // quando SyncWithTarget detectou movimento (posição da janela alvo mudou).
    // ClientToScreen é uma syscall Win32; evitar a cada frame reduz overhead.
    static HWND  s_cachedHwnd  = nullptr;
    static float s_offsetX     = 0.f;
    static float s_offsetY     = 0.f;
    static int   s_lastX       = INT_MIN;
    static int   s_lastY       = INT_MIN;

    if (targetHwnd)
    {
        RECT wr{};
        GetWindowRect(targetHwnd, &wr);
        if (targetHwnd != s_cachedHwnd || wr.left != s_lastX || wr.top != s_lastY)
        {
            POINT pt{ 0, 0 };
            ClientToScreen(targetHwnd, &pt);
            s_offsetX     = static_cast<float>(pt.x);
            s_offsetY     = static_cast<float>(pt.y);
            s_cachedHwnd  = targetHwnd;
            s_lastX       = wr.left;
            s_lastY       = wr.top;
        }
    }
    else
    {
        s_cachedHwnd = nullptr;
        s_offsetX    = 0.f;
        s_offsetY    = 0.f;
    }

    const float winOffsetX = s_offsetX;
    const float winOffsetY = s_offsetY;

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
    // Cacheia o resultado — GetModuleFileNameA só precisa ser chamado uma vez.
    static std::string s_dir;
    if (!s_dir.empty()) return s_dir;

    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    char* last = strrchr(buf, '\\');
    if (last) *(last + 1) = '\0';
    s_dir = buf;
    return s_dir;
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
    fprintf(f, "ad_jumpMinCrossY=%.2f\n",     c.jumpMinCrossY);
    fprintf(f, "ad_velAtCrossYMin=%.2f\n",   c.velAtCrossYMin);
    fprintf(f, "ad_measuredAccelYMin=%.2f\n",c.measuredAccelYMin);
    fprintf(f, "ad_simSteps=%d\n",             c.simSteps);
    fprintf(f, "ad_simDt=%.4f\n",              c.simDt);
    fprintf(f, "ad_gravity=%.4f\n",            c.gravity);
    fprintf(f, "ad_magnusCoeff=%.4f\n",        c.magnusCoeff);
    fprintf(f, "ad_dragCoeff=%.4f\n",          c.dragCoeff);
    fprintf(f, "ad_curveDecayRate=%.4f\n",     c.curveDecayRate);
    fprintf(f, "ad_emaAlpha=%.4f\n",           c.emaAlpha);
    fprintf(f, "ad_watchRange=%.2f\n",         c.watchRange);
    fprintf(f, "ad_diveFireDistance=%.2f\n",   c.diveFireDistance);
    fprintf(f, "ad_scanRate=%d\n",             c.scanRate);
    fprintf(f, "ad_keyHoldMs=%d\n",            c.keyHoldMs);

    fprintf(f, "\n[Misc]\n");
    fprintf(f, "misc_menuKey=%d\n",     g_menuKey);
    fprintf(f, "misc_diveKey=%d\n",     g_diveKey);
    fprintf(f, "misc_streamproof=%d\n", g_streamproof ? 1 : 0);
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
        FLOAT_KEY("ad_jumpMinCrossY",      c.jumpMinCrossY)
        FLOAT_KEY("ad_velAtCrossYMin",     c.velAtCrossYMin)
        FLOAT_KEY("ad_measuredAccelYMin",  c.measuredAccelYMin)
        INT_KEY("ad_simSteps",             c.simSteps)
        FLOAT_KEY("ad_simDt",              c.simDt)
        FLOAT_KEY("ad_gravity",            c.gravity)
        FLOAT_KEY("ad_magnusCoeff",        c.magnusCoeff)
        FLOAT_KEY("ad_dragCoeff",          c.dragCoeff)
        FLOAT_KEY("ad_curveDecayRate",     c.curveDecayRate)
        FLOAT_KEY("ad_emaAlpha",           c.emaAlpha)
        FLOAT_KEY("ad_watchRange",         c.watchRange)
        FLOAT_KEY("ad_diveFireDistance",   c.diveFireDistance)
        INT_KEY("ad_scanRate",             c.scanRate)
        INT_KEY("ad_keyHoldMs",            c.keyHoldMs)
        INT_KEY("misc_menuKey",            g_menuKey)
        INT_KEY("misc_diveKey",            g_diveKey)
        BOOL_KEY("misc_streamproof",       g_streamproof)
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
static bool LoadConfigSlot(const char* name, Overlay* overlay = nullptr)
{
    std::string path = GetConfigDir() + "configs\\" + name + std::string(".ini");
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    ReadConfigEntries(f);
    fclose(f);
    // Aplica streamproof imediatamente se a overlay foi passada
    if (overlay)
        overlay->SetStreamproof(g_streamproof);
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
    switch (vk)
    {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_UP: case VK_DOWN:
        case VK_LEFT: case VK_RIGHT: case VK_DIVIDE: case VK_NUMLOCK:
            sc |= 0x100;
            break;
    }
    static wchar_t wbuf[64];
    if (GetKeyNameTextW(static_cast<LONG>(sc << 16), wbuf, sizeof(wbuf) / sizeof(wchar_t)) > 0)
    {
        static char utf8Buf[128];
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, utf8Buf, sizeof(utf8Buf), nullptr, nullptr);
        return utf8Buf;
    }
    return "?";
}

// GUI estilo Vape V4 — incluida apos as declaracoes que ela referencia.
#include "menu_style.h"
#include "menu_tabs.inc"
#include "menu_tabs2.inc"
#include "menu_new.inc"

// ==========================================================================
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

    // Aplica streamproof imediatamente (padrão ligado)
    overlay.SetStreamproof(g_streamproof);

    ApplyVapeStyle();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    g_rbx = new RobloxReader(g_mem);
    TryAttach();

    // Baixa offsets dinamicos (HTTP -> offsets.json -> fallback hardcoded).
    // Roda ANTES do primeiro Update() para Offsets::* estarem prontos.
    OffsetManager::Get().Fetch();

    // Auto-load: lê meta e carrega o slot configurado se autoLoad=1
    {
        char  autoSlot[64] = {};
        bool  autoLoad     = false;
        LoadMeta(autoSlot, sizeof(autoSlot), autoLoad);
        if (autoLoad && autoSlot[0])
            LoadConfigSlot(autoSlot, &overlay);
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
            // Recuperou conexao -> re-resolve ponteiros (DataModel pode ter mudado)
            if (g_rbx) g_rbx->InvalidateStructure();
            g_dive.Start(g_rbx);
            diveStarted = true;
        }
        else if (wasValid && !isValid)
        {
            g_dive.Stop();
            diveStarted = false;
        }

        bool robloxActive = IsRobloxForeground(overlay.GetTargetHWND(), overlay.GetHWND());
        overlay.SetVisible(robloxActive);

        if (!robloxActive) { Sleep(50); continue; }

        PollHotkeys();
        overlay.SetClickThrough(!g_menuOpen);
        overlay.SetFocused(g_menuOpen);

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
        DrawToasts();
        renderer.EndFrame();
    }

    g_dive.Stop();
    delete g_rbx;
    renderer.Shutdown();
    g_mem.Detach();
    return 0;
}
