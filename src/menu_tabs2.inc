// Aba Auto Dive — todos os controles que antes estavam em 'Realistic Street Soccer'.

static void DrawAutoDiveTab()
{
    ImGui::Text("Auto Dive (GK)");
    ImGui::Separator();

    ImGui::Checkbox("Ativar Auto Dive", &g_dive.cfg.enabled);

    // Keybind toggle
    {
        static bool s_waitingDiveKey = false;
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Toggle:");
        ImGui::SameLine();

        if (s_waitingDiveKey)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.4f, 0.0f, 1.f));
            ImGui::Button("Aguardando... (Esc=cancelar, Del=remover)", ImVec2(-1, 0));
            ImGui::PopStyleColor();

            for (int vk = 1; vk < 256; ++vk)
            {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
                if (vk == VK_LSHIFT || vk == VK_RSHIFT) continue;
                if (vk == VK_LCONTROL || vk == VK_RCONTROL) continue;
                if (vk == VK_LMENU || vk == VK_RMENU) continue;
                if (GetAsyncKeyState(vk) & 0x8000)
                {
                    if (vk == VK_ESCAPE) s_waitingDiveKey = false;
                    else if (vk == VK_DELETE) { g_diveKey = 0; s_waitingDiveKey = false; }
                    else { g_diveKey = vk; s_waitingDiveKey = false; }
                    break;
                }
            }
        }
        else
        {
            char diveLabel[64];
            if (g_diveKey == 0) std::snprintf(diveLabel, sizeof(diveLabel), "[ Nenhuma ]");
            else               std::snprintf(diveLabel, sizeof(diveLabel), "[ %s ]", VKToName(g_diveKey));
            if (ImGui::Button(diveLabel, ImVec2(-1, 0))) s_waitingDiveKey = true;
        }
    }

    if (!g_dive.cfg.enabled) return;

    ImGui::Separator();
    ImGui::Checkbox("Forcar GK (Ignorar pasta Bools)", &g_dive.cfg.forceGK);
    ImGui::Checkbox("Apenas No Alvo (Gol)", &g_dive.cfg.onlyInGoal);

    static const char* kModeNames[] = { "4v4  (gol pequeno)", "7v7  (gol grande)" };
    int modeIdx = static_cast<int>(g_dive.cfg.gameMode);
    if (ImGui::Combo("Modo", &modeIdx, kModeNames, 2))
        g_dive.cfg.gameMode = static_cast<GameMode>(modeIdx);

    ImGui::Separator();
    ImGui::SliderFloat("Vel minima", &g_dive.cfg.minBallSpeed, 0.f, 50.f, "%.0f studs/s");
    ImGui::SliderFloat("Cooldown",   &g_dive.cfg.cooldownSec, 0.3f, 3.f, "%.1f s");
    ImGui::SliderFloat("Margem gol", &g_dive.cfg.goalMargin, 0.f, 8.f, "%.0f studs");
    ImGui::SliderFloat("Dist disparo",&g_dive.cfg.diveFireDistance, 3.f, 40.f, "%.0f studs");
    ImGui::SliderInt  ("Hold tecla", &g_dive.cfg.keyHoldMs, 20, 200, "%d ms");

    if (g_dive.cfg.gameMode == GameMode::Mode4v4)
    {
        ImGui::Separator();
        ImGui::TextDisabled("-- 4v4 --");
        ImGui::Checkbox("Pular em Bola Alta (Space)", &g_dive.cfg.highJump);
        ImGui::SliderFloat("relX dive [4v4]", &g_dive.cfg.diveXThreshold, 0.5f, 8.f, "%.1f");
        if (g_dive.cfg.highJump)
        {
            ImGui::SliderFloat("relY jump [4v4]", &g_dive.cfg.jumpYThreshold, 2.f, 12.f, "%.1f");
            ImGui::SliderFloat("|relX| max jump [4v4]", &g_dive.cfg.jumpXMaxForPure, 1.f, 10.f, "%.1f");
        }
    }
    else
    {
        ImGui::Separator();
        ImGui::TextDisabled("-- 7v7 --");
        ImGui::Checkbox("Pular em Bola Alta (Space) [7v7]", &g_dive.cfg.highJump);
        ImGui::SliderFloat("relX dive [7v7]",         &g_dive.cfg.diveXThreshold7v7, 1.f, 12.f, "%.1f");
        ImGui::SliderFloat("|relX| Jump puro [7v7]",  &g_dive.cfg.jumpPureXMax7v7,   0.f,  6.f, "%.1f");
        ImGui::SliderFloat("|relX| min J+D [7v7]",    &g_dive.cfg.jumpDiveXMin7v7,   0.f,  6.f, "%.1f");
        ImGui::SliderInt  ("Delay Space->Q/E (ms)",   &g_dive.cfg.jumpDiveDelayMs,   0,   400);
        ImGui::SliderFloat("Janela antecip. [7v7]",    &g_dive.cfg.jumpDiveTimeWindow,0.f, 1.5f, "%.2f s");
        ImGui::SliderFloat("crossY min p/ alto [7v7]", &g_dive.cfg.jumpMinCrossY,    -4.f,  8.f, "%.1f");
        ImGui::SliderFloat("velY cruzamento",          &g_dive.cfg.velAtCrossYMin,     0.f, 30.f, "%.1f  (0=off)");
        ImGui::SliderFloat("accelY medida",            &g_dive.cfg.measuredAccelYMin, 0.f, 40.f, "%.1f  (0=off)");
    }

    if (g_rbx)
    {
        const GKState&   gk   = g_rbx->GetGKState();
        const GoalState& goal = g_rbx->GetGoal();
        ImGui::Separator();
        if (gk.isGK) ImGui::TextColored(ImVec4(0.2f,1.f,0.2f,1.f), "GK: ATIVO");
        else         ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f), "GK: nao detectado");
        if (goal.exists) ImGui::TextColored(ImVec4(0.2f,1.f,0.2f,1.f), "Gol: encontrado");
        else             ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f), "Gol: NAO encontrado");
    }
}
