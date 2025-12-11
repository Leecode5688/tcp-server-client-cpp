#pragma once
#include "connection.h"
#include "threadpool.h"
#include "object_pool.h"
#include "safe_queue.h"
#include "event.h"
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <netinet/in.h>
#include <functional>
#include <sys/uio.h>
#include <memory>
#include <atomic>
#include <vector>
#include <mutex>
#include <deque>

//batch size for processing writes to avoid starving the read loop
#define WRITE_BUDGET_PER_LOOP 100
#define NUM_SHARDS 16

class WebServer{
private:
    struct BroadcastEvent {
        int sender_fd;
        std::shared_ptr<std::vector<char>> payload;
    };

    struct TimerEvent {
        std::chrono::steady_clock::time_point expires_at;
        int fd;
        bool operator>(const TimerEvent& other) const {
            return expires_at > other.expires_at;
        }
    };

    struct alignas(64) Shard {
        std::unordered_map<int, ConnPtr> connections;
        std::priority_queue<TimerEvent, std::vector<TimerEvent>, std::greater<TimerEvent>> timer_heap;

        mutable std::shared_mutex mtx;
    };

    size_t get_shard_idx(int fd) const {
        return fd % shards_.size();
    }

    //sharding
    std::vector<Shard> shards_;

    //parallel broadcast
    std::deque<BroadcastEvent> global_queue_;
    std::mutex global_queue_mtx_;
    std::unique_ptr<ThreadPool> threadpool_;
    
    //server config
    int port_;
    int n_workers_;
    std::atomic<bool> running_;

    //file descriptor setup
    int listen_fd_{-1};
    int epoll_fd_{-1};
    int sig_fd_{-1};
    int notify_fd_{-1};
    int timer_fd_{-1};
    
    //for memory optimization
    ObjectPool<std::vector<char>> buffer_pool_;

    //private functions

    void setup_signalfd();
    void setup_server_socket();
    void setup_epoll();
    void setup_eventfd();
    void setup_timerfd();
    static void set_nonblocking(int fd);
    static void set_tcp_nodelay(int fd);
    void add_fd_to_epoll(int fd, uint32_t events);
    void mod_fd_epoll(int fd, uint32_t events);
    void update_epoll_events(ConnPtr conn, uint32_t new_events);

    //core reactor functions, run in main thread
    void accept_loop();
    void handle_read(ConnPtr conn);
    void handle_write(ConnPtr conn);
    void close_conn(ConnPtr conn);
    void check_timeouts();
    void flush_all_queues();
    bool attempt_write(ConnPtr conn);
    void process_global_queue();    

public:
    WebServer(int port, int n_workers = 5);
    ~WebServer();

    SafeQueue<NetworkEvent> event_queue_;

    //api
    void SendPreEncoded(ConnPtr conn, std::shared_ptr<std::vector<char>> packet);
    void Broadcast(const std::vector<char>& data, int exclude_fd = -1);
    void Close(ConnPtr conn);
    
    void Run();
    void Stop();
};
