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
    //a vector to hold the actual worker threads
    std::vector<std::thread> workers_;
    //a queue that holds connections and their associated messages to be processed
    std::queue<std::pair<ConnPtr, std::string>> task_queue_;
    //use mutex and condition variable to add/remove tasks safely
    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;
    //thread-safe flag to signal a shutdown for all threads
    std::atomic<bool> running_{true};
    //the event fd to notify the main epoll loop
    int notify_fd_;

    WebServer& server_;
    
public: 
    ThreadPool(size_t n_workers, int notify_fd, WebServer& server);
    ~ThreadPool();
    //add new task to the queue for a worker to process
    void push_task(ConnPtr conn, std::string msg);
    //method to stop all threads and clean up
    void shutdown();
};