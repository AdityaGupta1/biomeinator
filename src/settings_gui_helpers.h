// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "settings_manager.h"

#include <imgui.h>

#include <algorithm>
#include <vector>

#include "debug.h"

namespace SettingsGuiHelpers
{

constexpr float inputWidth = 120.f;
constexpr float sliderWidth = 200.f;
constexpr float comboWidth = 200.f;

struct ScopedItemWidth
{
    ScopedItemWidth(float width)
    {
        ImGui::PushItemWidth(width);
    }

    ~ScopedItemWidth()
    {
        ImGui::PopItemWidth();
    }
};

inline bool Checkbox(const char* label, const char* settingName)
{
    bool value = SettingsManager::getAsBool(settingName);
    const bool didChange = ImGui::Checkbox(label, &value);
    if (didChange)
    {
        SettingsManager::setAsBool(settingName, value);
    }
    return didChange;
}

inline bool InputInt(const char* label, const char* settingName, int minVal, int maxVal, int step = 1)
{
    ScopedItemWidth width(inputWidth);

    int value = SettingsManager::getAsInt(settingName);
    const bool didChange = ImGui::InputInt(label, &value, step);
    if (didChange)
    {
        value = std::clamp(value, minVal, maxVal);
        SettingsManager::setAsInt(settingName, value);
    }
    return didChange;
}

inline bool SliderInt(const char* label, const char* settingName, int minVal, int maxVal)
{
    ScopedItemWidth width(sliderWidth);

    int value = SettingsManager::getAsInt(settingName);
    const bool didChange = ImGui::SliderInt(label, &value, minVal, maxVal);
    if (didChange)
    {
        value = std::clamp(value, minVal, maxVal);
        SettingsManager::setAsInt(settingName, value);
    }
    return didChange;
}

inline bool InputUint(const char* label, const char* settingName, int minVal, int maxVal, int step = 1)
{
    ScopedItemWidth width(inputWidth);

    int value = static_cast<int>(SettingsManager::getAsUint(settingName));
    const bool didChange = ImGui::InputInt(label, &value, step);
    if (didChange)
    {
        value = std::clamp(std::max(value, 0), minVal, maxVal);
        SettingsManager::setAsUint(settingName, static_cast<uint32_t>(value));
    }
    return didChange;
}

inline bool SliderUint(const char* label, const char* settingName, int minVal, int maxVal)
{
    ScopedItemWidth width(sliderWidth);

    int value = static_cast<int>(SettingsManager::getAsUint(settingName));
    const bool didChange = ImGui::SliderInt(label, &value, minVal, maxVal);
    if (didChange)
    {
        value = std::clamp(std::max(value, 0), minVal, maxVal);
        SettingsManager::setAsUint(settingName, static_cast<uint32_t>(value));
    }
    return didChange;
}

inline bool ComboUint(const char* label, const char* settingName, const std::vector<const char*>& items)
{
    ScopedItemWidth width(comboWidth);

    int value = static_cast<int>(SettingsManager::getAsUint(settingName));
    ASSERT(value < items.size());
    const bool didChange = ImGui::Combo(label, &value, items.data(), static_cast<int>(items.size()));
    if (didChange)
    {
        SettingsManager::setAsUint(settingName, static_cast<uint32_t>(value));
    }
    return didChange;
}

inline bool SliderFloat(const char* label, const char* settingName, float minVal, float maxVal)
{
    ScopedItemWidth width(sliderWidth);

    float value = SettingsManager::getAsFloat(settingName);
    const bool didChange = ImGui::SliderFloat(label, &value, minVal, maxVal);
    if (didChange)
    {
        value = std::clamp(value, minVal, maxVal);
        SettingsManager::setAsFloat(settingName, value);
    }
    return didChange;
}

inline bool ComboString(const char* label, const char* settingName, const std::vector<const char*>& items)
{
    ScopedItemWidth width(comboWidth);

    const std::string& currentSettingValue = SettingsManager::getAsString(settingName);
    int value = 0;
    for (uint32_t itemIdx = 0; itemIdx < items.size(); ++itemIdx)
    {
        if (currentSettingValue == items[itemIdx])
        {
            value = static_cast<int>(itemIdx);
            break;
        }
    }

    const bool didChange = ImGui::Combo(label, &value, items.data(), static_cast<int>(items.size()));
    if (didChange)
    {
        SettingsManager::setAsString(settingName, items[value]);
    }
    return didChange;
}

inline void SectionTitle(const char* settingName)
{
    ImGui::Text(settingName);
    ImGui::Separator();
}

inline void VerticalSpacing()
{
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
}

inline void Tooltip(const char* tooltip)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(400.f);
        ImGui::TextUnformatted(tooltip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

}
