#pragma once

#include <string>

namespace SettingsManager
{

void parseArgs(const int argc, const char* const* argv);

bool getAsBool(const std::string& name);
int getAsInt(const std::string& name);
uint32_t getAsUint(const std::string& name);
std::string getAsString(const std::string& name);

} // namespace SettingsManager
