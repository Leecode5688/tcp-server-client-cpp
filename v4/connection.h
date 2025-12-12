#pragma once
#include "ringbuffer.h"
#include "codec.h"
#include "utils.h"
#include <netinet/in.h>
#include <chrono>
#include <atomic>
#include <memory>
#include <string>
#include <mutex>
#include <deque>
#include <any>


struct OutgoingPacket {
    std::shared_ptr<std::vector<char>> payload;
    size_t sent_offset = 0;
};

struct Connection {
    //wrapper around raw fd 
    Socket sock;

    //generic hook
    std::any user_data;

    std::unique_ptr<PacketCodec> codec;
    RingBuffer in_buf{8192};
    std::deque<OutgoingPacket> outgoing_queue;
    size_t pending_bytes = 0;

    std::mutex mtx;
    std::atomic<bool> closed{false};

    uint32_t current_epoll_events{0};
    bool is_write_armed{false};
    bool is_reading_paused{false};

    std::chrono::steady_clock::time_point last_activity;

    explicit Connection(int fd_) : sock(fd_)
    {
        last_activity = std::chrono::steady_clock::now();
    }

    void update_activity()
    {
        last_activity = std::chrono::steady_clock::now();
    }
    int fd() const
    {
        return sock.get();
    }
};

using ConnPtr = std::shared_ptr<Connection>;

