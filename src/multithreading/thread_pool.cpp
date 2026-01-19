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

#include "thread_pool.h"

#define MAX_NUM_LOCAL_TASKS 4

ThreadPool::ThreadPool(uint32_t numWorkers)
{
    for (int i = 0; i < numWorkers; ++i)
    {
        this->workers.emplace_back(&ThreadPool::worker, this);
    }
}

void ThreadPool::worker()
{
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
            localTasks[i].fn(localTasks[i].chunkPtr);
        }
    }
}

void ThreadPool::enqueue(Task task)
{
    bool wasEmpty;
    {
        std::lock_guard<std::mutex> lock(mutex);

        ASSERT(!stop);

        wasEmpty = queue.empty();
        queue.push(task);
    }

    // if queue was not empty, all workers are currently busy
    if (wasEmpty)
    {
        cv.notify_one();
    }
}

void ThreadPool::shutdown()
{
    {
        std::unique_lock<std::mutex> lock(this->mutex);
        this->stop = true;
    }

    cv.notify_all();
    for (std::thread& worker : this->workers)
    {
        worker.join();
    }
}
