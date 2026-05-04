// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <string>

namespace SettingsManager
{

void parseArgs(const int argc, const char* const* argv);

bool getAsBool(const std::string& name);
int getAsInt(const std::string& name);
uint32_t getAsUint(const std::string& name);
float getAsFloat(const std::string& name);
std::string getAsString(const std::string& name);

void setAsBool(const std::string& name, bool value);
void toggleBool(const std::string& name);
void setAsInt(const std::string& name, int value);
void setAsUint(const std::string& name, uint32_t value);
void setAsFloat(const std::string& name, float value);
void setAsString(const std::string& name, const std::string& value);

uint32_t getWorldSeed();
void setWorldSeed(uint32_t value);

bool isTestMode();

} // namespace SettingsManager
