// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <filesystem>
#include <string>

namespace FileUtil
{

std::filesystem::path getDocumentsDir(const std::string& category);
std::string getTimestampString();

} // namespace FileUtil
