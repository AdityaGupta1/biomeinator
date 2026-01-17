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

#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool
{
private:
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::function<void()>> queue;
    void worker();
    bool stop{ false };

public:
    ThreadPool(uint32_t numWorkers = std::thread::hardware_concurrency());
    ~ThreadPool();

    template<class F, class... Args>
    void enqueue(F&& f, Args&&... args);

    ThreadPool(ThreadPool&) = delete;
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
};

template<class F, class... Args>
void ThreadPool::enqueue(F&& f, Args&&... args)
{
    using R = std::invoke_result_t<F, Args...>;
    auto task = std::make_shared<std::packaged_task<R()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    {
        std::lock_guard<std::mutex> lock(this->mutex);
        ASSERT(!stop);
        this->queue.emplace([task]() { (*task)(); });
    }

    this->cv.notify_one();
}
