#pragma once
#include <string>
#include <mutex>
#include <atomic>
#include <memory>

enum class ConnState {
    AWAITING_USERNAME,
    ACTIVE
};

struct Connection {
    int fd;
    std::string username;
    ConnState state = ConnState::AWAITING_USERNAME;
    std::string in_buf;
    std::string out_buf;
    std::mutex mtx;
    std::atomic<bool> closed{false};
    explicit Connection(int fd_) : fd(fd_) {}
};

using ConnPtr = std::shared_ptr<Connection>;