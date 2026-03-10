#include <gui/widgets/volume_meter.h>
#include <algorithm>
#include <gui/gui.h>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

namespace ImGui {
    void VolumeMeter(float avg, float peak, float val_min, float val_max, const ImVec2& size_arg) {
        ImGuiWindow* window = GetCurrentWindow();
        ImGuiStyle& style = GImGui->Style;

        avg = std::clamp<float>(avg, val_min, val_max);
        peak = std::clamp<float>(peak, val_min, val_max);

        ImVec2 min = window->DC.CursorPos;
        ImVec2 size = CalcItemSize(size_arg, CalcItemWidth(), (GImGui->FontSize / 2) + style.FramePadding.y);
        ImRect bb(min, min + size);

        float lineHeight = size.y;

        ItemSize(size, style.FramePadding.y);
        if (!ItemAdd(bb, 0)) {
            return;
        }

        float zeroDb = roundf(((-val_min) / (val_max - val_min)) * size.x);

        ImU32 meterCol = ImGui::ColorConvertFloat4ToU32(gui::themeManager.volMeterColor);
        ImU32 clipCol = ImGui::ColorConvertFloat4ToU32(gui::themeManager.volMeterClipColor);
        ImVec4 meterDim = gui::themeManager.volMeterColor; meterDim.w = 0.5f;
        ImVec4 clipDim = gui::themeManager.volMeterClipColor; clipDim.w = 0.5f;
        ImU32 meterBg = ImGui::ColorConvertFloat4ToU32(meterDim);
        ImU32 clipBg = ImGui::ColorConvertFloat4ToU32(clipDim);
        ImVec4 peakNorm = gui::themeManager.volMeterColor; peakNorm.w = 0.75f;
        ImVec4 peakClip = gui::themeManager.volMeterClipColor; peakClip.w = 0.75f;

        window->DrawList->AddRectFilled(min, min + ImVec2(zeroDb, lineHeight), meterBg);
        window->DrawList->AddRectFilled(min + ImVec2(zeroDb, 0), min + ImVec2(size.x, lineHeight), clipBg);

        float end = roundf(((avg - val_min) / (val_max - val_min)) * size.x);
        float endP = roundf(((peak - val_min) / (val_max - val_min)) * size.x);

        if (avg <= 0) {
            window->DrawList->AddRectFilled(min, min + ImVec2(end, lineHeight), meterCol);
        }
        else {
            window->DrawList->AddRectFilled(min, min + ImVec2(zeroDb, lineHeight), meterCol);
            window->DrawList->AddRectFilled(min + ImVec2(zeroDb, 0), min + ImVec2(end, lineHeight), clipCol);
        }

        if (peak <= 0) {
            window->DrawList->AddLine(min + ImVec2(endP, -1), min + ImVec2(endP, lineHeight - 1), ImGui::ColorConvertFloat4ToU32(peakNorm));
        }
        else {
            window->DrawList->AddLine(min + ImVec2(endP, -1), min + ImVec2(endP, lineHeight - 1), ImGui::ColorConvertFloat4ToU32(peakClip));
        }
    }
}