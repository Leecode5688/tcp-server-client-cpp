#pragma once
#include <vector>
#include <mutex>
#include <memory>
#include <functional>

template<typename T>
class ObjectPool {
private: 
    std::vector<T*> pool_;
    std::mutex mtx_;
public: 
    ~ObjectPool() {
        std::lock_guard<std::mutex> lock(mtx_);
        for(T* ptr : pool_)
        {
            delete ptr;
        }
        pool_.clear();
    }

    T* acquire()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(pool_.empty())
        {
            return new T();
        }

        T* ptr = pool_.back();
        pool_.pop_back();
        return ptr;
    }

    void release(T* ptr)
    {       
        ptr->clear(); 
        std::lock_guard<std::mutex> lock(mtx_);
        pool_.push_back(ptr);
    }

    std::shared_ptr<T> acquire_shared()
    {
        T* ptr = acquire();
        return std::shared_ptr<T>(ptr, [this](T* p)
        {
            release(p);
        });
    }
};