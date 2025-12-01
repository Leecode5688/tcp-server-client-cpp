#pragma once
#include "connection.h"
#include "threadpool.h"
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <memory>
#include <atomic>
#include <mutex>
#include <queue>

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

    //core reactor functions, run in main thread
    void accept_loop();
    void handle_read(ConnPtr conn);
    void handle_write(ConnPtr conn);
    void close_conn(ConnPtr conn);

    void check_timeouts();

    //handles the global write queue
    void handle_pending_writes();
    //worker tasks
    void handle_login_task(ConnPtr conn, std::string username);
    void handle_broadcast_task(ConnPtr sender_conn, std::shared_ptr<std::vector<char>> payload);

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
    
    //global write queue lock
    //protects the queue of fds ready to write
    std::queue<int> ready_to_write_fd_queue_;
    std::mutex ready_to_write_mtx_;

public:
    WebServer(int port, int n_workers = 5);
    ~WebServer();

    std::string format_message(const std::string& msg);
    void mod_fd_epoll(int fd, uint32_t events);
    std::vector<ConnPtr> get_active_connections();

    //called by worker threads to signal that a client has data to send
    void mark_fd_for_writing(const ConnPtr& conn);

    void run();
    void stop();
};
