#include "webserver.h"
#include <iostream>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/signalfd.h>
#include <sys/eventfd.h>

#define BACKLOG 128
#define BUFFER_SIZE 4096
#define MAX_EVENTS 128

WebServer::WebServer(int port, int n_workers) : 
    port_(port), n_workers_(n_workers), running_(true)
{
    setup_signalfd();
    setup_server_socket();
    setup_epoll();
    setup_eventfd();
    threadpool_ = std::make_unique<ThreadPool>(n_workers_, notify_fd_, *this);
}

WebServer::~WebServer(){
    stop();
}

//helper function to format messages before queuing
std::string WebServer::format_message(const std::string& msg)
{
    uint32_t len = msg.size();
    uint32_t net_len = htonl(len);

    std::string packet;
    packet.append(reinterpret_cast<const char*>(&net_len), sizeof(uint32_t));
    packet.append(msg);
    return packet;
}

std::vector<ConnPtr> WebServer::get_active_connections()
{
    std::vector<ConnPtr> active_conns;
    // std::lock_guard<std::mutex> lk(conn_map_mtx_);
    std::shared_lock<std::shared_mutex> lk(conn_map_mtx_);
    active_conns.reserve(connections_.size());
    for(const auto& conn_pair : connections_)
    {
        active_conns.push_back(conn_pair.second);
    }
    return active_conns;
}

void WebServer::setup_signalfd()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if(sigprocmask(SIG_BLOCK, &mask, nullptr) == -1)
    {
        perror("sigprocmask");
        exit(1);
    }

    sig_fd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if(sig_fd_ == -1)
    {
        perror("signalfd");
        exit(1);
    }
}

void WebServer::setup_server_socket()
{
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd_ < 0)
    {
        perror("Socket creation failed!!");
        exit(1);
    }

    int opt = 1;
    if(setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        perror("Setsockopt failed!!");
        exit(1);
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port_);

    if(bind(listen_fd_, (sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed!!");
        exit(1);
    }

    if(listen(listen_fd_, BACKLOG) < 0)
    {
        perror("Listen failed!!");
        exit(1);
    }
    set_nonblocking(listen_fd_);
}

void WebServer::setup_epoll()
{
    epoll_fd_ = epoll_create1(0);
    if(epoll_fd_ == -1)
    {
        perror("epoll_create1");
        exit(1);
    }
    add_fd_to_epoll(listen_fd_, EPOLLIN);
    add_fd_to_epoll(sig_fd_, EPOLLIN);
    add_fd_to_epoll(notify_fd_, EPOLLIN);
}

void WebServer::setup_eventfd()
{
    notify_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if(notify_fd_ == -1)
    {
        perror("eventfd");
        exit(1);
    }
    add_fd_to_epoll(notify_fd_, EPOLLIN);
}

void WebServer::set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1)
    {
        flags = 0;
    }
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void WebServer::add_fd_to_epoll(int fd, uint32_t events)
{
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = events | EPOLLET;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
}

void WebServer::mod_fd_epoll(int fd, uint32_t events)
{
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = events | EPOLLET;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

void WebServer::accept_loop()
{
    while(true)
    {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, (sockaddr*)&client_addr, &client_len);
        if(client_fd == -1)
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK) break;
            else
            {
                perror("Accept failed!!");
                break;
            }
        }

        set_nonblocking(client_fd);
        ConnPtr conn = std::make_shared<Connection>(client_fd);
        {
            // std::lock_guard<std::mutex> lk(conn_map_mtx_);
            std::lock_guard<std::shared_mutex> lk(conn_map_mtx_);
            connections_[client_fd] = conn;
        }

        add_fd_to_epoll(client_fd, EPOLLIN);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        std::cout<<"Accepted "<<ip<<":"<<ntohs(client_addr.sin_port)
            <<" fd = "<<client_fd<<"\n";

        std::string welcomeMessage = format_message("[Server]: Welcome! Please enter your username: ");
        send(client_fd, welcomeMessage.c_str(), welcomeMessage.size(), 0);
    }
}

void WebServer::handle_read(ConnPtr conn)
{
    char buffer[BUFFER_SIZE];
    while(true)
    {
        ssize_t n = recv(conn->fd, buffer, sizeof(buffer), 0);
        if(n > 0)
        {
            std::lock_guard<std::mutex> lk(conn->mtx);
            conn->in_buf.append(buffer, n);
            
            //v3 change: replace the \n find() loop
            while(true)
            {
                //do we have 4-byte header now?
                if(conn->in_buf.size() < sizeof(uint32_t))
                {
                    break;
                }

                //if yes, read the header 
                uint32_t net_len;
                memcpy(&net_len, conn->in_buf.data(), sizeof(uint32_t));
                uint32_t payload_len = ntohl(net_len);

                //Do we have ful header + payyload?
                if(conn->in_buf.size() < (sizeof(uint32_t) + payload_len))
                {
                    break;
                }
                
                //if yes, do message extraction
                std::string line = conn->in_buf.substr(sizeof(uint32_t), payload_len);
                conn->in_buf.erase(0, sizeof(uint32_t) + payload_len);

                if(conn->state == ConnState::AWAITING_USERNAME)
                {
                    std::string enteredName = line;
                    bool validName = false;
                    {
                        // std::unique_lock<std::shared_mutex> map_lk(conn_map_mtx_);
                        std::unique_lock<std::shared_mutex> user_name_lk(usernames_mtx_);
                        if(!usernames_.count(enteredName) && !enteredName.empty())
                        {
                            usernames_.insert(enteredName);
                            validName = true;
                        }
                    }

                    if(validName)
                    {
                        std::unique_lock<std::shared_mutex> user_name_lk(usernames_mtx_);
                        {
                            usernames_.insert(enteredName);
                        }
                        conn->username = enteredName;
                        conn->state = ConnState::ACTIVE;
                        std::string validMessage = "[Server]: Username accepted! You have entered the chat!\n";
                        // send as length-prefixed packet
                        {
                            std::string pkt = format_message(validMessage);
                            send(conn->fd, pkt.data(), pkt.size(), MSG_NOSIGNAL);
                        }

                        std::string joinMessage = "[Server]: " + conn->username + " has joined the chatroom!\n";
                        // queue_broadcast_message(joinMessage, -1);

                        threadpool_->push_task(nullptr, joinMessage);

                        // uint64_t one = 1;
                        // write(notify_fd_, &one, sizeof(one));
                    }
                    else
                    {
                        std::string errorMessage = "[Server]: Username is invalid or already used. Please try another one.\n";
                        std::string pkt = format_message(errorMessage);
                        send(conn->fd, pkt.data(), pkt.size(), MSG_NOSIGNAL);
                    }
                }
                else if(conn->state == ConnState::ACTIVE)
                {
                    // threadpool_->push_task(conn, line + '\n');
                    threadpool_->push_task(conn, line);
                }
            }
        }
        else if(n == 0)
        {
            close_conn(conn);
            return;
        }
        else
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK) break;

            close_conn(conn);
            return;
        }
    }
}

void WebServer::handle_write(ConnPtr conn)
{
    // std::lock_guard<std::mutex> lk(conn->mtx);
    
    while(!conn->out_buf.empty())
    {
        ssize_t n = send(conn->fd, conn->out_buf.data(), conn->out_buf.size(), MSG_NOSIGNAL);
        if(n > 0)
        {
            conn->out_buf.erase(0, n);
        }
        else if(n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return;
        }
        else
        {
            close_conn(conn);
            return;
        }
    }
    mod_fd_epoll(conn->fd, EPOLLIN);
    conn->is_write_armed = false;
}

void WebServer::close_conn(ConnPtr conn)
{
    if(conn->closed.exchange(true)) return;
    std::string leave_msg;

    if(!conn->username.empty())
    {
        leave_msg = "[Server]: "+ conn->username + " has left the chatroom.";
        std::unique_lock<std::shared_mutex> user_name_lk(usernames_mtx_);
        usernames_.erase(conn->username);
        
    }

    if(!leave_msg.empty())
    {
        threadpool_->push_task(nullptr, leave_msg);
    }

    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, conn->fd, nullptr);
    close(conn->fd);
    {
        // std::lock_guard<std::mutex> lk(conn_map_mtx_);
        std::unique_lock<std::shared_mutex> lk(conn_map_mtx_);
        connections_.erase(conn->fd);
    }

    std::cout<<"Closed fd = "<<conn->fd<<"\n";
}

void WebServer::mark_fd_for_writing(int fd)
{
    std::lock_guard<std::mutex> lk(ready_to_write_mtx_);
    ready_to_write_fd_queue_.push(fd);
}

void WebServer::handle_pending_writes()
{

    std::queue<int> fds_to_process;
    {
        std::lock_guard<std::mutex> lk(ready_to_write_mtx_);
        fds_to_process.swap(ready_to_write_fd_queue_);
    }

    std::unordered_set<int> unique_fds;
    while(!fds_to_process.empty())
    {
        unique_fds.insert(fds_to_process.front());
        fds_to_process.pop();
    }

    std::shared_lock<std::shared_mutex> lk(conn_map_mtx_);

    // for(auto& c : connections_)
    for(int fd : unique_fds)
    {
        auto it = connections_.find(fd);
        if(it == connections_.end()) continue;
        ConnPtr conn = it->second;
        
        // auto& conn = c.second;

        if(!conn || conn->closed.load()) continue;
        
        std::lock_guard<std::mutex> conn_lk(conn->mtx);
        if(!conn->incoming_buf.empty())
        {
            conn->out_buf.append(conn->incoming_buf);
            conn->incoming_buf.clear();
        }

        if(!conn->out_buf.empty() && !conn->is_write_armed)
        {
            mod_fd_epoll(conn->fd, EPOLLIN | EPOLLOUT);
            conn->is_write_armed = true;
        }

    }
}

void WebServer::run()
{
    std::cout<<"Chat server listening on port "<<port_<<"... :)\n";
    struct epoll_event events[MAX_EVENTS];

    while(running_)
    {
        int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if(nfds == -1)
        {
            if(errno == EINTR) continue;

            perror("epoll_wait");
            break;
        }

        for(int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if(fd == listen_fd_)
            {
                accept_loop();
            }
            else if(fd == sig_fd_)
            {
                struct signalfd_siginfo si;
                read(sig_fd_, &si, sizeof(si));
                std::cout<<"Signal received, shutting down...\n";
                running_ = false;
                break;
            }
            else if(fd == notify_fd_)
            {
                uint64_t cnt;
                read(notify_fd_, &cnt, sizeof(cnt));
                handle_pending_writes();
            }
            else
            {
                ConnPtr conn;
                {
                    // std::lock_guard<std::mutex> lk(conn_map_mtx_);
                    std::shared_lock<std::shared_mutex> lk(conn_map_mtx_);
                    auto it = connections_.find(fd);
                    if(it != connections_.end())
                    {
                        conn = it->second;
                    }
                }

                if(!conn) continue;

                if(ev & (EPOLLERR | EPOLLHUP))
                {
                    close_conn(conn);
                    continue;
                }

                if(ev & EPOLLIN)
                {
                    handle_read(conn);
                }

                if(ev & EPOLLOUT)
                {
                    handle_write(conn);
                }
            }
        }
    }
    stop();
}

void WebServer::stop()
{
    running_ = false;
    if(threadpool_)
    {
        threadpool_->shutdown();
    }

    {
        // std::lock_guard<std::mutex> lk(conn_map_mtx_);
        std::unique_lock<std::shared_mutex> lk(conn_map_mtx_);
        for(auto& c : connections_)
        {
            ::close(c.first);
        }
        connections_.clear();
    }

    if(listen_fd_ >= 0)
    {
        close(listen_fd_);
    }
    if(epoll_fd_ >= 0)
    {
        close(epoll_fd_);
    }
    if(sig_fd_ >= 0)
    {
        close(sig_fd_);
    }
    if(notify_fd_ >= 0)
    {
        close(notify_fd_);
    }
}
