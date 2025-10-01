#pragma once
#include <string>
#include <mutex>
#include <atomic>
#include <memory>

//a structure that holds the state associated with each active client connection
struct Connection {
    //file descriptor for the connection
    int fd;
    //in_buf stores the data from the socket that hasn't been processed yet
    //out_buf stores the data that is ready to be sent to the client
    std::string in_buf;
    std::string out_buf;
    //mutex to protect access to in_buf and out_buf
    std::mutex mtx;
    //thread-safe boolean flag to indicate if the connection is closed
    std::atomic<bool> closed{false};
    //constructor, use explicit to avoid implicit conversions
    explicit Connection(int fd_) : fd(fd_) {}
};

//convenience alias for a shared pointer to a Connection
using ConnPtr = std::shared_ptr<Connection>;

