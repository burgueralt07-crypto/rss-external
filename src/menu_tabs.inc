// Implementações das abas da GUI. Incluído por menu_new.inc.

// ── Visuals ──────────────────────────────────────────────────────────────
static void DrawVisualsTab()
{
    ImGui::Text("ESP (Visuals)");
    ImGui::Separator();

    ImGui::Checkbox("Ativar ESP", &g_cfg.enabled);
    if (g_cfg.enabled)
    {
        ImGui::Spacing();
        ImGui::Indent();
        ImGui::Checkbox("Box",      &g_cfg.showBox);
        ImGui::Checkbox("Vida",     &g_cfg.showHealth);
        ImGui::Checkbox("Nome",     &g_cfg.showName);
        ImGui::Checkbox("Distancia",&g_cfg.showDistance);
        ImGui::SliderFloat("Dist. maxima", &g_cfg.maxDistance, 50.f, 2000.f, "%.0f studs");
        ImGui::Unindent();
    }
}

// ── Misc ─────────────────────────────────────────────────────────────────
static void DrawMiscTab(Overlay& overlay)
{
    ImGui::Text("Miscellaneous");
    ImGui::Separator();

    // Streamproof
    if (ImGui::Checkbox("Streamproof", &g_streamproof))
        overlay.SetStreamproof(g_streamproof);
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Oculta o overlay em gravacoes/transmissoes.");

    ImGui::Spacing();

    // Hotkey menu
    static bool s_waitingKey = false;
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Abrir/fechar menu:");
    ImGui::SameLine();
    if (s_waitingKey)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.4f, 0.0f, 1.f));
        ImGui::Button("Aguardando tecla... (Esc=cancelar)", ImVec2(-1, 0));
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
                if (vk == VK_ESCAPE) s_waitingKey = false;
                else { g_menuKey = vk; s_waitingKey = false; }
                break;
            }
        }
    }
    else
    {
        char btnLabel[64];
        std::snprintf(btnLabel, sizeof(btnLabel), "[ %s ]", VKToName(g_menuKey));
        if (ImGui::Button(btnLabel, ImVec2(-1, 0))) s_waitingKey = true;
    }

    ImGui::Separator();

    // Configs
    static char  s_newSlotName[64] = "";
    static int   s_selectedSlot    = -1;
    static bool  s_autoLoad        = false;
    static char  s_autoSlot[64]    = {};
    static bool  s_metaLoaded      = false;
    static char  s_configMsg[128]  = {};
    static float s_configMsgTimer  = 0.f;

    if (!s_metaLoaded) { LoadMeta(s_autoSlot, sizeof(s_autoSlot), s_autoLoad); s_metaLoaded = true; }

    static std::vector<std::string> s_slots;
    if (s_menuSlotsStale) { s_slots = ListConfigSlots(); s_menuSlotsStale = false; }

    ImGui::TextDisabled("Configs salvas:");
    {
        float listH = ImGui::GetTextLineHeightWithSpacing() * 3.5f;
        if (ImGui::BeginListBox("##slots", ImVec2(-1, listH)))
        {
            if (s_slots.empty()) ImGui::TextDisabled("  (nenhuma config salva)");
            for (int i = 0; i < (int)s_slots.size(); ++i)
            {
                bool sel = (s_selectedSlot == i);
                if (ImGui::Selectable(s_slots[i].c_str(), sel))
                {
                    s_selectedSlot = i;
                    strncpy(s_newSlotName, s_slots[i].c_str(), sizeof(s_newSlotName) - 1);
                }
            }
            ImGui::EndListBox();
        }
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##slotname", "nome da config...", s_newSlotName, sizeof(s_newSlotName));

    float bw = (ImGui::GetContentRegionAvail().x - 8.f) / 3.f;

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.4f, 0.7f, 1.f));
    if (ImGui::Button("Salvar##cfg", ImVec2(bw, 0)) && s_newSlotName[0])
    {
        SaveConfigSlot(s_newSlotName);
        s_menuSlotsStale = true;
        std::snprintf(s_configMsg, sizeof(s_configMsg), "Salvo: %s", s_newSlotName);
        s_configMsgTimer = 2.f;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.2f, 1.f));
    if (ImGui::Button("Carregar##cfg", ImVec2(bw, 0)) && s_newSlotName[0])
    {
        if (LoadConfigSlot(s_newSlotName, &overlay))
        {
            if (g_mem.IsValid() && g_rbx) { g_dive.Stop(); g_dive.Start(g_rbx); }
            std::snprintf(s_configMsg, sizeof(s_configMsg), "Carregado: %s", s_newSlotName);
        }
        else
            std::snprintf(s_configMsg, sizeof(s_configMsg), "Nao encontrado: %s", s_newSlotName);
        s_configMsgTimer = 2.f;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 1.f));
    if (ImGui::Button("Deletar##cfg", ImVec2(-1, 0)) && s_selectedSlot >= 0 && s_selectedSlot < (int)s_slots.size())
    {
        std::string path = GetConfigDir() + "configs\\" + s_slots[s_selectedSlot] + ".ini";
        DeleteFileA(path.c_str());
        std::snprintf(s_configMsg, sizeof(s_configMsg), "Deletado: %s", s_slots[s_selectedSlot].c_str());
        s_configMsgTimer = 2.f;
        s_selectedSlot = -1;
        s_menuSlotsStale = true;
    }
    ImGui::PopStyleColor();

    if (s_configMsgTimer > 0.f)
    {
        s_configMsgTimer -= ImGui::GetIO().DeltaTime;
        ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "%s", s_configMsg);
    }

    ImGui::Separator();

    // Auto-load
    if (ImGui::Checkbox("Auto-carregar config ao iniciar", &s_autoLoad))
        SaveMeta(s_autoSlot, s_autoLoad);
    if (s_autoLoad)
    {
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
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    // Offsets
    {
        const OffsetManager& om = OffsetManager::Get();
        ImGui::Text("Versao: %s", om.GetVersion().c_str());
        ImGui::Text("Dumpado em: %s", om.GetDumpedAt().c_str());
        if (ImGui::Button("Atualizar offsets agora", ImVec2(-1, 0)))
        {
            OffsetManager::Get().Fetch();
            if (g_rbx) g_rbx->InvalidateStructure();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Unload / Exit
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.1f, 0.1f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 0.3f, 0.3f, 1.f));
    if (ImGui::Button("Unload / Exit", ImVec2(-1, 30)))
        PostQuitMessage(0);
    ImGui::PopStyleColor(3);
}
