#include "webserver.h"
#include "utils.h"
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
#include <sys/uio.h>

#define BACKLOG 128
#define MAX_EVENTS 128
#define MAX_PAYLOAD_SIZE (1024 * 4)

#define WRITE_BUDGET_PER_LOOP 100

//hard limit, if message size exceeds this, drop packets
#define MAX_OUTGOING_QUEUE_SIZE 5000

//high water mark: stop reading from client if they have this much pending data
#define HIGH_WATER_MARK (256 * 1024)
//low water mark => resume reading once they drain to this level
#define LOW_WATER_MARK (128 * 1024)

WebServer::WebServer(int port, int n_workers) : 
    port_(port), n_workers_(n_workers), running_(true)
{
    setup_signalfd();
    setup_server_socket();
    setup_epoll();
    setup_eventfd();

    threadpool_ = std::make_unique<ThreadPool>(n_workers, notify_fd_);
}

WebServer::~WebServer(){
    stop();
}

OutgoingPacket make_packet(const std::string& msg)
{
    OutgoingPacket pkt;
    pkt.payload = std::make_shared<std::vector<char>>(msg.begin(), msg.end());
    pkt.net_len = htonl(pkt.payload->size());
    return pkt;
}

void WebServer::setup_signalfd()
{
    sigset_t mask;
    
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGPIPE);

    if(sigprocmask(SIG_BLOCK, &mask, nullptr) == -1)
    {
        LOG_ERROR("sigprocmask failed: " + std::string(strerror(errno)));
        exit(1);
    }

    sig_fd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if(sig_fd_ == -1)
    {
        LOG_ERROR("signalfd failed: "+ std::string(strerror(errno)));
        exit(1);
    }
}

void WebServer::setup_server_socket()
{
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd_ < 0)
    {
        LOG_ERROR("Socket creation failed: " + std::string(strerror(errno)));        exit(1);
    }

    int opt = 1;
    if(setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        LOG_ERROR("Setsockopt failed: " + std::string(strerror(errno)));
        exit(1);
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port_);

    if(bind(listen_fd_, (sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        LOG_ERROR("Bind failed: " + std::string(strerror(errno)));
        exit(1);
    }

    if(listen(listen_fd_, BACKLOG) < 0)
    {
        LOG_ERROR("Listen failed: " + std::string(strerror(errno)));
        exit(1);
    }
    set_nonblocking(listen_fd_);
}

void WebServer::setup_epoll()
{
    epoll_fd_ = epoll_create1(0);
    if(epoll_fd_ == -1)
    {
        LOG_ERROR("epoll_create1: " + std::string(strerror(errno)));
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
        LOG_ERROR("eventfd: " + std::string(strerror(errno)));
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
    bool accept_any = false;

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
                LOG_ERROR("Accept failed: " + std::string(strerror(errno)));
                break;
            }
        }

        set_nonblocking(client_fd);
        ConnPtr conn = std::make_shared<Connection>(client_fd);
        {
            std::lock_guard<std::shared_mutex> lk(conn_map_mtx_);
            connections_[client_fd] = conn;
        }

        add_fd_to_epoll(conn->fd(), EPOLLIN);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

        LOG_INFO("Accepted "+ std::string(ip) + ":" + std::to_string(ntohs(client_addr.sin_port))
            + " fd = " + std::to_string(client_fd));

        std::string welcomeMessage = "[Server]: Welcome! Please enter your username: ";

        OutgoingPacket pkt = make_packet(welcomeMessage);
        {
            std::lock_guard<std::mutex> conn_lk(conn->mtx);
            conn->outgoing_queue.push_back(pkt);
        }

        mark_fd_for_writing(conn);
        uint64_t one = 1;
        accept_any = true;

        write(notify_fd_, &one, sizeof(one));
    }
    if(accept_any)
    {
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
        {
            std::lock_guard<std::mutex> conn_lk(conn->mtx);
            conn->username = username;
        }
        response_msg = "[Server]: Username accepted! You have entered the chat!\n";
        needs_broadcast = true;
    }
    else
    {
        response_msg = "[Server]: Username is invalid or already used. Please try another one: ";    
    }

    OutgoingPacket pkt = make_packet(response_msg);
    {
        std::lock_guard<std::mutex> conn_lk(conn->mtx);
        conn->outgoing_queue.push_back(pkt);

        conn->pending_bytes += (4 + pkt.payload->size());
    }

    mark_fd_for_writing(conn);

    if(validName)
    {
        std::lock_guard<std::mutex> conn_lk(conn->mtx);
        conn->state = ConnState::ACTIVE;
    }

    if(needs_broadcast)
    {
        std::string joinMessage = "[Server]: " + username + " has joined the chatroom!\n";    
        auto payload_ptr = std::make_shared<std::vector<char>>(joinMessage.begin(), joinMessage.end());
        handle_broadcast_task(nullptr, payload_ptr);
    }

    uint64_t one = 1;
    write(notify_fd_, &one, sizeof(one));
}

void WebServer::handle_broadcast_task(ConnPtr sender_conn, std::shared_ptr<std::vector<char>> payload)
{
    std::shared_ptr<std::vector<char>> final_payload;

    if(sender_conn)
    {
        std::string prefix = "[" + sender_conn->username + "]: ";
        final_payload = std::make_shared<std::vector<char>>();
        final_payload->reserve(prefix.size() + payload->size());
        final_payload->insert(final_payload->end(), prefix.begin(), prefix.end());
        final_payload->insert(final_payload->end(), payload->begin(), payload->end());
    }
    else
    {
        final_payload = payload;
    }

    OutgoingPacket packet;
    packet.net_len = htonl(final_payload->size());
    packet.payload = final_payload;

    {
        std::shared_lock<std::shared_mutex> map_lock(conn_map_mtx_);

        for(const auto& pair : connections_)
        {
            const ConnPtr& conn = pair.second;
            if(!conn || conn->closed.load() || conn->state != ConnState::ACTIVE)
            {
                continue;
            }
            if(sender_conn && conn->fd() == sender_conn->fd())
            {
                continue;
            }

            {
                std::lock_guard<std::mutex> conn_lk(conn->mtx);
               
                //hard limit check to prevent slow clients from overwhelming server memory
                if(conn->outgoing_queue.size() >= MAX_OUTGOING_QUEUE_SIZE) continue;

                conn->outgoing_queue.push_back(packet);
                conn->pending_bytes += (4 + final_payload->size());

                //flow control, pause reading if writing is too slow
                if(!conn->is_reading_paused && conn->pending_bytes >= HIGH_WATER_MARK)
                {
                    conn->is_reading_paused = true;
                    mod_fd_epoll(conn->fd(), EPOLLOUT);
                }
            }

            mark_fd_for_writing(conn);
        }
    }

    uint64_t one = 1;
    write(notify_fd_, &one, sizeof(one));
}


void WebServer::handle_read(ConnPtr conn)
{
    bool should_close = false;
    {
        std::lock_guard<std::mutex> lk(conn->mtx);

        while(true)
        {
            //scatter read, get iovecs to read directly into ring buffer
            auto iovecs = conn->in_buf.get_writeable_iovecs();

            if(iovecs.empty())
            {
                //buffer full, apply backpressure, stop listening for reads
                uint32_t events = 0;
                if(!conn->outgoing_queue.empty()) events |= EPOLLOUT;
                mod_fd_epoll(conn->fd(), events);
                return;
            }

            //read directly into ring buffer in one syscall
            ssize_t n = readv(conn->fd(), iovecs.data(), iovecs.size());

            if(n > 0)
            {   
                conn->in_buf.commit_write(n);
                
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
                    memcpy(&net_len, header, sizeof(uint32_t));

                    uint32_t payload_len = ntohl(net_len);
                    
                    //check for invalid payload size
                    if(payload_len > MAX_PAYLOAD_SIZE)
                    {
                        std::cout<<"Client "<<conn->fd()<<" sent invalid payload size: "<<payload_len<<std::endl;
                        close_conn(conn);
                        return;
                    }

                    //Do we have ful header + payyload?
                    if(conn->in_buf.size() < (sizeof(uint32_t) + payload_len))
                    {
                        break;
                    }

                    conn->in_buf.consume(sizeof(uint32_t));
                    
                    auto payload_ptr = std::make_shared<std::vector<char>>(payload_len);

                    conn->in_buf.peek(payload_ptr->data(), payload_len);
                    conn->in_buf.consume(payload_len);

                    if(conn->state == ConnState::AWAITING_USERNAME)
                    {
                        std::string username(payload_ptr->begin(), payload_ptr->end());
                        threadpool_->push_task([this, conn, username]{
                            this->handle_login_task(conn, username);
                        });
                    }
                    else if(conn->state == ConnState::ACTIVE)
                    {
                        threadpool_->push_task([this, conn, payload_ptr]{
                            this->handle_broadcast_task(conn, payload_ptr);
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
    if(should_close)
    {
        close_conn(conn);
    }
}

void WebServer::handle_write(ConnPtr conn)
{
    bool should_close = false;
    {

        std::lock_guard<std::mutex> lk(conn->mtx);

        if(!conn->outgoing_queue.empty())
        {
            std::vector<struct iovec> iov;
            const int IOV_LIMIT = 64;
            int iov_count = 0;
            auto it = conn->outgoing_queue.begin();

            for( ; it != conn->outgoing_queue.end() && iov_count < IOV_LIMIT ; ++it)
            {
                OutgoingPacket& pkt = *it;
                size_t off = pkt.sent_offset;
                
                //header
                if(off < 4)
                {
                    struct iovec v_header;
                    v_header.iov_base = (char*)&pkt.net_len + off;
                    v_header.iov_len = 4 - off;
                    iov.push_back(v_header);
                    iov_count++;
                }
                //payload

                size_t payload_off = (off < 4) ? 0 : (off - 4);
                if(payload_off < pkt.payload->size())
                {
                    struct iovec v_payload;
                    v_payload.iov_base = pkt.payload->data() + payload_off;
                    v_payload.iov_len = pkt.payload->size() - payload_off;
                    iov.push_back(v_payload);
                    iov_count++;
                }
            }

            if(iov.empty())
            {
                conn->outgoing_queue.pop_front();
                return;
            }

            //hather write: send multiple buffers in one syscall
            ssize_t n = writev(conn->fd(), iov.data(), iov.size());

            if(n < 0)
            {
                if(errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    close_conn(conn);
                    return;
                }
            }
            else
            {
                if((size_t) n <= conn->pending_bytes)
                {
                    conn->pending_bytes -= n;
                }
                else
                {
                    conn->pending_bytes = 0;
                }

                size_t bytes_sent = n;
                while(bytes_sent > 0 && !conn->outgoing_queue.empty())
                {
                    auto& pkt = conn->outgoing_queue.front();
                    size_t pkt_total_size = 4 + pkt.payload->size();
                    size_t remaining_in_pkt = pkt_total_size - pkt.sent_offset;

                    if(bytes_sent >= remaining_in_pkt)
                    {
                        bytes_sent -= remaining_in_pkt;
                        conn->outgoing_queue.pop_front();
                    }
                    else
                    {
                        pkt.sent_offset += bytes_sent;
                        break;
                    }
                }
            }
        }

        if(conn->is_reading_paused && conn->pending_bytes < LOW_WATER_MARK)
        {
            conn->is_reading_paused = false;
        }

        uint32_t final_events = 0;
        if(!conn->is_reading_paused && !conn->in_buf.full())
        {
            final_events |= EPOLLIN;
        }
        
        if(!conn->outgoing_queue.empty())
        {
            final_events |= EPOLLOUT;
            conn->is_write_armed = true;
        }
        else
        {
            conn->is_write_armed = false;
        }

        if(!should_close)
        {
            mod_fd_epoll(conn->fd(), final_events);
        }
    }

    if(should_close)
    {
        close_conn(conn);
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
        auto payload = std::make_shared<std::vector<char>>(leave_msg.begin(), leave_msg.end());
        threadpool_->push_task([this, payload]{
            this->handle_broadcast_task(nullptr, payload);
        });
    }

    // epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, conn->fd, nullptr);
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, conn->fd(), nullptr);
    // close(conn->fd);
    {
        std::unique_lock<std::shared_mutex> lk(conn_map_mtx_);
        connections_.erase(conn->fd());
    }
    LOG_INFO("Closed fd = " + std::to_string(conn->fd()));
}

void WebServer::mark_fd_for_writing(const ConnPtr& conn)
{
    //called by worker threads
    //acquires lock B (conn->mtx) then lock A (ready_to_write_mtx_)
    std::lock_guard<std::mutex> conn_lk(conn->mtx);
    if(!conn->needs_processing)
    {
        conn->needs_processing = true;
        std::lock_guard<std::mutex> lk(ready_to_write_mtx_);
        ready_to_write_fd_queue_.push(conn->fd());
    }
}

void WebServer::handle_pending_writes()
{
    std::queue<int> local_queue;

    {
        std::lock_guard<std::mutex> lk(ready_to_write_mtx_);
        local_queue.swap(ready_to_write_fd_queue_);
    }

    int clients_processed = 0;
    std::shared_lock<std::shared_mutex> map_lk(conn_map_mtx_);
    //lock global queue, process our micro-batch
    
    while(!local_queue.empty() && clients_processed < WRITE_BUDGET_PER_LOOP)
    {
        int fd = local_queue.front();
        local_queue.pop();
        clients_processed++;

        auto it = connections_.find(fd);
        if(it == connections_.end()) continue;

        ConnPtr conn = it->second;

        if(!conn || conn->closed.load())
        {
            continue;
        }

        {
            //lock the connection (lock B)
            std::lock_guard<std::mutex> conn_lk(conn->mtx);
            conn->needs_processing = false;

            if(!conn->outgoing_queue.empty() && !conn->is_write_armed)
            {
                uint32_t events = EPOLLOUT;
                if(!conn->is_reading_paused && !conn->in_buf.full())
                {
                    events |= EPOLLIN;
                }
                mod_fd_epoll(conn->fd(), events);
                conn->is_write_armed = true;
            }
        }
    }

    if(!local_queue.empty())
    {
        std::lock_guard<std::mutex> lk(ready_to_write_mtx_);
        while(!local_queue.empty())
        {
            ready_to_write_fd_queue_.push(local_queue.front());
            local_queue.pop();
        }
    }
}


void WebServer::run()
{
    LOG_INFO("Chat server listening on port " + std::to_string(port_) + "...");    
    struct epoll_event events[MAX_EVENTS];

    while(running_)
    {
        int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if(nfds == -1)
        {
            if(errno == EINTR) continue;
            LOG_ERROR("epoll_wait failed: " + std::string(strerror(errno)));
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
                LOG_INFO("Signal received, shutting down...");
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
        std::unique_lock<std::shared_mutex> lk(conn_map_mtx_);
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
    LOG_INFO("Server stopped and resources cleaned up!");
}
