#pragma once
#include <string>
#include <mutex>
#include <atomic>
#include <memory>
#include <queue>
#include <deque>
#include "ringbuffer.h"
#include "utils.h"

enum class ConnState {
    AWAITING_USERNAME,
    ACTIVE
};

struct OutgoingPacket {
    //4 byte header (network order)
    uint32_t net_len;
    //shared payload
    std::shared_ptr<std::vector<char>> payload;
    //track partial writes
    size_t sent_offset = 0;
};

struct Connection {
    // int fd;
    Socket sock;
    std::string username;
    ConnState state = ConnState::AWAITING_USERNAME;
    
    RingBuffer in_buf{8192};
    // RingBuffer out_buf{8192};

    // std::deque<std::shared_ptr<std::string>> outgoing_queue;
    std::deque<OutgoingPacket> outgoing_queue;

    //total bytes (header + payload) waiting to be sent
    size_t pending_bytes = 0;
    //is EPOLLIN currently disabled ?
    bool is_reading_paused = false;

    std::mutex mtx;
    std::atomic<bool> closed{false};

    bool is_write_armed{false};
    bool needs_processing{false};

    // explicit Connection(int fd_) : fd(fd_) {}
    explicit Connection(int fd_) : sock(fd_) {}
    int fd() const { return sock.get();}
};

using ConnPtr = std::shared_ptr<Connection>;