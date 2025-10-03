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

//constructor
WebServer::WebServer(int port, int n_workers) : 
    port_(port), n_workers_(n_workers), running_(true)
{
    setup_signalfd();
    setup_server_socket();
    setup_epoll();
    setup_eventfd();
    threadpool_ = std::make_unique<ThreadPool>(n_workers_, notify_fd_, *this);
}

//destructor
WebServer::~WebServer(){
    stop();
}

void WebServer::queue_broadcast_message(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(broadcast_mtx_);
    broadcast_queue_.push(msg);
}

void WebServer::handle_broadcasts(){
    std::vector<std::string> messages_to_send;
    {
        std::lock_guard<std::mutex> lk(broadcast_mtx_);
        while(!broadcast_queue_.empty())
        {
            messages_to_send.push_back(std::move(broadcast_queue_.front()));
            broadcast_queue_.pop();
        }
    }

    if(messages_to_send.empty()) return;

    std::lock_guard<std::mutex> lk(conn_map_mtx_);
    for(const auto& msg : messages_to_send)
    {
        for(auto& pair : connections_)
        {
            ConnPtr conn = pair.second;
            if(conn && !conn->closed.load())
            {
                std::lock_guard<std::mutex> conn_lk(conn->mtx);
                conn->out_buf += msg;
                mod_fd_epoll(conn->fd, EPOLLIN | EPOLLOUT);
            }
        }
    }
}

//handle signals without using a global signal handler
void WebServer::setup_signalfd()
{
    //represent the set of signals to be handled
    sigset_t mask;
    //initialize the signal set pointed by mask to empty
    sigemptyset(&mask);
    //add specific signals we want to handleto the set
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    //block the signals in the set for the entire process
    //so they can be handled via signalfd
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    //create a file descriptor for signal handling
    //make it non-blocking and close-on-exec
    //this fd will be monitored in the epoll loop
    sig_fd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if(sig_fd_ == -1)
    {
        perror("signalfd");
        exit(1);
    }
}

void WebServer::setup_server_socket()
{
    //creating the socket
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd_ < 0)
    {
        perror("Socket creation failed!!");
        exit(1);
    }
    
    // This option allows the socket to be bound to the same address and port
    // even if it's in a TIME_WAIT state, preventing "Address already in use" errors.
    int opt = 1;
    if(setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        perror("Setsockopt failed!!");
        exit(1);
    }

    sockaddr_in server_addr{};
    //configuring the server address
    //IPv4, need to match the socket type AF_INET
    server_addr.sin_family = AF_INET;
    //Accepts connections on all local IP addresses
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    //set the port number of the server
    server_addr.sin_port = htons(port_);

    //binding socket to address
    if(bind(listen_fd_, (sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed!!");
        exit(1);
    }

    //convert active socket into a passive listening socket
    if(listen(listen_fd_, BACKLOG) < 0)
    {
        perror("Listen failed!!");
        exit(1);
    }
    //set the listening socket to non-blocking mode
    set_nonblocking(listen_fd_);
}

void WebServer::setup_epoll()
{
    //create an epoll instance
    epoll_fd_ = epoll_create1(0);
    if(epoll_fd_ == -1)
    {
        perror("epoll_create1");
        exit(1);
    }
    //tell epoll to monitor the listening socket for incoming connections
    add_fd_to_epoll(listen_fd_, EPOLLIN);
    //tell epoll to monitor the signal fd for incoming signals
    add_fd_to_epoll(sig_fd_, EPOLLIN);
}

void WebServer::setup_eventfd()
{
    //create a file descriptor for event notifications
    //also make it non-blocking and close-on-exec
    notify_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if(notify_fd_ == -1)
    {
        perror("eventfd");
        exit(1);
    }
    //add the eventfd to the epoll instance
    add_fd_to_epoll(notify_fd_, EPOLLIN);
}

//set a fd to non-blocking mode
void WebServer::set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1)
    {
        flags = 0;
    }
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

//add a fd to the epoll instance with specified events
void WebServer::add_fd_to_epoll(int fd, uint32_t events)
{
    epoll_event ev{};
    ev.data.fd = fd;
    //set the epoll event to edge-triggered mode
    ev.events = events | EPOLLET;
    //add the fd to epoll watchlist
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
}

//modify the settings for a fd that is already in the epoll instance 
//with specified events
void WebServer::mod_fd_epoll(int fd, uint32_t events)
{
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = events | EPOLLET;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

//accept all pending connections on the listening socket
//configures them for non-blocking I/O
//add them to the epoll instance for monitoring
void WebServer::accept_loop()
{
    while(true)
    {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, (sockaddr*)&client_addr, &client_len);
        if(client_fd == -1)
        {
            //no more pending connections to accept right now
            //accept loop can exit
            if(errno == EAGAIN || errno == EWOULDBLOCK) break;
            else
            {
                //something else went wrong
                perror("Accept failed!!");
                break;
            }
        }
        
        //set the client socket fd to non-blocking mode
        set_nonblocking(client_fd);
        ConnPtr conn = std::make_shared<Connection>(client_fd);
        {
            std::lock_guard<std::mutex> lk(conn_map_mtx_);
            connections_[client_fd] = conn;
        }

        //add the new client socket to the epoll instance
        //EPOLLIN to monitor for incoming data
        //also we set it to edge-triggered mode in add_fd_to_epoll
        add_fd_to_epoll(client_fd, EPOLLIN);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        std::cout<<"Accepted "<<ip<<":"<<ntohs(client_addr.sin_port)
            <<" fd = "<<client_fd<<"\n";

        //broadcast when a client has joined
        std::string join_msg = "[Server]: Client "+std::to_string(client_fd)+" has joined the chatroom.\n";
        queue_broadcast_message(join_msg);

        uint64_t one = 1;
        write(notify_fd_, &one, sizeof(one));
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
            size_t pos;
            while((pos = conn->in_buf.find('\n')) != std::string::npos)
            {
                std::string line = conn->in_buf.substr(0, pos+1);
                conn->in_buf.erase(0, pos+1);
                threadpool_->push_task(conn, line);
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
    std::lock_guard<std::mutex> lk(conn->mtx);
    while(!conn->out_buf.empty())
    {
        ssize_t n = send(conn->fd, conn->out_buf.data(), conn->out_buf.size(), MSG_NOSIGNAL);
        if(n > 0)
        {
            conn->out_buf.erase(0, n);
        }
        else if(n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            mod_fd_epoll(conn->fd, EPOLLIN | EPOLLOUT);
            return;
        }
        else
        {
            close_conn(conn);
            return;
        }
    }
    mod_fd_epoll(conn->fd, EPOLLIN);
}

void WebServer::close_conn(ConnPtr conn)
{
    if(conn->closed.exchange(true)) return;

    //broadcast that a client has left
    std::string leave_msg = "[Server]: Client "+std::to_string(conn->fd)+" has left the chatroom.\n";

    queue_broadcast_message(leave_msg);
    uint64_t one = 1;
    write(notify_fd_, &one, sizeof(one));

    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, conn->fd, nullptr);
    close(conn->fd);
    {
        std::lock_guard<std::mutex> lk(conn_map_mtx_);
        connections_.erase(conn->fd);
    }

    std::cout<<"Closed fd = "<<conn->fd<<"\n";
}

void WebServer::handle_pending_writes()
{
    //lock the connection map while checking for pending writes
    std::lock_guard<std::mutex> lk(conn_map_mtx_);
    //loop through all active connections
    //find those with pending data in their out_buf
    for(auto& c : connections_)
    {
        auto& conn = c.second;
        std::lock_guard<std::mutex> lk2(conn->mtx);
        if(!conn->out_buf.empty())
        {
            //also watch for EPOLLOUT events because we have data to send
            mod_fd_epoll(conn->fd, EPOLLIN | EPOLLOUT);
        }
    }
}

//main event loop
//repeatedly calls epoll_wait, dispatches events to handlers
void WebServer::run()
{
    std::cout<<"Chat server listening on port "<<port_<<"... :)\n";
    struct epoll_event events[MAX_EVENTS];

    while(running_)
    {
        //makes a single blocking call to wait for events on the monitored fds
        int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if(nfds == -1)
        {
            if(errno == EINTR) continue;

            perror("epoll_wait");
            break;
        }

        //when epoll_wait returns, iterate over the triggered events
        //and dispatch them to the appropriate handlers
        //afrer handling all events, loop back to epoll_wait
        for(int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;
            //detects an EPOLLIN event on the listening socket
            if(fd == listen_fd_)
            {
                accept_loop();
            }
            //detects an EPOLLIN event for graceful shutdown signals
            else if(fd == sig_fd_)
            {
                struct signalfd_siginfo si;
                read(sig_fd_, &si, sizeof(si));
                std::cout<<"Signal received, shutting down...\n";
                running_ = false;
                break;
            }
            //data is available in a connection's output buffer
            else if(fd == notify_fd_)
            {
                uint64_t cnt;
                read(notify_fd_, &cnt, sizeof(cnt));
                handle_broadcasts();
                //handle_pending_writes();
            }
            else
            {
                ConnPtr conn;
                {
                    std::lock_guard<std::mutex> lk(conn_map_mtx_);
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
    //set running_ to false to signal that the server should stop
    running_ = false;
    //if a threadpool object exists, call its shutdown method to stop all worker threads
    if(threadpool_)
    {
        threadpool_->shutdown();
    }

    {
        std::lock_guard<std::mutex> lk(conn_map_mtx_);
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
