// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

// https://dev.to/ish4n10/making-a-thread-pool-in-c-from-scratch-bnm

#include "thread_pool.h"

#include "thread_memory_allocator.h"

#include "rendering/dxr_includes.h"

#include <chrono>

#define MAX_NUM_LOCAL_TASKS 8

ThreadPool::ThreadPool()
{}

void ThreadPool::init(uint32_t numWorkers)
{
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->stop = false;
    }
    ASSERT(this->workers.empty());
    for (int i = 0; i < numWorkers; ++i)
    {
        this->workers.emplace_back(&ThreadPool::worker, this);
    }
}

void ThreadPool::worker()
{
    // See knowledge/multithreading/thread_pool.md for why workers yield to the render thread
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    ThreadMemoryAllocator threadMemoryAlloc{};
    Task localTasks[MAX_NUM_LOCAL_TASKS];

    while (true)
    {
        int numLocalTasks = 0;

        {
            std::unique_lock<std::mutex> lock(this->mutex);
            cv.wait(lock, [this]() { return this->stop || !this->queue.empty(); });

            if (this->stop && this->queue.empty())
            {
                break;
            }

            while (numLocalTasks < MAX_NUM_LOCAL_TASKS && !this->queue.empty())
            {
                localTasks[numLocalTasks++] = this->queue.front();
                this->queue.pop();
            }
        }

        const auto batchStart = std::chrono::steady_clock::now();
        for (int i = 0; i < numLocalTasks; ++i)
        {
            localTasks[i].func(localTasks[i].chunkPtr, threadMemoryAlloc);
            threadMemoryAlloc.clear();
            this->numPendingTasks.fetch_sub(1, std::memory_order_relaxed);
        }
        this->busyNanos.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - batchStart).count(),
            std::memory_order_relaxed);
    }
}

void ThreadPool::enqueue(Task task)
{
    {
        std::lock_guard<std::mutex> lock(mutex);

        ASSERT(!stop);

        queue.push(task);
        this->numPendingTasks.fetch_add(1, std::memory_order_relaxed);
    }

    cv.notify_one();
}

void ThreadPool::shutdown()
{
    {
        std::unique_lock<std::mutex> lock(this->mutex);
        this->stop = true;
        while (!this->queue.empty())
        {
            this->queue.pop();
            this->numPendingTasks.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    cv.notify_all();
    for (std::thread& worker : this->workers)
    {
        worker.join();
    }
    this->workers.clear();
}
