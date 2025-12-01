#pragma once
#include <string>
#include <mutex>
#include <atomic>
#include <memory>
#include <queue>
#include <deque>
#include <chrono>
#include "ringbuffer.h"
#include "utils.h"

enum class ConnState {
    AWAITING_USERNAME,
    ACTIVE
};

//a single message packet to be sent over the network
struct OutgoingPacket {
    //4 byte header (network order), representing the length of payload
    uint32_t net_len;
    //shared payload, allows broadcasting the same data without copying
    std::shared_ptr<std::vector<char>> payload;
    //tracks how many bytes of this specific packet have been sent
    size_t sent_offset = 0;
};

struct Connection {
    // int fd;
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

    //optimization flags to avoid unnecessary epoll_mod calls
    //true if EPOLLOUT is currently set
    bool is_write_armed{false};
    //true if currently in the processing queue
    bool needs_processing{false};

    //optimization: idle timeout tracking
    std::chrono::steady_clock::time_point last_activity;

    explicit Connection(int fd_) : sock(fd_) {
        update_activity();
    }

    void update_activity()
    {
        last_activity = std::chrono::steady_clock::now();
    }

    int fd() const { return sock.get();}
};

using ConnPtr = std::shared_ptr<Connection>;