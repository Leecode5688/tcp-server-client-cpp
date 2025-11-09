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

class WebServer{
private:
    void setup_signalfd();
    void setup_server_socket();
    void setup_epoll();
    void setup_eventfd();

    static void set_nonblocking(int fd);
    void add_fd_to_epoll(int fd, uint32_t events);
    // void mod_fd_epoll(int fd, uint32_t events);

    void accept_loop();
    void handle_read(ConnPtr conn);
    void handle_write(ConnPtr conn);
    void close_conn(ConnPtr conn);
    void handle_pending_writes();
    // void handle_broadcasts();
    //add a message formatting helper function
    // std::string format_message(const std::string& msg);

    int port_;
    int n_workers_;
    std::atomic<bool> running_;
    int listen_fd_{-1};
    int epoll_fd_{-1};
    int sig_fd_{-1};
    int notify_fd_{-1};
    
    std::unique_ptr<ThreadPool> threadpool_;
    std::unordered_map<int, ConnPtr> connections_;
    std::unordered_set<std::string> usernames_;
    // std::mutex conn_map_mtx_;
    std::shared_mutex conn_map_mtx_;

    // std::queue<std::pair<std::string, int>> broadcast_queue_;
    // std::mutex broadcast_mtx_;

public:
    WebServer(int port, int n_workers = 5);
    ~WebServer();

    std::string format_message(const std::string& msg);
    void mod_fd_epoll(int fd, uint32_t events);
    std::vector<ConnPtr> get_active_connections();

    void run();
    void stop();
    
    // void queue_broadcast_message(const std::string& msg, int sender_fd);
};
