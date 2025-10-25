#include "threadpool.h"
#include "webserver.h"
#include <unistd.h>
#include <sys/eventfd.h>
#include <iostream>

//global mutex to protect std::cout
std::mutex log_mtx;

//initialize the thread pool with a specified number of worker threads
//initialize the notify_fd with the provided file descriptor
ThreadPool::ThreadPool(size_t n_workers, int notify_fd, WebServer& server) : 
    notify_fd_(notify_fd), server_(server)
{
    workers_.reserve(n_workers);
    for(size_t i = 0; i < n_workers; i++)
    {
        workers_.emplace_back(
            [this]{ this->worker_loop(); });
    }
}

ThreadPool::~ThreadPool(){
    shutdown();
}

//add a new task to the queue and notify one worker thread
void ThreadPool::push_task(ConnPtr conn, std::string message)
{
    //lock the queue while adding a new task
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        task_queue_.emplace(std::move(conn), std::move(message));
    }
    //notify one worker that's waiting on queue_cv_
    queue_cv_.notify_one();
}

//shutdown the thread pool gracefully
//signal all workers to stop, wake up the sleeping threads, and join them
void ThreadPool::shutdown()
{
    bool expected = true;
    //if running_ is true, set it to false and proceed
    //if it's already false, just return 
    //ensures shutdown is only done once
    if(!running_.compare_exchange_strong(expected, false)) return;
    //wake up all sleeping workers
    queue_cv_.notify_all();
    for(auto &worker : workers_)
    {
        if(worker.joinable())
        {
            worker.join();
        }
    }
}

//the worker thread's main loop for broadcasting
void ThreadPool::worker_loop()
{
    while(running_)
    {
        //a container for next task
        std::pair<ConnPtr, std::string> task;
        //lock the task queue and wait for a task or shutdown signal
        {
            std::unique_lock<std::mutex> lk(queue_mtx_);
            //waits until either there is a task or running_ is set to false
            //prevent spurious wakeups by checking the condition in the lambda
            queue_cv_.wait(lk, [&]{
                return !task_queue_.empty() || !running_;
            });
            if(!running_ && task_queue_.empty()) return;
            task = std::move(task_queue_.front());
            task_queue_.pop();
        }

        ConnPtr conn = task.first;
        std::string& message = task.second;

        if(!conn || conn->closed.load() || conn->username.empty()) continue;
        
        std::string broadcast_msg = "["+ conn->username + "]: "+ 
        message.substr(0, message.size()-1) + "\n";

        server_.queue_broadcast_message(broadcast_msg, conn->fd);

        //notify the event loop
        uint64_t one = 1;
        write(notify_fd_, &one, sizeof(one));
    }
}