// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <filesystem>
#include <span>
#include <string>

namespace FileUtil
{

std::filesystem::path getDocumentsDir(const std::string& category);
std::string getTimestampString();

// One writer per destination. Readers see either the previous file or a completed
// replacement; a failed write leaves the previous file intact.
bool writeAtomically(const std::filesystem::path& path, std::span<const char> bytes);

} // namespace FileUtil
