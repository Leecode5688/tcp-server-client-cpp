#pragma once
#include <string>
#include <mutex>
#include <atomic>
#include <memory>
#include <queue>

enum class ConnState {
    AWAITING_USERNAME,
    ACTIVE
};

//add double buffer to reduce lock time 
struct Connection {
    int fd;
    std::string username;
    ConnState state = ConnState::AWAITING_USERNAME;
    std::string in_buf;
    //out_buf now will only be used by main WebServer thread
    std::string out_buf;

    // std::string incoming_buf;

    //replace  string incoming_buf with queue that holds
    std::queue<std::shared_ptr<std::string>> incoming_message_queue;

    std::mutex mtx;
    std::atomic<bool> closed{false};

    bool is_write_armed{false};
    bool needs_processing{false};

    explicit Connection(int fd_) : fd(fd_) {}
};

using ConnPtr = std::shared_ptr<Connection>;