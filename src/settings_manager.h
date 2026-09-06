// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <functional>
#include <string>
#include <variant>

namespace SettingsManager
{

using SettingValue = std::variant<bool, int, uint32_t, float, std::string>;

void parseArgs(const int argc, const char* const* argv);

bool getAsBool(const std::string& name);
int getAsInt(const std::string& name);
uint32_t getAsUint(const std::string& name);
float getAsFloat(const std::string& name);
const std::string& getAsString(const std::string& name);

void setAsBool(const std::string& name, bool value);
void toggleBool(const std::string& name);
void setAsInt(const std::string& name, int value);
void setAsUint(const std::string& name, uint32_t value);
void setAsFloat(const std::string& name, float value);
void setAsString(const std::string& name, const std::string& value);

uint32_t getWorldSeed();
void setWorldSeed(uint32_t value);

// Visits every setting in unspecified order
void forEachSetting(const std::function<void(const std::string& name, const SettingValue& value)>& callback);

// Golden screenshot run: render, save --testOutput, exit
bool isTestMode();
// Performance measurement run: warm up, measure, write --perfOutput, exit
bool isPerfMode();
// Either automated run: camera locked, GUI hidden, animation paused, vsync off (all as
// overridable defaults), no foreground window, voxel import awaited
bool isHeadless();

} // namespace SettingsManager
