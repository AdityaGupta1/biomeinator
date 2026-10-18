// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "instance_lock.h"
#include "logger.h"

#include "util/file_util.h"

#include <Windows.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>

namespace InstanceLock
{

namespace
{

// A shared instance may run unprotected, but an exclusive one must not
void handleFailure(const bool exclusive, const std::string& what)
{
    if (exclusive)
    {
        Logger::logError("Exclusive mode: %s", what.c_str());
        std::exit(EXIT_FAILURE);
    }

    Logger::logWarning("Instance lock: %s", what.c_str());
}

} // namespace

void acquire(const bool exclusive)
{
    std::filesystem::path lockDir;
    try
    {
        lockDir = FileUtil::getLocalAppDataDir("locks");
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        handleFailure(exclusive, std::format("failed to create the lock directory: {}", error.what()));
        return;
    }
    if (lockDir.empty())
    {
        handleFailure(exclusive, "failed to locate %LOCALAPPDATA%");
        return;
    }

    const std::filesystem::path lockFilePath = lockDir / "instance.lock";
    const HANDLE lockFile = CreateFileW(lockFilePath.c_str(),
                                        GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
    if (lockFile == INVALID_HANDLE_VALUE)
    {
        handleFailure(exclusive, std::format("failed to open {} (error {})", lockFilePath.string(), GetLastError()));
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
    bool ownsTurnstile = false;
    if (turnstile)
    {
        // WAIT_ABANDONED means the previous holder died while holding the turnstile, which still grants ownership
        DWORD waitResult = WaitForSingleObject(turnstile, 0);
        if (waitResult == WAIT_TIMEOUT)
        {
            logWaiting();
            waitResult = WaitForSingleObject(turnstile, INFINITE);
        }
        ownsTurnstile = waitResult != WAIT_FAILED;
    }
    // Without the turnstile a shared instance only loses starvation protection, so it still takes the file lock
    if (!ownsTurnstile)
    {
        handleFailure(exclusive, std::format("failed to take the turnstile (error {})", GetLastError()));
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

    if (ownsTurnstile)
    {
        ReleaseMutex(turnstile);
    }
    if (turnstile)
    {
        CloseHandle(turnstile);
    }

    if (!locked)
    {
        handleFailure(exclusive, std::format("failed to lock {} (error {})", lockFilePath.string(), lockError));
    }
}

} // namespace InstanceLock
