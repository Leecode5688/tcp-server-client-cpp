#pragma once
#include <string>
#include <mutex>
#include <atomic>
#include <memory>
#include <queue>
#include <deque>
#include <chrono>
#include <netinet/in.h>

#include "ringbuffer.h"
#include "codec.h"
#include "utils.h"


enum class ConnState {
    AWAITING_USERNAME,
    ACTIVE
};

struct OutgoingPacket {
    std::shared_ptr<std::vector<char>> payload;
    size_t sent_offset = 0;
};

struct Connection {
    //wrapper around raw fd 
    Socket sock;
    std::string username;
    ConnState state = ConnState::AWAITING_USERNAME;

    //circular buffer for read operations
    RingBuffer in_buf{8192};

    //queue of outgoing packets to be sent, protected by mtx
    std::deque<OutgoingPacket> outgoing_queue;

    //total bytes (header + payload) currently pending in outgoing_queue
    //used for flow control (high and low water marks)
    size_t pending_bytes = 0;

    //flow control, true if we have disabled EPOLLIN for this client because they are sending data 
    //faster than we can write to them
    bool is_reading_paused = false;

    //mutex protecting in_bufm outginq_queue, state flags
    std::mutex mtx;

    //atomic flag to ensure we only close the connection once
    std::atomic<bool> closed{false};

    //epoll state tracking
    uint32_t current_epoll_events{0};

    //optimization flags to avoid unnecessary epoll_mod calls
    //true if EPOLLOUT is currently set
    bool is_write_armed{false};
    //true if currently in the processing queue
    bool needs_processing{false};

    //for rate limiting
    int message_count = 0;
    std::chrono::steady_clock::time_point last_msg_time;

    //optimization: idle timeout tracking
    std::chrono::steady_clock::time_point last_activity;

    bool is_broadcast_recipient = false;

    std::unique_ptr<IProtocolCodec> codec;

    explicit Connection(int fd_) : sock(fd_) {
        last_msg_time = std::chrono::steady_clock::now();
        update_activity();
    }

    void update_activity()
    {
        last_activity = std::chrono::steady_clock::now();
    }

    int fd() const { return sock.get();}
};

using ConnPtr = std::shared_ptr<Connection>;