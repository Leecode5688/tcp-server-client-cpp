#pragma once
#include "connection.h"
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>

class WebServer;

class ThreadPool {
private: 
    void worker_loop();
    std::vector<std::thread> workers_;
    std::queue<std::pair<ConnPtr, std::string>> task_queue_;
    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;
    std::atomic<bool> running_{true};
    int notify_fd_;

    WebServer& server_;
public:
    ThreadPool(size_t n_workers, int notify_fd, WebServer& server);
    ~ThreadPool();
    void push_task(ConnPtr conn, std::string msg);
    void shutdown();
};