// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

// https://dev.to/ish4n10/making-a-thread-pool-in-c-from-scratch-bnm

#include "thread_pool.h"

#include "thread_memory_allocator.h"

#define MAX_NUM_LOCAL_TASKS 8

ThreadPool::ThreadPool()
{}

void ThreadPool::init(uint32_t numWorkers)
{
    for (int i = 0; i < numWorkers; ++i)
    {
        this->workers.emplace_back(&ThreadPool::worker, this);
    }
}

void ThreadPool::worker()
{
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

        for (int i = 0; i < numLocalTasks; ++i)
        {
            localTasks[i].func(localTasks[i].chunkPtr, threadMemoryAlloc);
            threadMemoryAlloc.clear();
        }
    }
}

void ThreadPool::enqueue(Task task)
{
    {
        std::lock_guard<std::mutex> lock(mutex);

        ASSERT(!stop);

        queue.push(task);
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
        }
    }

    cv.notify_all();
    for (std::thread& worker : this->workers)
    {
        worker.join();
    }
}
