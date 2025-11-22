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
#define MAX_PAYLOAD_SIZE (1024 * 4)

WebServer::WebServer(int port, int n_workers) : 
    port_(port), n_workers_(n_workers), running_(true)
{
    setup_signalfd();
    setup_server_socket();
    setup_epoll();
    setup_eventfd();

    // threadpool_ = std::make_unique<ThreadPool>(n_workers_, notify_fd_, *this);
    threadpool_ = std::make_unique<ThreadPool>(n_workers, notify_fd_);
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
        // send(client_fd, welcomeMessage.c_str(), welcomeMessage.size(), 0);
        
        auto welcome_pkt = std::make_shared<std::string>(format_message(welcomeMessage));
        {
            std::lock_guard<std::mutex> conn_lk(conn->mtx);
            conn->incoming_message_queue.push(welcome_pkt);
        }
        
        mark_fd_for_writing(conn);
        uint64_t one = 1;
        write(notify_fd_, &one, sizeof(one));
    }
}

void WebServer::handle_login_task(ConnPtr conn, std::string username)
{
    bool validName = false;
    {
        std::unique_lock<std::shared_mutex> user_name_lk(usernames_mtx_);
        if(!usernames_.count(username) && !username.empty() && username.size() < 50)
        {
            usernames_.insert(username);
            validName = true;
        }
    }

    std::string response_msg;
    bool needs_broadcast = false;

    if(validName)
    {
        conn->username = username;
        // conn->state = ConnState::ACTIVE;
        response_msg = "[Server]: Username accepted! You have entered the chat!\n";
        needs_broadcast = true;
    }
    else
    {
        response_msg = "[Server]: Username is invalid or already used. Please try another one: ";    
    }

    //queue the packet for this client
    auto pkt_ptr = std::make_shared<std::string>(format_message(response_msg));
    {
        std::lock_guard<std::mutex> conn_lk(conn->mtx);
        conn->incoming_message_queue.push(pkt_ptr);
    }
    mark_fd_for_writing(conn);

    if(validName)
    {
        conn->state = ConnState::ACTIVE;
    }

    if(needs_broadcast)
    {
        std::string joinMessage = "[Server]: " + conn->username + " has joined the chatroom!\n";    
        threadpool_->push_task([this, joinMessage]{
            this->handle_broadcast_task(nullptr, joinMessage);
        });
    }

    uint64_t one = 1;
    write(notify_fd_, &one, sizeof(one));
}

void WebServer::handle_broadcast_task(ConnPtr sender_conn, std::string message)
{
    std::string broadcast_msg;
    if(sender_conn)
    {
        broadcast_msg = "[" + sender_conn->username + "]: " + message;    
    }
    else
    {
        broadcast_msg = message;
    }

    auto packet_ptr = std::make_shared<std::string>(format_message(broadcast_msg));

    std::vector<ConnPtr> active_conns = get_active_connections();

    for(const auto& conn : active_conns)
    {
        if(!conn || conn->closed.load() || conn->state != ConnState::ACTIVE)
        {
            continue;
        }
        if(sender_conn && conn->fd == sender_conn->fd)
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> conn_lk(conn->mtx);
            conn->incoming_message_queue.push(packet_ptr);
        }
        mark_fd_for_writing(conn);
    }

    uint64_t one = 1;
    write(notify_fd_, &one, sizeof(one));
}


void WebServer::handle_read(ConnPtr conn)
{
    char buffer[BUFFER_SIZE] = {0};
    while(true)
    {
        ssize_t n = recv(conn->fd, buffer, sizeof(buffer), 0);
        if(n > 0)
        {
            std::lock_guard<std::mutex> lk(conn->mtx);
            // conn->in_buf.append(buffer, n);
            if(conn->in_buf.write(buffer, n) < (size_t)n)
            {
                std::cerr<<"RingBuffer full for client "<<conn->fd<<", dropping data.\n";
            }
            
            while(true)
            {
                //do we have 4-byte header now?
                if(conn->in_buf.size() < sizeof(uint32_t))
                {
                    break;
                }

                //if yes, read the header 

                char header[4];
                conn->in_buf.peek(header, 4);

                uint32_t net_len;
                // memcpy(&net_len, conn->in_buf.data(), sizeof(uint32_t));
                memcpy(&net_len, header, sizeof(uint32_t));

                uint32_t payload_len = ntohl(net_len);
                
                if(payload_len > MAX_PAYLOAD_SIZE)
                {
                    std::cout<<"Client "<<conn->fd<<" sent invalid payload size: "<<payload_len<<std::endl;
                    close_conn(conn);
                    return;
                }

                //Do we have ful header + payyload?
                if(conn->in_buf.size() < (sizeof(uint32_t) + payload_len))
                {
                    break;
                }
                
                //if yes, do message extraction
                // std::string line = conn->in_buf.substr(sizeof(uint32_t), payload_len);
                // conn->in_buf.erase(0, sizeof(uint32_t) + payload_len);
                
                std::string line;
                line.resize(payload_len);
                //consume header
                conn->in_buf.consume(sizeof(uint32_t));
                
                //read payload
                conn->in_buf.peek(&line[0], payload_len);

                //consume payload
                conn->in_buf.consume(payload_len);

                if(conn->state == ConnState::AWAITING_USERNAME)
                {
                    threadpool_->push_task([this, conn, line]{
                        this->handle_login_task(conn, line);
                    });
                }
                else if(conn->state == ConnState::ACTIVE)
                {
                    // threadpool_->push_task(conn, line);
                    threadpool_->push_task([this, conn, line] {
                        this->handle_broadcast_task(conn, line);
                    });
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
    std::lock_guard<std::mutex> conn_lk(conn->mtx);
    
    while(!conn->out_buf.empty())
    {
        // ssize_t n = send(conn->fd, conn->out_buf.data(), conn->out_buf.size(), MSG_NOSIGNAL);
        
        //get the pointer of the furrst contiguous chunk of data
        const char* data_ptr = conn->out_buf.get_read_ptr();
        //get the size of the chunk, up to the end if buffer
        size_t data_len = conn->out_buf.get_contiguous_read_size();
        ssize_t n = send(conn->fd, data_ptr, data_len, MSG_NOSIGNAL); 

        if(n > 0)
        {
            // conn->out_buf.erase(0, n);
            conn->out_buf.consume(n);
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

    //check if more works are queued when we were sending
    if(!conn->incoming_message_queue.empty())
    {
        if(!conn->needs_processing)
        {
            conn->needs_processing = true;
            std::lock_guard<std::mutex> lk(ready_to_write_mtx_);
            ready_to_write_fd_queue_.push(conn->fd);
        }
    }
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
        // threadpool_->push_task(nullptr, leave_msg);
        threadpool_->push_task([this, leave_msg] {
            this->handle_broadcast_task(nullptr, leave_msg);
        });
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

// void WebServer::mark_fd_for_writing(int fd)
void WebServer::mark_fd_for_writing(const ConnPtr& conn)
{
    // std::lock_guard<std::mutex> lk(ready_to_write_mtx_);
    std::lock_guard<std::mutex> conn_lk(conn->mtx);
    if(!conn->needs_processing)
    {
        conn->needs_processing = true;
        std::lock_guard<std::mutex> lk(ready_to_write_mtx_);
        ready_to_write_fd_queue_.push(conn->fd);
    }
    // ready_to_write_fd_queue_.push(fd);
}

void WebServer::handle_pending_writes()
{
    int clients_processed = 0;
    std::shared_lock<std::shared_mutex> map_lk(conn_map_mtx_);

    //lock global queue, process our micro-batch
    std::lock_guard<std::mutex> lk(ready_to_write_mtx_);
    
    while(!ready_to_write_fd_queue_.empty() && clients_processed < WRITE_BUDGET_PER_LOOP)
    {
        int fd = ready_to_write_fd_queue_.front();
        ready_to_write_fd_queue_.pop();
        clients_processed++;

        auto it = connections_.find(fd);
        if(it == connections_.end()) continue;

        ConnPtr conn = it->second;

        if(!conn || conn->closed.load())
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> conn_lk(conn->mtx);
            conn->needs_processing = false;

            //drain the incoming queue into the out_buf
            while(!conn->incoming_message_queue.empty())
            {
                // conn->out_buf.append(*conn->incoming_message_queue.front());

                auto msg_ptr = conn->incoming_message_queue.front();
                conn->out_buf.write(msg_ptr->data(), msg_ptr->size());

                conn->incoming_message_queue.pop();
            }

            //arm epollout if it's not already armed and has data
            if(!conn->out_buf.empty() && !conn->is_write_armed)
            {
                mod_fd_epoll(conn->fd, EPOLLIN | EPOLLOUT);
                conn->is_write_armed = true;
            }
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


        if(!ready_to_write_fd_queue_.empty())
        {
            handle_pending_writes();
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
