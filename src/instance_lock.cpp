// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "instance_lock.h"
#include "logger.h"

#include <Windows.h>
#include <shlobj.h>

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace InstanceLock
{

namespace
{

std::filesystem::path getLockFilePath()
{
    wchar_t localAppDataPath[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localAppDataPath)))
    {
        return {};
    }

    const std::filesystem::path dir = std::filesystem::path(localAppDataPath) / "biomeinator";
    std::error_code error;
    std::filesystem::create_directories(dir, error);

    return dir / "instance.lock";
}

// A shared instance may run unprotected, but an exclusive one must not
void handleFailure(const bool exclusive, const char* what, const DWORD error)
{
    if (exclusive)
    {
        Logger::logError("Exclusive mode: %s (error %lu)", what, error);
        std::exit(EXIT_FAILURE);
    }

    Logger::logWarning("Instance lock: %s (error %lu); running without it", what, error);
}

} // namespace

void acquire(const bool exclusive)
{
    const std::filesystem::path lockFilePath = getLockFilePath();
    const HANDLE lockFile = CreateFileW(lockFilePath.c_str(),
                                        GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
    if (lockFile == INVALID_HANDLE_VALUE)
    {
        handleFailure(exclusive, "failed to open the lock file", GetLastError());
        return;
    }

    bool loggedWaiting = false;
    const auto logWaiting = [exclusive, &loggedWaiting]()
    {
        if (loggedWaiting)
        {
            return;
        }

        Logger::log(exclusive ? "Exclusive mode: waiting for other Biomeinator instances to exit"
                              : "Waiting for an exclusive Biomeinator instance to exit");
        loggedWaiting = true;
    };

    // Every instance passes through the turnstile before locking, and a waiting exclusive instance keeps
    // holding it, so a steady stream of shared instances cannot starve the exclusive one
    const HANDLE turnstile = CreateMutexW(nullptr, FALSE, L"Local\\BiomeinatorInstanceTurnstile");
    if (!turnstile)
    {
        handleFailure(exclusive, "failed to create the turnstile", GetLastError());
        return;
    }

    // WAIT_ABANDONED means a previous holder exited while queued, which still grants ownership
    DWORD waitResult = WaitForSingleObject(turnstile, 0);
    if (waitResult == WAIT_TIMEOUT)
    {
        logWaiting();
        waitResult = WaitForSingleObject(turnstile, INFINITE);
    }
    if (waitResult == WAIT_FAILED)
    {
        handleFailure(exclusive, "failed to wait for the turnstile", GetLastError());
        return;
    }

    const DWORD lockFlags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    OVERLAPPED overlapped{};
    bool locked = LockFileEx(lockFile, lockFlags | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped);
    if (!locked && GetLastError() == ERROR_LOCK_VIOLATION)
    {
        logWaiting();
        overlapped = {};
        locked = LockFileEx(lockFile, lockFlags, 0, 1, 0, &overlapped);
    }
    const DWORD lockError = GetLastError();

    ReleaseMutex(turnstile);

    if (!locked)
    {
        handleFailure(exclusive, "failed to lock the lock file", lockError);
    }
}

} // namespace InstanceLock
