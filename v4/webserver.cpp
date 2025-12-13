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
#include <typeindex>
#include <unordered_map>

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

#define BATCH_FLUSH_THRESHOLD 4096


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
    ts.it_value.tv_sec = 0;
    ts.it_value.tv_nsec = 10 * 1000000;
    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 10 * 1000000;

    if(timerfd_settime(timer_fd_, 0, &ts, nullptr) == -1)
    {
        LOG_ERROR("timerfd_settime: " + std::string(strerror(errno)));
        exit(1);
    }

    add_fd_to_epoll(timer_fd_, EPOLLIN);
}

WebServer::WebServer(int port, int n_workers) : 
    shards_(NUM_SHARDS), port_(port), n_workers_(n_workers), running_(true)
{
    setup_signalfd();
    setup_server_socket();
    setup_epoll();
    setup_eventfd();
    setup_timerfd();
    threadpool_ = std::make_unique<ThreadPool>(n_workers, notify_fd_);
}

WebServer::~WebServer(){
    Stop();
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

bool WebServer::attempt_write(ConnPtr conn)
{
    if (conn->outgoing_queue.empty()) return false;

    while (!conn->outgoing_queue.empty()) {
        
        struct iovec iov[64];
        const int IOV_LIMIT = 64;
        int iov_count = 0;
        size_t bytes_to_write = 0;

        auto it = conn->outgoing_queue.begin();
        for (; it != conn->outgoing_queue.end() && iov_count < IOV_LIMIT; ++it) {
            OutgoingPacket& pkt = *it;
            size_t off = pkt.sent_offset;
            
            if (off < pkt.payload->size()) {
                iov[iov_count].iov_base = pkt.payload->data() + off;
                iov[iov_count].iov_len = pkt.payload->size() - off;
                bytes_to_write += iov[iov_count].iov_len;
                iov_count++;
            }
        }

        if (iov_count == 0) {
            conn->outgoing_queue.pop_front();
            continue;
        }

        ssize_t n = writev(conn->fd(), iov, iov_count);

        if (n > 0) {
            METRICS.on_bytes_sent(n);
            
            if ((size_t)n <= conn->pending_bytes) {
                conn->pending_bytes -= n;
            } else {
                conn->pending_bytes = 0;
            }

            size_t bytes_left = n;
            while (bytes_left > 0 && !conn->outgoing_queue.empty()) {
                auto& pkt = conn->outgoing_queue.front();
                size_t pkt_total_size = pkt.payload->size();
                size_t remaining_in_pkt = pkt_total_size - pkt.sent_offset;

                if (bytes_left >= remaining_in_pkt) {
                    bytes_left -= remaining_in_pkt;
                    conn->outgoing_queue.pop_front();
                    METRICS.on_message_sent_count();
                } else {
                    pkt.sent_offset += bytes_left;
                    bytes_left = 0;
                    break;
                }
            }

        } 
        else if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false; 
            } else {
                LOG_ERROR("Write error on fd " + std::to_string(conn->fd()) + ": " + std::string(strerror(errno)));
                return true; 
            }
        }
    }

    if (conn->is_reading_paused && conn->pending_bytes <= LOW_WATER_MARK) {
        conn->is_reading_paused = false;
    }

    return false;
}

void WebServer::SendPreEncoded(ConnPtr conn, std::shared_ptr<std::vector<char>> packet)
{
    std::lock_guard<std::mutex> lk(conn->mtx);
    if(conn->closed) return;

    bool was_empty = (conn->outgoing_queue.empty() && !conn->is_write_armed);


    OutgoingPacket pkt;
    pkt.payload = packet;

    conn->outgoing_queue.push_back(pkt);
    conn->pending_bytes += pkt.payload->size();

    //watermark check
    if(!conn->is_reading_paused && conn->pending_bytes >= HIGH_WATER_MARK)
    {
        conn->is_reading_paused = true;
    }

    if(conn->pending_bytes > BATCH_FLUSH_THRESHOLD || conn->is_write_armed || was_empty)
    {
        attempt_write(conn);
        uint32_t events = conn->current_epoll_events;
        if(!conn->outgoing_queue.empty())
        {
            if(!conn->is_write_armed)
            {
                events |= EPOLLOUT;
                conn->is_write_armed = true;
            }
        }
        else
        {
            if(conn->is_write_armed)
            {
                events &= ~EPOLLOUT;
                conn->is_write_armed = false;
            }
        }
        update_epoll_events(conn, events);
    }

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

//timer callback to flush all queues
void WebServer::flush_all_queues()
{
    for(size_t i = 0; i < shards_.size(); ++i)
    {
        std::vector<ConnPtr> active_conns_in_shard;
        {
            std::shared_lock<std::shared_mutex> lk(shards_[i].mtx);
            for(const auto& pair : shards_[i].connections)
            {
                ConnPtr conn = pair.second;
                if(!conn || conn->closed.load()) continue;
                if(!conn->outgoing_queue.empty())
                {
                    active_conns_in_shard.push_back(conn);
                }
            }
        }

        for(const auto& conn : active_conns_in_shard)
        {
            if(conn->closed.load()) continue;

            std::unique_lock<std::mutex> lk(conn->mtx, std::try_to_lock);
            if(!lk.owns_lock()) continue;

            if(conn->closed.load() || conn->outgoing_queue.empty()) continue;
            attempt_write(conn);
            if(!conn->outgoing_queue.empty() && !conn->is_write_armed)
            {
                update_epoll_events(conn, conn->current_epoll_events | EPOLLOUT);
                conn->is_write_armed = true;
            }
        }
    }
}

void WebServer::process_global_queue()
{
    std::deque<BroadcastEvent> events;
    {
        std::lock_guard<std::mutex> lk(global_queue_mtx_);
        if(global_queue_.empty()) return;
        events.swap(global_queue_);
    }

    std::vector<std::function<void()>> batch_tasks;
    batch_tasks.reserve(shards_.size());

    for(size_t i = 0; i < shards_.size(); ++i)
    {
        auto events_copy = std::make_shared<std::deque<BroadcastEvent>>(events);

        batch_tasks.emplace_back([this, i, events_copy]()
        {
            std::vector<ConnPtr> shard_active_conns;
            {
                std::shared_lock<std::shared_mutex> lk(shards_[i].mtx);
                shard_active_conns.reserve(shards_[i].connections.size());

                for(const auto& pair : shards_[i].connections)
                {
                    if(pair.second && !pair.second->closed.load())
                    {
                        shard_active_conns.push_back(pair.second);
                    }
                }
            }

            if(shard_active_conns.empty()) return;

            for(const auto& conn : shard_active_conns)
            {
                std::lock_guard<std::mutex> conn_lk(conn->mtx);
                if(conn->closed.load()) continue;

                
                if(!conn->codec) conn->codec = std::make_unique<PacketCodec>();
                
                bool added_any = false;
                for(const auto& evt : *events_copy)
                {
                    if(evt.sender_fd != -1 && conn->fd() == evt.sender_fd) continue;

                    if(conn->outgoing_queue.size() >= MAX_OUTGOING_QUEUE_SIZE) 
                    {
                        //log once per burst to avoid log spamming
                         if (conn->outgoing_queue.size() == MAX_OUTGOING_QUEUE_SIZE) 
                         {
                             LOG_ERROR("Dropping broadcast to fd " + std::to_string(conn->fd()));
                             METRICS.on_message_dropped();
                         }
                        break;
                    }

                    OutgoingPacket pkt;
                    pkt.payload = evt.payload;
                    pkt.sent_offset = 0;

                    conn->outgoing_queue.push_back(std::move(pkt));
                    conn->pending_bytes += evt.payload->size();
                    added_any = true;   
                }
                
                if(added_any) 
                {
                    if(!conn->is_reading_paused && conn->pending_bytes >= HIGH_WATER_MARK)
                    {
                        conn->is_reading_paused = true;
                    }

                    attempt_write(conn);
                    
                    if(!conn->outgoing_queue.empty() && !conn->is_write_armed)
                    {
                        uint32_t events = conn->current_epoll_events | EPOLLOUT;
                        this->update_epoll_events(conn, events);
                        conn->is_write_armed = true;
                    }
                }
            }
        });
    }
    if(!batch_tasks.empty())
    {
        threadpool_->push_batch(batch_tasks);
    }
}

void WebServer::Broadcast(const std::vector<char>& data, int exclude_fd)
{
    {
        std::lock_guard<std::mutex> lk(global_queue_mtx_);

        BroadcastEvent evt;
        evt.sender_fd = exclude_fd;
        evt.payload = std::make_shared<std::vector<char>>(data);
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
        size_t shard_idx = get_shard_idx(client_fd);
        {
            std::lock_guard<std::shared_mutex> lk(shards_[shard_idx].mtx);
            // connections_[client_fd] = conn;
            shards_[shard_idx].connections[client_fd] = conn;

            //add initial timer event (current time + timeout)
            auto expiry = std::chrono::steady_clock::now() + std::chrono::seconds(CONNECTION_TIMEOUT_SEC);
            shards_[shard_idx].timer_heap.push({expiry, client_fd});
        }

        add_fd_to_epoll(conn->fd(), EPOLLIN);
        conn->current_epoll_events = EPOLLIN;
        METRICS.on_connection_accepted();

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

        LOG_INFO("Accepted "+ std::string(ip) + ":" + std::to_string(ntohs(client_addr.sin_port))
            + " fd = " + std::to_string(client_fd) + " [Shard " + std::to_string(shard_idx) + "]");;

        event_queue_.push({EventType::CONNECT, conn, {}});
    }
}

void WebServer::handle_read(ConnPtr conn)
{
    bool should_close = false;
    {
        std::lock_guard<std::mutex> lk(conn->mtx);
        while(true) 
        {
            auto iovecs = conn->in_buf.get_writeable_iovecs();

            //buffer is full
            if(iovecs.empty())
            {
                LOG_ERROR("Buffer full. Closing fd: " + std::to_string(conn->fd()));
                should_close = true;
                break;
            }

            ssize_t n = readv(conn->fd(), iovecs.data(), iovecs.size());
            
            if(n > 0)
            {
                conn->in_buf.commit_write(n);
                conn->update_activity();
                METRICS.on_bytes_received(n);

                //default codec if missing
                if(!conn->codec)
                {
                    // conn->codec = std::make_unique<LengthPrefixedCodec>();
                    conn->codec = std::make_unique<PacketCodec>();
                }
                
                //loop through messages using the codec
                while(true)
                {
                    //delegate to codec
                    auto msg_opt = conn->codec->decode(conn->in_buf);

                    if(!msg_opt.has_value())
                    {
                        break;
                    }
                    
                    auto payload_ptr = buffer_pool_.acquire_shared();
                    *payload_ptr = std::move(*msg_opt);
                    METRICS.on_message_received();
                    // event_queue_.push({EventType::MESSAGE, conn, std::move(*msg_opt)});
                    event_queue_.push({EventType::MESSAGE, conn, payload_ptr});
                }
            }
            else if(n == 0)
            {
                should_close = true;
                break;
            }
            else
            {
                if(errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break;
                }
                else
                {
                    should_close = true;
                    break;
                }
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

    event_queue_.push({EventType::DISCONNECT, conn, {}});

    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, conn->fd(), nullptr);
    size_t shard_idx = get_shard_idx(conn->fd());
    {
        std::unique_lock<std::shared_mutex> lk(shards_[shard_idx].mtx);
        shards_[shard_idx].connections.erase(conn->fd());
    }
    LOG_INFO("Closed fd = " + std::to_string(conn->fd()));
}

//idle sweeper
void WebServer::check_timeouts()
{
    auto now = std::chrono::steady_clock::now();
    std::vector<ConnPtr> timed_out;

    //iterate all shards
    for(size_t i = 0; i < shards_.size(); ++i)
    {
        std::unique_lock<std::shared_mutex> lk(shards_[i].mtx);
        auto& heap = shards_[i].timer_heap;

        while(!heap.empty())
        {
            const auto& event = heap.top();
            if(event.expires_at > now) break;

            int fd = event.fd;
            heap.pop();

            auto it = shards_[i].connections.find(fd);
            if(it == shards_[i].connections.end()) continue;

            ConnPtr conn = it->second;
            auto last_active = conn->last_activity;
            auto expected_expiry = last_active + std::chrono::seconds(CONNECTION_TIMEOUT_SEC);
            if(expected_expiry > now)
            {
                heap.push({expected_expiry, fd});
            }
            else
            {
                timed_out.push_back(conn);
            }
        }
    }

    for(auto& conn : timed_out)
    {
        METRICS.on_timeout();
        close_conn(conn);
    }
}

void WebServer::Close(ConnPtr conn)
{
    close_conn(conn);
}

void WebServer::Run()
{
    LOG_INFO("Server listening on port " + std::to_string(port_) + "...");    
    struct epoll_event events[MAX_EVENTS];
    static uint64_t ticks = 0;
    static uint64_t stats_timer_accumulator = 0;

    while(running_)
    {
        int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, 100);
        bool pending_broadcast = false;


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
                
                flush_all_queues();
                stats_timer_accumulator += expirations;
                ticks += expirations;

                if(stats_timer_accumulator >= 6000)
                {
                    check_timeouts();
                    METRICS.print_stats();
                    stats_timer_accumulator = 0;
                }
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
                pending_broadcast = true;
                uint64_t cnt;
                read(notify_fd_, &cnt, sizeof(cnt));
            }
            else
            {
                ConnPtr conn;

                size_t shard_idx = get_shard_idx(fd);
                {
                    std::shared_lock<std::shared_mutex> lk(shards_[shard_idx].mtx);
                    auto it = shards_[shard_idx].connections.find(fd);
                    if(it != shards_[shard_idx].connections.end())
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

        if(pending_broadcast) 
        {
            process_global_queue();
        }
    }

    Stop();
}

void WebServer::Stop()
{
    running_ = false;
    if(threadpool_)
    {
        threadpool_->shutdown();
    }

    for(auto& shard : shards_)
    {
        std::unique_lock<std::shared_mutex> lk(shard.mtx);
        shard.connections.clear();
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
