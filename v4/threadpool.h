#pragma once
#include "connection.h"
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <functional>

class WebServer;

class ThreadPool {
private: 
    void worker_loop();
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> task_queue_;
    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;
    std::atomic<bool> running_{true};
    int notify_fd_;

public:
    ThreadPool(size_t n_workers, int notify_fd);
    ~ThreadPool();
    void push_task(std::function<void()> task);    
    void push_batch(std::vector<std::function<void()>>& tasks);
    void shutdown();
};