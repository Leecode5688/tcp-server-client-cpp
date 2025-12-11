#pragma once
#include <iostream>
#include <string>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <unistd.h>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>

#define LOG_INFO(msg) AsyncLogger::getInstance().log(msg)
#define LOG_ERROR(msg) AsyncLogger::getInstance().error(msg)

class AsyncLogger {
private:
    std::queue<std::string> log_queue_;
    std::mutex queue_mtx_;
    std::condition_variable cv_;
    std::thread writer_thread_;
    std::atomic<bool> running_;
    std::ofstream log_file_;

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    AsyncLogger() : running_(true)
    {
        log_file_.open("server.log", std::ios::app);
        writer_thread_ = std::thread(&AsyncLogger::writer_loop, this);
    }
    ~AsyncLogger()
    {
        shutdown();
    }

    std::string get_timestamp()
    {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);

        std::stringstream ss;
        struct tm buf;
        localtime_r(&in_time_t, &buf);
        ss<<std::put_time(&buf, "[%Y-%m-%d %H:%M:%S]");
        return ss.str();
    }

    void push_message(std::string msg)
    {
        {
            std::lock_guard<std::mutex> lk(queue_mtx_);
            log_queue_.push(std::move(msg));
        }
        cv_.notify_one();
    }
    void writer_loop()
    {
        while(true)
        {
            std::string msg;
            {
                std::unique_lock<std::mutex> lk(queue_mtx_);
                cv_.wait(lk, [this]{
                    return !log_queue_.empty() || !running_;
                });

                if(!running_ && log_queue_.empty()) return;

                msg = std::move(log_queue_.front());
                log_queue_.pop();
            }
            if(log_file_.is_open())
            {
                log_file_<<msg<<std::endl;
            }
            else
            {
                std::cout<<msg<<std::endl;
            }
        }
    }
    void shutdown()
    {
        if(!running_) return;
        {
            std::lock_guard<std::mutex> lk(queue_mtx_);
            running_ = false;
        }
        cv_.notify_all();
        if(writer_thread_.joinable())
        {
            writer_thread_.join();
        }
    }
public:
    static AsyncLogger& getInstance()
    {
        static AsyncLogger instance;
        return instance;
    }
    void log(const std::string& msg)
    {
        push_message(get_timestamp() + "[INFO]: " + msg);
    }
    void error(const std::string& msg)
    {
        push_message(get_timestamp() + "[ERROR]: " + msg);
    }
};


class Socket {
private:
    int fd_;
public:
    Socket() : fd_(-1) {}
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket()
    {
        close_socket();
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    //move constructor, used for transferring ownership
    Socket(Socket&& other) noexcept : fd_(other.fd_)
    {
        other.fd_ = -1;
    }

    //move assignment
    Socket& operator=(Socket&& other) noexcept {
        if(this != &other)
        {
            close_socket();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const
    {
        return fd_;
    }

    operator bool() const
    {
        return fd_ != -1;
    }

    void close_socket()
    {
        if(fd_ != -1)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }
};