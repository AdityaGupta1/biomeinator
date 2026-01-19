/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

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

// https://dev.to/ish4n10/making-a-thread-pool-in-c-from-scratch-bnm

#pragma once

#include "debug.h"

#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class Chunk;

struct Task
{
    void (*fn)(Chunk*);
    Chunk* chunkPtr;
};

class ThreadPool
{
private:
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<Task> queue;
    void worker();
    bool stop{ false };

public:
    ThreadPool(uint32_t numWorkers = std::thread::hardware_concurrency() - 1);

    void enqueue(Task task);
    template<class It>
    void bulkEnqueue(It first, It last);

    void shutdown();

    ThreadPool(ThreadPool&) = delete;
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
};

template<class It>
void ThreadPool::bulkEnqueue(It first, It last)
{
    bool wasEmpty;
    uint32_t numTasksEnqueued = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);

        ASSERT(!stop);

        wasEmpty = queue.empty();
        for (; first != last; ++first)
        {
            queue.push(*first);
            ++numTasksEnqueued;
        }
    }

    // if queue was not empty, all workers are currently busy
    if (wasEmpty)
    {
        if (numTasksEnqueued == 1)
        {
            cv.notify_one();
        }
        else
        {
            cv.notify_all();
        }
    }
}
