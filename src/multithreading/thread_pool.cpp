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

#include "thread_pool.h"

ThreadPool::ThreadPool(std::size_t numWorkers)
{
    for (int i = 0; i < numWorkers; ++i)
    {
        this->workers.emplace_back(&ThreadPool::worker, this);
    }
}

ThreadPool::~ThreadPool()
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

void ThreadPool::worker()
{
    while (true)
    {
        std::function<void()> currentTask;
        {
            std::unique_lock<std::mutex> lock(this->mutex);
            cv.wait(lock, [this]() { return this->stop || !this->queue.empty(); });

            if (!this->queue.empty())
            {
                currentTask = this->queue.front();
                this->queue.pop();
            }
            else if (this->stop)
            {
                break;
            }
        }

        currentTask();
    }
}

template<typename F, typename... Args>
inline auto ThreadPool::enqueue(F&& f, Args&&... args) -> std::future<decltype(f(args...))>
{
    const auto func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    const auto packagedTaskPtr = std::make_shared<std::packaged_task<decltype(f(args...))()>>(func);

    std::future<std::result_of_t<F(Args...)>> futureObj = packagedTaskPtr->get_future();
    {
        std::unique_lock<std::mutex> lock(this->mutex);
        queue.emplace([packagedTaskPtr]() { (*packagedTaskPtr)(); });
    }

    cv.notify_one();

    return futureObj;
}
