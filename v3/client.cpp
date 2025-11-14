#include "client.h"
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <cstring>

#define BUFFER_SIZE 4096
#define MAX_EVENTS 2

struct termios Client::orig_termios_;

Client::Client(const std::string& ip, int port) : server_ip_(ip), port_(port) 
{
    enable_raw_mode();   
    setup_signalfd();
}

Client::~Client() 
{
    if(sock_fd_ != -1) close(sock_fd_);
    if(epoll_fd_ != -1) close(epoll_fd_);
    if(sig_fd_ != -1) close(sig_fd_);
}

bool Client::connect_to_server() 
{
    sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(sock_fd_ < 0)
    {
        perror("Socket creation failed!!");
        return false;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_);

    if(inet_pton(AF_INET, server_ip_.c_str(), &server_addr.sin_addr) <= 0)
    {
        perror("Invalid address/ Address not supported!!");
        close(sock_fd_);
        return false;
    }

    if(connect(sock_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Connection failed!!");
        close(sock_fd_);
        return false;
    }

    return true;
}

bool Client::login()
{
    bool login_success = false;
    while(!login_success && running_)
    {
        char buffer[BUFFER_SIZE];
        ssize_t n = recv(sock_fd_, buffer, sizeof(buffer) - 1, 0);
        if(n <= 0)
        {
            std::cerr<<"\nConncetion closed during login."<<std::endl;
            return false;
        }
        buffer[n] = '\0';
        
        //v3
        in_buf_.append(buffer, n);
        std::string server_msg_payload;

        while(true)
        {
            if(in_buf_.size() < sizeof(uint32_t))
            {
                // Not enough data for a header yet; try receiving more
                break;
            }

            uint32_t net_len;
            memcpy(&net_len, in_buf_.data(), sizeof(uint32_t));
            uint32_t payload_len = ntohl(net_len);

            if(in_buf_.size() < (sizeof(uint32_t) + payload_len))
            {
                // Not enough data for full payload yet; wait for more
                break;
            }

            server_msg_payload = in_buf_.substr(sizeof(uint32_t), payload_len);
            in_buf_.erase(0, sizeof(uint32_t) + payload_len);

            // We successfully parsed one server message
            break;
        }

        if(server_msg_payload.empty())
        {
            // We didn't get a full message yet; continue receiving
            continue;
        }
        std::cout << "\r\033[K" << server_msg_payload << std::flush;
        
        if(server_msg_payload.find("Username accepted!") != std::string::npos)
        {
            login_success = true;
        }
        else
        {
            username_.clear(); 
            char c;
            while(running_ && read(STDIN_FILENO, &c, 1) == 1 && c != '\n' && c != '\r')
            {
                if(isprint(c))
                {
                    username_ += c;
                    std::cout<<c<<std::flush;
                }
                else if((c == 127 || c == 8) && !username_.empty())
                {
                    username_.pop_back();
                    std::cout<<"\b \b"<<std::flush;
                }
            }

            std::cout<<std::endl;
            
            //v3
            uint32_t len = username_.size();
            uint32_t net_len = htonl(len);

            if(send(sock_fd_, &net_len, sizeof(uint32_t), MSG_NOSIGNAL) < 0)
            {
                perror("Failed to send username header!!");
                return false;
            }
            if(send(sock_fd_, username_.c_str(), len, MSG_NOSIGNAL) < 0)
            {
                perror("Failed to send username payload!!");
                return false;
            }
        }

        if(login_success)
        {
            prompt_ = "[" + username_ + "]: ";
        }

    }
    return login_success;
}

void Client::run()
{
    set_nonblocking(sock_fd_);
    set_nonblocking(STDIN_FILENO);

    setup_epoll();

    std::cout<<prompt_<<std::flush;
    epoll_event events[MAX_EVENTS];

    while(running_)
    {
        int nfds = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if(nfds == -1)
        {
            if(errno == EINTR) continue;
            perror("epoll wait");
            break;
        }

        for(int i = 0; i < nfds; ++i)
        {   
            if(!running_) break;

            // if(events[i].events & (EPOLLERR | EPOLLHUP))
            if(events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
            {
                if(events[i].data.fd == sock_fd_)
                {
                    std::cout << "\r\033[KServer connection lost." << std::endl;
                }
                stop();

                break;
        
            }
            else if(events[i].data.fd == sig_fd_)
            {
                struct signalfd_siginfo fdsi;
                ssize_t s = read(sig_fd_, &fdsi, sizeof(fdsi));
                if(s != sizeof(fdsi))
                {
                    perror("read");
                    // running_ = false;
                    stop();
                    break;
                }

                std::cout<<"\nSignal received, exiting...\n";
                stop();
            }
            else if(events[i].data.fd == sock_fd_)
            {
                if(events[i].events & EPOLLIN)
                {
                    handle_server_message();
                }
            }
            else if(events[i].data.fd == STDIN_FILENO)
            {
                if(events[i].events & EPOLLIN)
                {
                    handle_keyboard_input();
                }
            }

        }
    }
}

void Client::setup_signalfd()
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
void Client::setup_epoll()
{
    epoll_fd_ = epoll_create1(0);
    if(epoll_fd_ == -1)
    {
        perror("epoll_create1");
        stop();
        exit(1);
    }
    epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;

    ev.data.fd = sig_fd_;
    if(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, sig_fd_, &ev) == -1)
    {
        perror("epoll_ctl (sig_fd)");
        stop();
        exit(1);
    }

    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;

    ev.data.fd = sock_fd_;
    if(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, sock_fd_, &ev) == -1)
    {
        perror("epoll_ctl (sock_fd)");
        // running_ = false;
        stop();
        return;
    }
    
    ev.events = EPOLLIN | EPOLLET;

    ev.data.fd = STDIN_FILENO;
    if(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, STDIN_FILENO, &ev) == -1)
    {
        perror("epoll_ctl (STDIN)");
        stop();
        return;
    }
}

void Client::stop()
{
    running_ = false;
}

void Client::handle_server_message()
{
    if(!running_) return;
    
    char buffer[BUFFER_SIZE];
    while(running_)
    {
        ssize_t n = recv(sock_fd_, buffer, sizeof(buffer), 0);
        if(n > 0)
        {
            in_buf_.append(buffer, n);
            while(true)
            {
                if(in_buf_.size() < sizeof(uint32_t)) break;

                uint32_t net_len;
                memcpy(&net_len, in_buf_.data(), sizeof(uint32_t));
                uint32_t payload_len = ntohl(net_len);

                if(in_buf_.size() < (sizeof(uint32_t) + payload_len))
                {
                    break;
                }

                std::string msg = in_buf_.substr(sizeof(uint32_t), payload_len);
                in_buf_.erase(0, sizeof(uint32_t) + payload_len);

                std::cout << "\r\033[K" << msg;
                if(!msg.empty() && msg.back() != '\n')
                {
                    std::cout << std::endl;
                }
            }

            // continue reading until EAGAIN
            continue;
        }
        else
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            else
            {
                std::cout << "\r\033[KServer connection lost." << std::endl;
                stop();
                break;
            }
        }
    }
    if(running_)
    {
        std::cout<<"\r\033[K"<<prompt_<<current_line_<<std::flush;
    }
}

void Client::handle_keyboard_input()
{
    if(!running_) return;

    char c;
    while(read(STDIN_FILENO, &c, 1) > 0)
    {
        if(c == '\n' || c == '\r')
        {
            if(current_line_ == "exit")
            {
                stop();
                return;
            }

            if(!current_line_.empty())
            {
                std::cout << "\r\033[K" << prompt_ << current_line_ << std::endl; 
                // std::string message_to_send = current_line_ + "\n";
                
                uint32_t len = current_line_.size();
                uint32_t net_len = htonl(len);

                if(send(sock_fd_, &net_len, sizeof(uint32_t), MSG_NOSIGNAL) < 0)
                {
                    perror("Send failed (header)!!");
                    stop();
                    return;
                }

                if(send(sock_fd_, current_line_.c_str(), len, 0) < 0)
                {
                    perror("Send failed (payload)!!");
                    stop();
                    return;
                }           
                current_line_.clear();
            }
            else continue;
            
            std::cout<<prompt_<<std::flush;
        }
        else if(c == 127 || c == 8)
        {
            if(!current_line_.empty())
            {
                current_line_.pop_back();
                std::cout << "\r\033[K" << prompt_ << current_line_ << std::flush;
            }
            else if(current_line_.empty())
            {
                std::cout<<"\a"<<std::flush;
                std::cout<<"\r\033[K" << prompt_ << std::flush;
            }
        }
        else if(isprint(c))
        {
            current_line_ += c;
            std::cout<<c<<std::flush;
        }
    }
}

void Client::set_nonblocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

void Client::disable_raw_mode()
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios_);
}

void Client::enable_raw_mode()
{
    tcgetattr(STDIN_FILENO, &orig_termios_);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios_;
    raw.c_lflag &= ~(ICANON | ECHO);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}