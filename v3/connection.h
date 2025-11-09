#pragma once
#include <string>
#include <mutex>
#include <atomic>
#include <memory>

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

    std::string incoming_buf;

    std::mutex mtx;
    std::atomic<bool> closed{false};

    bool is_write_armed{false};

    explicit Connection(int fd_) : fd(fd_) {}
};

using ConnPtr = std::shared_ptr<Connection>;