/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

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

inline void Checkbox(const char* label, const char* settingName)
{
    bool value = SettingsManager::getAsBool(settingName);
    if (ImGui::Checkbox(label, &value))
    {
        SettingsManager::setAsBool(settingName, value);
    }
}

inline void InputInt(const char* label, const char* settingName, int minVal, int maxVal, int step = 1)
{
    ScopedItemWidth width(inputWidth);

    int value = SettingsManager::getAsInt(settingName);
    if (ImGui::InputInt(label, &value, step))
    {
        value = std::clamp(value, minVal, maxVal);
        SettingsManager::setAsInt(settingName, value);
    }
}

inline void SliderInt(const char* label, const char* settingName, int minVal, int maxVal)
{
    ScopedItemWidth width(sliderWidth);

    int value = SettingsManager::getAsInt(settingName);
    if (ImGui::SliderInt(label, &value, minVal, maxVal))
    {
        value = std::clamp(value, minVal, maxVal);
        SettingsManager::setAsInt(settingName, value);
    }
}

inline void InputUint(const char* label, const char* settingName, int minVal, int maxVal, int step = 1)
{
    ScopedItemWidth width(inputWidth);

    int value = static_cast<int>(SettingsManager::getAsUint(settingName));
    if (ImGui::InputInt(label, &value, step))
    {
        value = std::clamp(std::max(value, 0), minVal, maxVal);
        SettingsManager::setAsUint(settingName, static_cast<uint32_t>(value));
    }
}

inline void SliderUint(const char* label, const char* settingName, int minVal, int maxVal)
{
    ScopedItemWidth width(sliderWidth);

    int value = static_cast<int>(SettingsManager::getAsUint(settingName));
    if (ImGui::SliderInt(label, &value, minVal, maxVal))
    {
        value = std::clamp(std::max(value, 0), minVal, maxVal);
        SettingsManager::setAsUint(settingName, static_cast<uint32_t>(value));
    }
}

inline void ComboUint(const char* label, const char* settingName, const std::vector<const char*>& items)
{
    ScopedItemWidth width(comboWidth);

    int value = static_cast<int>(SettingsManager::getAsUint(settingName));
    ASSERT(value < items.size());
    if (ImGui::Combo(label, &value, items.data(), static_cast<int>(items.size())))
    {
        SettingsManager::setAsUint(settingName, static_cast<uint32_t>(value));
    }
}

inline void ComboString(const char* label, const char* settingName, const std::vector<const char*>& items)
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

    if (ImGui::Combo(label, &value, items.data(), static_cast<int>(items.size())))
    {
        SettingsManager::setAsString(settingName, items[value]);
    }
}

}
