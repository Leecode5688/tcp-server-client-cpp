#pragma once
#include "connection.h"
#include "threadpool.h"
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>

class WebServer{
private:
    void setup_signalfd();
    void setup_server_socket();
    void setup_epoll();
    void setup_eventfd();

    static void set_nonblocking(int fd);
    void add_fd_to_epoll(int fd, uint32_t events);
    void mod_fd_epoll(int fd, uint32_t events);

    void accept_loop();
    void handle_read(ConnPtr conn);
    void handle_write(ConnPtr conn);
    void close_conn(ConnPtr conn);
    void handle_pending_writes();

    int port_;
    int n_workers_;
    std::atomic<bool> running_;
    int listen_fd_{-1};
    int epoll_fd_{-1};
    int sig_fd_{-1};
    int notify_fd_{-1};
    
    std::unique_ptr<ThreadPool> threadpool_;
    //a map protected by mutex that stores fd to their corresponding connection state
    std::unordered_map<int, ConnPtr> connections_;
    std::mutex conn_map_mtx_;

public:
    WebServer(int port, int n_workers = 5);
    ~WebServer();

    void run();
    void stop();
};
