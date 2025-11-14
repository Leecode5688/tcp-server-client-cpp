#include "threadpool.h"
#include "webserver.h"
#include <unistd.h>
#include <sys/epoll.h>
#include <vector>
#include <sys/eventfd.h>
#include <iostream>

// ThreadPool::ThreadPool(size_t n_workers, int notify_fd, WebServer& server):
//     notify_fd_(notify_fd), server_(server)

ThreadPool::ThreadPool(size_t n_workers, int notify_fd):
    notify_fd_(notify_fd)
{
    workers_.reserve(n_workers);
    for(size_t i = 0; i < n_workers; ++i)
    {
        workers_.emplace_back(
            [this]{this->worker_loop();} );
    }
}

ThreadPool::~ThreadPool()
{   
    shutdown();
}

void ThreadPool::push_task(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        task_queue_.emplace(std::move(task));
    }
    queue_cv_.notify_one();
}

void ThreadPool::shutdown()
{
    bool expected = true;
    if(!running_.compare_exchange_strong(expected, false)) return;
    queue_cv_.notify_all();
    for(auto& worker : workers_)
    {
        if(worker.joinable())
        {
            worker.join();
        }
    }
}

void ThreadPool::worker_loop()
{
    while(running_)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(queue_mtx_);
            queue_cv_.wait(lk, [&]{
                return !task_queue_.empty() || !running_;
            });

            if(!running_ && task_queue_.empty()) return;

            task = std::move(task_queue_.front());
            task_queue_.pop();
        }

        task();
    }
}