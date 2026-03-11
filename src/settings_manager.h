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

} // namespace SettingsManager
