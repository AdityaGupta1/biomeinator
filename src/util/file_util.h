// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace FileUtil
{

std::filesystem::path getDocumentsDir(const std::string& category);
std::string getTimestampString();

// Coordinates this process's reads and replacements of the same normalized path.
class PathLock
{
    std::shared_ptr<std::mutex> mutex;
    std::unique_lock<std::mutex> lock;

public:
    explicit PathLock(const std::filesystem::path& path);
};

// Holds PathLock until publication or cleanup. The callback streams to a temporary
// file; exceptions and write/close failures leave the previous destination intact.
bool writeAtomically(const std::filesystem::path& path, const std::function<void(std::ostream&)>& write);
bool writeAtomically(const std::filesystem::path& path, std::span<const char> bytes);

} // namespace FileUtil
