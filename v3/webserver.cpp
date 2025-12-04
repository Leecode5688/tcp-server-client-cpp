#include "webserver.h"
#include "utils.h"
#include "metrics.h"
#include <iostream>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/signalfd.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
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
//10 minute timeout
#define CONNECTION_TIMEOUT_SEC 600

//limit for global queue
#define MAX_GLOBAL_QUEUE_SIZE 10000
//max events to process per epoll loop
#define GLOBAL_BATCH_SIZE 100

//helper function to create a paclet with network header
OutgoingPacket make_packet(const std::vector<char>& full_msg)
{
    OutgoingPacket pkt;
    pkt.payload = std::make_shared<std::vector<char>>(full_msg.begin(), full_msg.end());
    pkt.net_len = htonl(pkt.payload->size());
    return pkt;
}


//setup functions
void WebServer::setup_timerfd()
{
    timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if(timer_fd_ == -1)
    {
        LOG_ERROR("timerfd_create: " + std::string(strerror(errno)));
        exit(1);
    }

    struct itimerspec ts;
    ts.it_value.tv_sec = 60;
    ts.it_value.tv_nsec = 0;
    ts.it_interval.tv_sec = 60;
    ts.it_interval.tv_nsec = 0;

    if(timerfd_settime(timer_fd_, 0, &ts, nullptr) == -1)
    {
        LOG_ERROR("timerfd_settime: " + std::string(strerror(errno)));
        exit(1);
    }

    add_fd_to_epoll(timer_fd_, EPOLLIN);
}

WebServer::WebServer(int port, int n_workers) : 
    port_(port), n_workers_(n_workers), running_(true)
{
    setup_signalfd();
    setup_server_socket();
    setup_epoll();
    setup_eventfd();
    setup_timerfd();
    threadpool_ = std::make_unique<ThreadPool>(n_workers, notify_fd_);
}

WebServer::~WebServer(){
    stop();
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

void WebServer::set_tcp_nodelay(int fd)
{
    int opt = 1;
    if(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0)
    {
        LOG_ERROR("setsocket TCP_NODELAY failed");
    }
}

void WebServer::setup_server_socket()
{
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd_ < 0)
    {
        LOG_ERROR("Socket creation failed: " + std::string(strerror(errno)));        
        exit(1);
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

void WebServer::update_epoll_events(ConnPtr conn, uint32_t new_events)
{
    if(conn->current_epoll_events == new_events) return;

    mod_fd_epoll(conn->fd(), new_events);
    conn->current_epoll_events = new_events;
}

//core logic

//write helper, optimistic write attempt
bool WebServer::attempt_write(ConnPtr conn)
{
    if(conn->outgoing_queue.empty()) return false;

    // std::vector<struct iovec> iov;
    struct iovec iov[64];
    const int IOV_LIMIT = 64;
    int iov_count = 0;
    auto it = conn->outgoing_queue.begin();

    for( ; it != conn->outgoing_queue.end() && iov_count < IOV_LIMIT; ++it)
    {
        OutgoingPacket& pkt = *it;
        size_t off = pkt.sent_offset;

        if(off < 4)
        {
            iov[iov_count].iov_base = (char*)&pkt.net_len + off;
            iov[iov_count].iov_len = 4 - off;
            iov_count++;
        }
        size_t payload_off = (off < 4) ? 0 : (off - 4);
        if(payload_off < pkt.payload->size())
        {
            iov[iov_count].iov_base = pkt.payload->data() + payload_off;
            iov[iov_count].iov_len = pkt.payload->size() - payload_off;
            iov_count++;
        }
    }
    if(iov_count == 0)
    {
        conn->outgoing_queue.pop_front();
        return false;
    }

    ssize_t n = writev(conn->fd(), iov, iov_count);

    if(n > 0)
    {
        METRICS.on_bytes_sent(n);    
    }

    if(n < 0)
    {
        if(errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return false;
        }
        return true;
    }

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
            METRICS.on_message_sent_count();
        }
        else
        {
            pkt.sent_offset += bytes_sent;
            break;
        }
    }

    if(conn->is_reading_paused && conn->pending_bytes <= LOW_WATER_MARK)
    {
        conn->is_reading_paused = false;
    }

    return false;
}

//centralized batched pub/sub processor
void WebServer::process_global_queue()
{
    std::deque<ChatEvent> local_batch;
    bool more_work_remains = false;
    {
        std::lock_guard<std::mutex> lk(global_queue_mtx_);
        for(int i = 0; i < GLOBAL_BATCH_SIZE && !global_queue_.empty(); ++i)
        {
            local_batch.push_back(std::move(global_queue_.front()));
            global_queue_.pop_front();
        }
        more_work_remains = !global_queue_.empty();
    }

    if(local_batch.empty()) return;

    std::vector<std::pair<int, OutgoingPacket>> ready_broadcasts;
    std::vector<int> flush_targets;

    ready_broadcasts.reserve(local_batch.size());

    for(const auto& evt : local_batch)
    {
        if(!evt.payload)
        {
            if(evt.sender_fd != -1)
            {
                flush_targets.push_back(evt.sender_fd);
                continue;
            }
        }

        std::vector<char> final_msg;
        if(evt.is_system_msg)
        {
            final_msg.insert(final_msg.end(), evt.payload->begin(), evt.payload->end());
        }
        else
        {
            std::string prefix = "[" + evt.username + "]: ";
            final_msg.reserve(prefix.size() + evt.payload->size());
            final_msg.insert(final_msg.end(), prefix.begin(), prefix.end());
            final_msg.insert(final_msg.end(), evt.payload->begin(), evt.payload->end());
        }

        ready_broadcasts.push_back({evt.sender_fd, make_packet(final_msg)});
    }

    //no broadcast, but do have specific targets like login errors
    if(ready_broadcasts.empty() && !flush_targets.empty())
    {
        std::shared_lock<std::shared_mutex> map_lock(conn_map_mtx_);
        for(int fd : flush_targets)
        {
            auto it = connections_.find(fd);
            if(it == connections_.end())
            {
                continue;
            }
            
            ConnPtr conn = it->second;
            std::lock_guard<std::mutex> conn_lk(conn->mtx);
            
            if(conn->closed.load()) continue;

            attempt_write(conn);

            uint32_t events = 0;
            if(!conn->outgoing_queue.empty())
            {
                events |= EPOLLOUT;
                conn->is_write_armed = true;
            }
            else
            {
                conn->is_write_armed = false;
            }

            if(!conn->is_reading_paused && !conn->in_buf.full())
            {
                events |= EPOLLIN;
            }
            update_epoll_events(conn, events);
        }
        if(more_work_remains)
        {
            uint64_t one = 1;
            write(notify_fd_, &one, sizeof(one));
        }
        return;
    }

    {   
        std::shared_lock<std::shared_mutex> map_lock(conn_map_mtx_);
        for(auto& pair : connections_)
        {
            ConnPtr conn = pair.second;
            if(!conn || conn->closed.load() || conn->state != ConnState::ACTIVE) continue;

            std::lock_guard<std::mutex> conn_lk(conn->mtx);

            //if EPOLLOUT armed, don't waste system calls
            //just queue the message
            if(conn->is_write_armed)
            {
                for(const auto& item : ready_broadcasts)
                {
                    if(item.first != -1 && item.first == conn->fd()) continue;

                    if(conn->outgoing_queue.size() >= MAX_OUTGOING_QUEUE_SIZE)
                    {
                        METRICS.on_message_dropped();
                        continue;
                    }
                    conn->outgoing_queue.push_back(item.second);
                    conn->pending_bytes += (4 + item.second.payload->size());
                }
                continue;
            }
            
            //normal path, sockets are believed to be writable
            if(!ready_broadcasts.empty())
            {
                for(const auto& item : ready_broadcasts)
                {
                    int sender_fd = item.first;
                    const OutgoingPacket& pkt = item.second;

                    if(sender_fd != -1 && sender_fd == conn->fd()) continue;

                    if(conn->outgoing_queue.size() >= MAX_OUTGOING_QUEUE_SIZE)
                    {
                        METRICS.on_message_dropped();
                        continue;
                    }

                    conn->outgoing_queue.push_back(pkt);
                    conn->pending_bytes += (4 + pkt.payload->size());
                }
            }

            //flow control check
            if(!conn->is_reading_paused && conn->pending_bytes >= HIGH_WATER_MARK)
            {
                conn->is_reading_paused = true;
            }

            bool error = attempt_write(conn);
            if(error) continue;

            uint32_t events = 0;
            if(!conn->outgoing_queue.empty())
            {
                events |= EPOLLOUT;
                conn->is_write_armed = true;
            }
            else
            {
                conn->is_write_armed = false;
            }

            if(!conn->is_reading_paused && !conn->in_buf.full())
            {
                events |= EPOLLIN;
            }

            // mod_fd_epoll(conn->fd(), events);
            update_epoll_events(conn, events);
        }
    }

    if(more_work_remains)
    {
        uint64_t one = 1;
        write(notify_fd_, &one, sizeof(one));
    }
}

void WebServer::publish_to_global_queue(int sender_fd, std::string username, std::shared_ptr<std::vector<char>> payload, bool is_system)
{
    {
        std::lock_guard<std::mutex> lk(global_queue_mtx_);

        if(global_queue_.size() >= MAX_GLOBAL_QUEUE_SIZE)
        {
            METRICS.on_message_dropped();
            return;
        }

        ChatEvent evt;
        evt.sender_fd = sender_fd;
        evt.username = username;
        evt.payload = payload;
        evt.is_system_msg = is_system;
        global_queue_.push_back(evt);
    }

    uint64_t one = 1;
    write(notify_fd_, &one, sizeof(one));
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
                LOG_ERROR("Accept failed: " + std::string(strerror(errno)));
                break;
            }
        }

        set_nonblocking(client_fd);
        set_tcp_nodelay(client_fd);

        ConnPtr conn = std::make_shared<Connection>(client_fd);
        {
            std::lock_guard<std::shared_mutex> lk(conn_map_mtx_);
            connections_[client_fd] = conn;
        }

        add_fd_to_epoll(conn->fd(), EPOLLIN);

        conn->current_epoll_events = EPOLLIN;

        METRICS.on_connection_accepted();

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

        LOG_INFO("Accepted "+ std::string(ip) + ":" + std::to_string(ntohs(client_addr.sin_port))
            + " fd = " + std::to_string(client_fd));

        std::string welcomeMessage = "[Server]: Welcome! Please enter your username: ";

        auto payload = std::make_shared<std::vector<char>>(welcomeMessage.begin(), welcomeMessage.end());

        OutgoingPacket pkt = make_packet(*payload);
        {
            std::lock_guard<std::mutex> conn_lk(conn->mtx);
            conn->outgoing_queue.push_back(pkt);
            conn->pending_bytes += (4 + payload->size());
            attempt_write(conn);
            uint32_t ev = EPOLLIN;
            if(!conn->outgoing_queue.empty())
            {
                // mod_fd_epoll(conn->fd(), EPOLLIN | EPOLLOUT);
                ev |= EPOLLOUT;
                conn->is_write_armed = true;
                update_epoll_events(conn, ev);
            }
        }
    }
}

void WebServer::handle_login_task(ConnPtr conn, std::string username)
{
    if(conn->closed.load()) return;

    std::lock_guard<std::mutex> conn_lk(conn->mtx);

    if(conn->closed.load()) return;

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
        conn->state = ConnState::ACTIVE;
    
        response_msg = "[Server]: Username accepted! You have entered the chat!\n";
        needs_broadcast = true;
    }
    else
    {
        response_msg = "[Server]: Username is invalid or already used. Please try another one: ";    
    }

    auto payload = std::make_shared<std::vector<char>>(response_msg.begin(), response_msg.end());
    OutgoingPacket pkt = make_packet(*payload);


    conn->outgoing_queue.push_back(pkt);
    conn->pending_bytes += (4 + pkt.payload->size());


    if(needs_broadcast)
    {
        std::string joinMessage = "[Server]: " + username + " has joined the chatroom!\n";    
        auto payload_ptr = std::make_shared<std::vector<char>>(joinMessage.begin(), joinMessage.end());
        publish_to_global_queue(-1, "", payload_ptr, true);
    }
    else
    {
        publish_to_global_queue(conn->fd(), "", nullptr, true);
    }

}

void WebServer::handle_read(ConnPtr conn)
{
    bool should_close = false;
    {
        std::lock_guard<std::mutex> lk(conn->mtx);
        auto iovecs = conn->in_buf.get_writeable_iovecs();
        if(iovecs.empty())
        {
            // mod_fd_epoll(conn->fd(), EPOLLOUT);
            update_epoll_events(conn, EPOLLOUT);
            return;
        }

        ssize_t n = readv(conn->fd(), iovecs.data(), iovecs.size());

        if(n > 0)
        {
            conn->in_buf.commit_write(n);
            METRICS.on_bytes_received(n);

            while(true)
            {
                if(conn->in_buf.size() < sizeof(uint32_t))
                {
                    break;
                }

                char header[4];
                conn->in_buf.peek(header, 4);
                uint32_t net_len;
                memcpy(&net_len, header, sizeof(uint32_t));
                uint32_t payload_len = ntohl(net_len);
                if(payload_len > MAX_PAYLOAD_SIZE)
                {
                    should_close = true;
                    break;
                }

                if(conn->in_buf.size() < sizeof(uint32_t) + payload_len) 
                {
                    break;
                }

                conn->in_buf.consume(sizeof(uint32_t));
                auto payload_ptr = std::make_shared<std::vector<char>>(payload_len);

                conn->in_buf.peek(payload_ptr->data(), payload_len);
                conn->in_buf.consume(payload_len);
                METRICS.on_message_received();

                if(conn->state == ConnState::AWAITING_USERNAME)
                {
                    std::string username(payload_ptr->begin(), payload_ptr->end());
                    threadpool_->push_task([this, conn, username]
                    {
                        this->handle_login_task(conn, username);
                    });
                }
                else if(conn->state == ConnState::ACTIVE)
                {
                    std::string usr_name = conn->username;
                    int s_fd = conn->fd();
                    threadpool_->push_task([this, s_fd, usr_name, payload_ptr]
                    {
                        this->publish_to_global_queue(s_fd, usr_name, payload_ptr);
                    });
                }
            }
        }
        else if(n == 0)
        {
            should_close = true;
        }
        else
        {
            if(errno != EAGAIN && errno != EWOULDBLOCK)
            {
                should_close = true;
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
        should_close = attempt_write(conn);

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
            // mod_fd_epoll(conn->fd(), final_events);
            update_epoll_events(conn, final_events);
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

    METRICS.on_connection_closed();

    std::string leave_msg;
    std::string captured_username;

    {
        std::lock_guard<std::mutex> conn_lk(conn->mtx);
        captured_username = conn->username;
    }

    if(!captured_username.empty())
    {
        leave_msg = "[Server]: "+ captured_username + " has left the chatroom.";
        std::unique_lock<std::shared_mutex> user_name_lk(usernames_mtx_);
        usernames_.erase(captured_username);
    }

    if(!leave_msg.empty())
    {
        auto payload = std::make_shared<std::vector<char>>(leave_msg.begin(), leave_msg.end());
        publish_to_global_queue(-1, "", payload, true);
    }

    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, conn->fd(), nullptr);
    // close(conn->fd());
    {
        std::unique_lock<std::shared_mutex> lk(conn_map_mtx_);
        connections_.erase(conn->fd());
    }
    LOG_INFO("Closed fd = " + std::to_string(conn->fd()));
}

//idle sweeper
void WebServer::check_timeouts()
{
    auto now = std::chrono::steady_clock::now();
    std::vector<ConnPtr> timed_out;
    {
        std::shared_lock<std::shared_mutex> lk(conn_map_mtx_);
        for(auto& pair : connections_)
        {
            auto diff = now - pair.second->last_activity;
            if(std::chrono::duration_cast<std::chrono::seconds>(diff).count() > CONNECTION_TIMEOUT_SEC)
            {
                timed_out.push_back(pair.second);
            }
        }
    }

    for(auto& conn : timed_out)
    {
        METRICS.on_timeout();
        close_conn(conn);
    }
}

void WebServer::run()
{
    LOG_INFO("Chat server listening on port " + std::to_string(port_) + "...");    
    struct epoll_event events[MAX_EVENTS];

    while(running_)
    {
        //wait 1000ms max to allow periodic tasks
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
            else if(fd == timer_fd_)
            {
                uint64_t expirations;
                read(timer_fd_, &expirations, sizeof(expirations));

                check_timeouts();
                METRICS.print_stats();
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
                // handle_pending_writes();
                process_global_queue();
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
    if(timer_fd_ >= 0)
    {
        close(timer_fd_);
    }
    LOG_INFO("Server stopped and resources cleaned up!");
}
