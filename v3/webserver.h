#pragma once
#include "connection.h"
#include "threadpool.h"
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

class WebServer{
private:
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

    //return true if a fatal error occurred and connection should be closed
    bool attempt_write(ConnPtr conn);

    //centralized pub/sub processor
    void process_global_queue();
    struct BroadcastEvent {
        int sender_fd;
        std::shared_ptr<std::vector<char>> payload;
    };


    //handles the global write queue
    // void handle_pending_writes();

    //worker tasks
    void handle_login_task(ConnPtr conn, std::string username);

    // void handle_broadcast_task(ConnPtr sender_conn, std::shared_ptr<std::vector<char>> payload);

    void publish_to_global_queue(int sender_fd, std::string username, std::shared_ptr<std::vector<char>> payload, bool is_system = false);

    int port_;
    int n_workers_;
    std::atomic<bool> running_;

    int listen_fd_{-1};
    int epoll_fd_{-1};
    int sig_fd_{-1};
    int notify_fd_{-1};
    int timer_fd_{-1};

    std::unique_ptr<ThreadPool> threadpool_;

    //map lock, protects the connections_ map 
    //use shared locks for lookkups and iteration, unique locks for adding and removing clients
    std::unordered_map<int, ConnPtr> connections_;
    std::shared_mutex conn_map_mtx_;

    //username lock
    //protects the set of active usernames
    std::unordered_set<std::string> usernames_;
    std::shared_mutex usernames_mtx_;
    
    //global pub/sub queue queue
    // std::deque<ChatEvent> global_queue_;
    std::deque<BroadcastEvent> global_queue_;
    std::mutex global_queue_mtx_;

public:
    WebServer(int port, int n_workers = 5);
    ~WebServer();

    //callback hooks
    std::function<void(ConnPtr)> OnClientConnect;
    std::function<void(ConnPtr)> OnClientDisconnect;
    std::function<void(ConnPtr, std::vector<char>)> OnMessageRecv;

    //api
    void Send(ConnPtr conn, const std::vector<char>& data);
    void Broadcast(const std::vector<char>& data, int exclude_fd = -1);
    void Close(ConnPtr conn);
    void Run();
    void Stop();
};
