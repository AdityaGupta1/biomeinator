// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "file_util.h"

#include <shlobj.h>

namespace FileUtil
{

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
