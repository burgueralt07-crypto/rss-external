#pragma once
#include <imgui.h>
#include <Windows.h>
#include <string>
#include <cstdio>

// ==========================================================================
// Estilo Vape V4 — cores, arredondamentos e espaçamentos idênticos ao
// projeto de referência (rss-external/base). Aplicar uma vez após Init().
// ==========================================================================
inline void ApplyVapeStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    style.WindowRounding    = 8.0f;
    style.FrameRounding     = 4.0f;
    style.ChildRounding     = 6.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding      = 4.0f;
    style.WindowPadding     = ImVec2(12, 12);
    style.FramePadding      = ImVec2(8, 6);
    style.ItemSpacing       = ImVec2(10, 8);
    style.ItemInnerSpacing  = ImVec2(8, 6);
    ImVec4 accent = ImVec4(0.17f, 0.47f, 0.88f, 1.0f);
    ImVec4 bg     = ImVec4(0.08f, 0.08f, 0.08f, 0.96f);
    ImVec4 bg_hov = ImVec4(0.12f, 0.12f, 0.12f, 1.0f);
    ImVec4 text   = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    ImVec4 text_d = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    colors[ImGuiCol_Text]                  = text;
    colors[ImGuiCol_TextDisabled]          = text_d;
    colors[ImGuiCol_WindowBg]              = bg;
    colors[ImGuiCol_ChildBg]               = ImVec4(0.10f, 0.10f, 0.10f, 0.96f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.12f, 0.12f, 0.96f);
    colors[ImGuiCol_Border]                = ImVec4(0.20f, 0.20f, 0.20f, 0.5f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.14f, 0.14f, 0.14f, 1.0f);
    colors[ImGuiCol_FrameBgHovered]        = bg_hov;
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    colors[ImGuiCol_TitleBg]               = bg;
    colors[ImGuiCol_TitleBgActive]         = bg;
    colors[ImGuiCol_TitleBgCollapsed]      = bg;
    colors[ImGuiCol_MenuBarBg]             = bg;
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.30f, 0.30f, 0.30f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.40f, 0.40f, 0.40f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
    colors[ImGuiCol_CheckMark]             = accent;
    colors[ImGuiCol_SliderGrab]            = accent;
    colors[ImGuiCol_SliderGrabActive]      = accent;
    colors[ImGuiCol_Button]                = ImVec4(0.14f, 0.14f, 0.14f, 1.0f);
    colors[ImGuiCol_ButtonHovered]         = bg_hov;
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    colors[ImGuiCol_Header]                = ImVec4(0.14f, 0.14f, 0.14f, 1.0f);
    colors[ImGuiCol_HeaderHovered]         = bg_hov;
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    colors[ImGuiCol_Separator]             = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    colors[ImGuiCol_SeparatorHovered]      = accent;
    colors[ImGuiCol_SeparatorActive]       = accent;
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
    colors[ImGuiCol_TabHovered]            = bg_hov;
    colors[ImGuiCol_TabActive]             = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    colors[ImGuiCol_PlotLines]             = accent;
    colors[ImGuiCol_PlotLinesHovered]      = accent;
    colors[ImGuiCol_PlotHistogram]         = accent;
    colors[ImGuiCol_PlotHistogramHovered]  = accent;
    colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.20f, 0.40f, 0.80f, 0.5f);
    colors[ImGuiCol_DragDropTarget]        = accent;
    colors[ImGuiCol_NavHighlight]          = accent;
}
