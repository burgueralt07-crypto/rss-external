// Nova GUI estilo Vape V4 — inclusa em main.cpp no lugar da antiga DrawMenu.

static void DrawMenu(Overlay& overlay)
{
    static bool s_wasMenuOpen = false;
    if (g_menuOpen && !s_wasMenuOpen) s_menuSlotsStale = true;
    s_wasMenuOpen = g_menuOpen;

    if (!g_menuOpen) return;

    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(700, 450), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.95f);

    if (ImGui::Begin("Vape V4", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize))
    {
        // ── Header: título | status | versão | FPS ────────────────────────
        ImGui::TextColored(ImVec4(0.17f, 0.47f, 0.88f, 1.0f), "Vape V4");
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        if (g_mem.IsValid())
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Conectado  PID: %lu", g_mem.GetPID());
        else
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Aguardando Roblox...");

        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        {
            const OffsetManager& om = OffsetManager::Get();
            const std::string& st = om.GetStatus();
            const std::string& vr = om.GetVersion();
            ImVec4 col = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
            if (st.rfind("FALLBACK", 0) == 0) col = ImVec4(1.f, 0.7f, 0.2f, 1.f);
            else if (st == "NONE" || st.rfind("FAIL", 0) == 0) col = ImVec4(1.f, 0.4f, 0.4f, 1.f);
            ImGui::TextColored(col, "Offsets: %s", vr.c_str());
        }

        ImGui::SameLine(ImGui::GetWindowWidth() - 120);
        ImGui::TextDisabled("FPS: %.0f", ImGui::GetIO().Framerate);
        ImGui::Separator();

        // ── Sidebar ──────────────────────────────────────────────────────
        ImGui::BeginChild("Sidebar", ImVec2(180.0f, 0), true);
        {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));

            const char* tabNames[] = { "Visuals", "Auto Dive", "Misc" };
            for (int i = 0; i < 3; ++i) {
                if (g_activeTab == i) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.17f, 0.47f, 0.88f, 0.2f));
                    ImGui::PushStyleColor(ImGuiCol_Text,   ImVec4(0.17f, 0.47f, 0.88f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text,   ImVec4(0.90f, 0.90f, 0.90f, 1.0f));
                }
                if (ImGui::Button(tabNames[i], ImVec2(-1, 35))) g_activeTab = i;
                ImGui::PopStyleColor(2);
                ImGui::Spacing();
            }

            ImGui::PopStyleVar(2);
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // ── Content ──────────────────────────────────────────────────────
        ImGui::BeginChild("Content", ImVec2(0, 0), true);
        {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10);

            if (g_activeTab == 0)      DrawVisualsTab();
            else if (g_activeTab == 1) DrawAutoDiveTab();
            else                       DrawMiscTab(overlay);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}
