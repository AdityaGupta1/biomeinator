// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "file_util.h"
#include "logger.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <shlobj.h>
#include <stdexcept>
#include <unordered_map>

namespace FileUtil
{

namespace
{
std::shared_ptr<std::mutex> getPathMutex(const std::filesystem::path& path)
{
    std::wstring key = std::filesystem::weakly_canonical(path).native();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return std::towlower(c); });

    static std::mutex registryMutex;
    static std::unordered_map<std::wstring, std::weak_ptr<std::mutex>> registry;
    std::scoped_lock registryLock(registryMutex);
    // Retain only active operations, not an entry for every region ever visited.
    std::erase_if(registry, [](const auto& entry) { return entry.second.expired(); });
    auto& entry = registry[key];
    auto mutex = entry.lock();
    if (!mutex)
    {
        mutex = std::make_shared<std::mutex>();
        entry = mutex;
    }
    return mutex;
}
} // namespace

PathLock::PathLock(const std::filesystem::path& path) : mutex(getPathMutex(path)), lock(*mutex)
{}

bool writeAtomically(const std::filesystem::path& path, const std::function<void(std::ostream&)>& write)
{
    PathLock lock(path);
    std::filesystem::path temporaryPath = path;
    temporaryPath += ".tmp";
    bool createdTemporary = false;
    try
    {
        std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            throw std::runtime_error("failed to create temporary file");
        }
        createdTemporary = true;
        write(file);
        file.close();
        if (!file)
        {
            throw std::runtime_error("write or close failed");
        }
        std::filesystem::rename(temporaryPath, path);
        return true;
    }
    catch (const std::exception& error)
    {
        Logger::logError("file write: %s: %s", path.generic_string().c_str(), error.what());
        if (createdTemporary)
        {
            std::error_code cleanupError;
            std::filesystem::remove(temporaryPath, cleanupError);
            if (cleanupError)
            {
                Logger::logError("file cleanup: %s: %s", temporaryPath.generic_string().c_str(),
                                 cleanupError.message().c_str());
            }
        }
        return false;
    }
}

bool writeAtomically(const std::filesystem::path& path, std::span<const char> bytes)
{
    return writeAtomically(path, [bytes](std::ostream& file)
    {
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    });
}

std::filesystem::path getDocumentsDir(const std::string& category)
{
    wchar_t documentsPath[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documentsPath)))
    {
        return {};
    }

    const std::filesystem::path dir =
        std::filesystem::path(documentsPath) / "biomeinator" / category;
    std::filesystem::create_directories(dir);

    return dir;
}

std::string getTimestampString()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[64];
    sprintf_s(buf,
              "%04d.%02d.%02d_%02d-%02d-%02d",
              st.wYear,
              st.wMonth,
              st.wDay,
              st.wHour,
              st.wMinute,
              st.wSecond);

    return buf;
}

} // namespace FileUtil
