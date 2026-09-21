// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

// https://dev.to/ish4n10/making-a-thread-pool-in-c-from-scratch-bnm

#pragma once

#include "debug.h"

#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class Chunk;
class ThreadMemoryAllocator;

struct Task
{
    void (*func)(Chunk*, ThreadMemoryAllocator&);
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
    std::atomic<uint64_t> busyNanos{ 0 }; // summed over workers, for utilization measurements
    std::atomic<uint32_t> numPendingTasks{ 0 }; // queued or executing

public:
    ThreadPool();

    void init(uint32_t numWorkers = std::thread::hardware_concurrency() - 1);

    void enqueue(Task task);
    template<class Iter>
    void bulkEnqueue(Iter first, Iter last);

    void shutdown();

    uint32_t getNumWorkers() const
    {
        return static_cast<uint32_t>(this->workers.size());
    }

    uint64_t getBusyNanos() const
    {
        return this->busyNanos.load(std::memory_order_relaxed);
    }

    uint32_t getNumPendingTasks() const
    {
        return this->numPendingTasks.load(std::memory_order_relaxed);
    }

    ThreadPool(ThreadPool&) = delete;
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
};

template<class Iter>
void ThreadPool::bulkEnqueue(Iter first, Iter last)
{
    uint32_t numTasksEnqueued = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);

        ASSERT(!stop);

        for (; first != last; ++first)
        {
            queue.push(*first);
            ++numTasksEnqueued;
        }
        this->numPendingTasks.fetch_add(numTasksEnqueued, std::memory_order_relaxed);
    }

    if (numTasksEnqueued == 1)
    {
        cv.notify_one();
    }
    else
    {
        cv.notify_all();
    }
}
