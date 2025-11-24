#pragma once
#include <string>
#include <mutex>
#include <atomic>
#include <memory>
#include <queue>
#include <deque>
#include "ringbuffer.h"

enum class ConnState {
    AWAITING_USERNAME,
    ACTIVE
};

struct Connection {
    int fd;
    std::string username;
    ConnState state = ConnState::AWAITING_USERNAME;
    
    RingBuffer in_buf{8192};
    // RingBuffer out_buf{8192};

    std::deque<std::shared_ptr<std::string>> outgoing_queue;

    std::mutex mtx;
    std::atomic<bool> closed{false};

    bool is_write_armed{false};
    bool needs_processing{false};

    explicit Connection(int fd_) : fd(fd_) {}
};

using ConnPtr = std::shared_ptr<Connection>;