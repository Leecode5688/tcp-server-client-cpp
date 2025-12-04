#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>

//a thread-safe queue 

template <typename T>
class SafeQueue {
private:
    std::queue<T> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool finished_ = false;
public: 
    void push(const T item)
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }
    bool pop(T& item)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        
        cv_.wait(lock, [this]{
            return !queue_.empty() || finished_;
        });

        if(queue_.empty() && finished_) return false;

        //retrieve item
        item = std::move(queue_.front());
        queue_.pop();

        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            finished_ = true;
        }
        cv_.notify_all();
    }
};
