#include "overlay.h"
#include "renderer.h"
#include "memory.h"
#include "rbx.h"
#include "rmath.h"

#include <imgui.h>
#include <Windows.h>
#include <string>
#include <cstdio>

// --------------------------------------------------------------------------
// Configuração
// --------------------------------------------------------------------------
static constexpr wchar_t TARGET_WINDOW[]  = L"Roblox";
static constexpr wchar_t TARGET_PROCESS[] = L"RobloxPlayerBeta.exe";

// --------------------------------------------------------------------------
// Globals
// --------------------------------------------------------------------------
static Memory        g_mem;
static RobloxReader* g_rbx      = nullptr;
static bool          g_menuOpen = true;   // HOME toggle

// --------------------------------------------------------------------------
// ESPConfig
// --------------------------------------------------------------------------
struct ESPConfig {
    bool  enabled      = true;
    bool  showBox      = true;
    bool  showHealth   = true;
    bool  showName     = true;
    bool  showDistance = true;
    float maxDistance  = 500.f;
};
static ESPConfig g_cfg;

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------
static ImU32 HealthColor(float hp, float maxHp)
{
    float ratio = (maxHp > 0.f) ? (hp / maxHp) : 0.f;
    int r = static_cast<int>((1.f - ratio) * 255.f);
    int g = static_cast<int>(ratio          * 255.f);
    return IM_COL32(r, g, 0, 255);
}

// --------------------------------------------------------------------------
// PollHotkeys — chama no início de cada frame
// --------------------------------------------------------------------------
static void PollHotkeys()
{
    // GetAsyncKeyState: bit 0x8000 = pressionado agora, bit 0x0001 = pressionado desde última chamada
    // Usamos um flag estático para debounce (só muda ao soltar e pressionar de novo)
    static bool homePrev = false;
    bool homeNow = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    if (homeNow && !homePrev)
        g_menuOpen = !g_menuOpen;
    homePrev = homeNow;
}

// --------------------------------------------------------------------------
// IsRobloxForeground — retorna true se o Roblox está em foco E maximizado
// --------------------------------------------------------------------------
static bool IsRobloxForeground(HWND targetHwnd)
{
    if (!targetHwnd || !IsWindow(targetHwnd)) return false;

    // Verifica se é a janela em foreground
    if (GetForegroundWindow() != targetHwnd) return false;

    // Verifica se está maximizado (ou fullscreen — SHOWMAXIMIZED ou tela cheia sem borda)
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    GetWindowPlacement(targetHwnd, &wp);

    return wp.showCmd == SW_SHOWMAXIMIZED || wp.showCmd == SW_SHOWNORMAL;
}

// --------------------------------------------------------------------------
// DrawESP
// --------------------------------------------------------------------------
static void DrawESP(ImDrawList* dl,
                    const std::vector<PlayerData>& players,
                    const Matrix4x4& viewMatrix,
                    const Vector2&   viewport,
                    const Vector3&   localPos)
{
    if (!g_cfg.enabled || players.empty()) return;

    constexpr float CHAR_HEIGHT = 5.0f;

    for (const auto& p : players)
    {
        Vector3 diff = p.position - localPos;
        float   dist = diff.Length();
        if (dist > g_cfg.maxDistance) continue;

        Vector3 posTop    = { p.position.x, p.position.y + CHAR_HEIGHT, p.position.z };
        Vector3 posBottom = { p.position.x, p.position.y - 0.5f,        p.position.z };

        Vector2 screenCenter, screenTop, screenBottom;
        if (!WorldToScreen(viewMatrix, p.position, viewport, screenCenter)) continue;
        if (!WorldToScreen(viewMatrix, posTop,      viewport, screenTop))    continue;
        if (!WorldToScreen(viewMatrix, posBottom,   viewport, screenBottom)) continue;

        float boxH = screenBottom.y - screenTop.y;
        if (boxH < 5.f) continue;

        float boxW = boxH * 0.45f;
        float x1 = screenCenter.x - boxW * 0.5f;
        float y1 = screenTop.y;
        float x2 = screenCenter.x + boxW * 0.5f;
        float y2 = screenBottom.y;

        // Box
        if (g_cfg.showBox)
        {
            dl->AddRect(ImVec2(x1-1,y1-1), ImVec2(x2+1,y2+1), IM_COL32(0,0,0,200), 0.f, 0, 3.f);
            dl->AddRect(ImVec2(x1,  y1  ), ImVec2(x2,  y2  ), IM_COL32(255,255,255,220), 0.f, 0, 1.5f);
        }

        // Barra de vida
        if (g_cfg.showHealth && p.maxHealth > 0.f)
        {
            float hpRatio = p.health / p.maxHealth;
            float barX    = x1 - 5.f;
            float barFill = y1 + (y2 - y1) * (1.f - hpRatio);
            dl->AddRectFilled(ImVec2(barX-1, y1-1), ImVec2(barX+3, y2+1), IM_COL32(0,0,0,180));
            dl->AddRectFilled(ImVec2(barX,   barFill), ImVec2(barX+2, y2), HealthColor(p.health, p.maxHealth));
        }

        // Nome
        if (g_cfg.showName && !p.name.empty())
        {
            ImVec2 sz = ImGui::CalcTextSize(p.name.c_str());
            dl->AddText(ImVec2(screenCenter.x - sz.x * 0.5f, y1 - sz.y - 2.f),
                        IM_COL32(255,255,255,255), p.name.c_str());
        }

        // Distância
        if (g_cfg.showDistance)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0fm", dist);
            ImVec2 sz = ImGui::CalcTextSize(buf);
            dl->AddText(ImVec2(screenCenter.x - sz.x * 0.5f, y2 + 2.f),
                        IM_COL32(200,200,200,220), buf);
        }
    }
}

// --------------------------------------------------------------------------
// DrawMenu — janela flutuante e arrastável
// --------------------------------------------------------------------------
static void DrawMenu()
{
    if (!g_menuOpen) return;

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoCollapse;

    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);

    if (ImGui::Begin("ESP  [HOME = fechar]", nullptr, flags))
    {
        if (g_mem.IsValid())
            ImGui::TextColored(ImVec4(0,1,0,1), "Conectado  PID: %lu", g_mem.GetPID());
        else
            ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "Aguardando Roblox...");

        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::Separator();

        ImGui::Checkbox("ESP Ativo",   &g_cfg.enabled);
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

        // ---- Painel de debug ----
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Debug"))
        {
            auto hex = [](uintptr_t v) -> const char* {
                static char buf[32];
                std::snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)v);
                return buf;
            };

            auto ok  = [](uintptr_t v) { return v != 0; };

            ImGui::TextColored(ok(g_rbx->GetBase())      ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1),
                "Base:        %s", hex(g_rbx->GetBase()));
            ImGui::TextColored(ok(g_rbx->GetDataModel()) ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1),
                "DataModel:   %s", hex(g_rbx->GetDataModel()));
            ImGui::TextColored(ok(g_rbx->GetWorkspace()) ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1),
                "Workspace:   %s", hex(g_rbx->GetWorkspace()));
            ImGui::TextColored(ok(g_rbx->GetCamera())    ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1),
                "Camera:      %s", hex(g_rbx->GetCamera()));
            ImGui::TextColored(ok(g_rbx->GetPlayers_())  ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1),
                "Players svc: %s", hex(g_rbx->GetPlayers_()));
            ImGui::TextColored(ok(g_rbx->GetLocalPtr())  ? ImVec4(0,1,0,1) : ImVec4(1,0,0,1),
                "LocalPlayer: %s", hex(g_rbx->GetLocalPtr()));

            ImGui::Separator();
            ImGui::Text("Viewport: %.0f x %.0f",
                g_rbx->GetViewport().x, g_rbx->GetViewport().y);
            ImGui::Text("Players encontrados: %zu", g_rbx->GetPlayers().size());

            // Lista jogadores encontrados com posição
            for (const auto& p : g_rbx->GetPlayers())
            {
                ImGui::Text("  [%s] HP:%.0f/%.0f pos:(%.1f,%.1f,%.1f)",
                    p.name.c_str(), p.health, p.maxHealth,
                    p.position.x, p.position.y, p.position.z);
            }
        }

        // ---- Scanner de estrutura ----
        if (ImGui::CollapsingHeader("Scanner (FindChild)") && g_rbx->GetDataModel())
        {
            uintptr_t dm = g_rbx->GetDataModel();
            ImGui::Text("DataModel: 0x%llX", (unsigned long long)dm);
            ImGui::Separator();

            // Dump raw de todos os ponteiros do DataModel (0x00 a 0x200)
            if (ImGui::CollapsingHeader("Raw ptrs DataModel"))
            {
                for (int off = 0x00; off <= 0x200; off += 8)
                {
                    uintptr_t v = 0;
                    g_mem.ReadRaw(dm + off, &v, 8);
                    if (v > 0x10000000000ULL && v < 0x7FFFFFFFFFFFULL)
                        ImGui::Text("+0x%03X = 0x%llX", off, (unsigned long long)v);
                }
            }

            ImGui::Separator();
            ImGui::Text("Procurando vetor begin/end (range 0x00-0x300):");

            // Varre range maior com múltiplos offsets de NameContainer
            static const int nameOffsets[] = { 0x60, 0x68, 0x70, 0x78, 0x80, 0x88 };

            for (int off = 0x00; off <= 0x300; off += 8)
            {
                uintptr_t begin = 0, end = 0;
                g_mem.ReadRaw(dm + off,     &begin, 8);
                g_mem.ReadRaw(dm + off + 8, &end,   8);

                if (!begin || !end || end <= begin) continue;
                size_t cnt = (end - begin) / 8;
                if (cnt == 0 || cnt > 128) continue;
                // ponteiros devem parecer endereços userspace válidos
                if (begin < 0x10000 || begin > 0x7FFFFFFFFFFFULL) continue;

                // Tenta ler nome do primeiro filho com vários offsets
                uintptr_t firstChild = 0;
                g_mem.ReadRaw(begin, &firstChild, 8);
                if (!firstChild || firstChild < 0x10000) continue;

                for (int noff : nameOffsets)
                {
                    uintptr_t nc = 0;
                    g_mem.ReadRaw(firstChild + noff, &nc, 8);
                    if (!nc || nc < 0x10000) continue;

                    size_t slen = 0, scap = 0;
                    // NameContainer + 0x8 = string (Instance::Name = 0x8)
                    uintptr_t strAddr = nc + 0x8;
                    g_mem.ReadRaw(strAddr + 0x10, &slen, 8);
                    g_mem.ReadRaw(strAddr + 0x18, &scap, 8);
                    if (slen == 0 || slen > 32) continue;

                    char nameBuf[64]{};
                    uintptr_t dataPtr = strAddr;
                    if (scap > 15) g_mem.ReadRaw(strAddr, &dataPtr, 8);
                    if (!dataPtr || dataPtr < 0x10000) continue;
                    g_mem.ReadRaw(dataPtr, nameBuf, slen);

                    bool ascii = true;
                    for (size_t k = 0; k < slen; k++)
                        if ((unsigned char)nameBuf[k] < 0x20 || (unsigned char)nameBuf[k] > 0x7e)
                            { ascii = false; break; }
                    if (!ascii) continue;

                    ImGui::TextColored(ImVec4(0,1,1,1),
                        "vec+0x%03X nameOff+0x%02X cnt=%zu [0]='%s'",
                        off, noff, cnt, nameBuf);
                }
            }
        }
    }
    ImGui::End();
}

// --------------------------------------------------------------------------
// TryAttach
// --------------------------------------------------------------------------
static bool TryAttach()
{
    if (g_mem.IsValid()) return true;
    return g_mem.Attach(TARGET_PROCESS);
}

// --------------------------------------------------------------------------
// WinMain
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

    // Permite que o ImGui receba input do mouse quando o menu estiver aberto
    // A janela overlay alterna entre click-through e interativa conforme necessário
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    g_rbx = new RobloxReader(g_mem);
    TryAttach();

    // ---------- Loop principal ----------
    while (overlay.IsRunning())
    {
        if (!overlay.ProcessMessages()) break;

        overlay.SyncWithTarget();
        renderer.Resize(overlay.GetWidth(), overlay.GetHeight());

        TryAttach();

        // Verifica se Roblox está em foco e maximizado
        bool robloxActive = IsRobloxForeground(overlay.GetTargetHWND());

        // Oculta/mostra o overlay conforme o Roblox estar em foco
        overlay.SetVisible(robloxActive);

        if (!robloxActive)
        {
            // Não processa frame — apenas consome mensagens
            Sleep(50);
            continue;
        }

        PollHotkeys();

        // Alterna click-through: com menu aberto o mouse precisa interagir
        overlay.SetClickThrough(!g_menuOpen);

        if (g_mem.IsValid())
            g_rbx->Update();

        renderer.BeginFrame();

        if (g_mem.IsValid() && g_cfg.enabled)
        {
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            DrawESP(dl,
                    g_rbx->GetPlayers(),
                    g_rbx->GetViewMatrix(),
                    g_rbx->GetViewport(),
                    {});
        }

        DrawMenu();

        renderer.EndFrame();
    }

    delete g_rbx;
    renderer.Shutdown();
    g_mem.Detach();
    return 0;
}
