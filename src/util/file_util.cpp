// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "file_util.h"
#include "logger.h"

#include <fstream>
#include <shlobj.h>

namespace FileUtil
{

bool writeAtomically(const std::filesystem::path& path, std::span<const char> bytes)
{
    std::filesystem::path temporaryPath = path;
    temporaryPath += ".tmp";
    std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        Logger::logError("file write: failed to create %s", temporaryPath.generic_string().c_str());
        return false;
    }
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    std::error_code error;
    if (file)
    {
        std::filesystem::rename(temporaryPath, path, error);
        if (!error) return true;
    }
    Logger::logError("file write: failed to publish %s: %s", path.generic_string().c_str(),
                     error ? error.message().c_str() : "write or close failed");
    std::filesystem::remove(temporaryPath, error);
    return false;
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
