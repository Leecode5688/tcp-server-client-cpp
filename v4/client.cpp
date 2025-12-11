#include "client.h"
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <cstring>
#include <netdb.h>
#include <poll.h>

#define BUFFER_SIZE 4096

struct termios Client::orig_termios_;

Client::Client(const std::string& ip, int port) : server_ip_(ip), port_(port) 
{
    enable_raw_mode();   
    setup_signalfd();
}

Client::~Client() 
{
    stop();
    
    if(sock_fd_ != -1) 
    {
        close(sock_fd_);
        sock_fd_ = -1;
    }
    if(sig_fd_ != -1)
    {
        close(sig_fd_);
        sig_fd_ = -1;
    }
}

bool Client::connect_to_server() 
{
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(server_ip_.c_str(), std::to_string(port_).c_str(), &hints, &res);
    if(err != 0) 
    {
        std::cerr<<"getaddrinfo failed: "<<gai_strerror(err)<<std::endl;
        freeaddrinfo(res);
        return false;
    }

    sock_fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if(sock_fd_ < 0) {
        perror("Socket creation failed!!");
        freeaddrinfo(res);
        close(sock_fd_);
        return false;
    }

    if(connect(sock_fd_, res->ai_addr, res->ai_addrlen) < 0) {
        perror("Connection failed!!");
        freeaddrinfo(res);
        close(sock_fd_);
        sock_fd_ = -1; 
        return false;
    }

    freeaddrinfo(res);
    return true;
}

bool Client::login()
{
    bool login_success = false;
    while(!login_success && running_)
    {
        char buffer[BUFFER_SIZE];
        ssize_t n = recv(sock_fd_, buffer, sizeof(buffer) - 1, 0); 
        if(n <= 0) {
            std::cerr << "\nConnection closed during login." << std::endl;
            return false;
        }
        buffer[n] = '\0';
        
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
            while(running_)
            {
                username_.clear(); 
                char c;
                
                while(running_)
                {
                    struct pollfd pfd[2];
                    pfd[0].fd = STDIN_FILENO; 
                    pfd[0].events = POLLIN;
                    pfd[1].fd = sig_fd_;      
                    pfd[1].events = POLLIN;

                    int ret = poll(pfd, 2, -1);
                    if (ret < 0 && errno != EINTR) 
                    {
                        break;
                    }
                    // signal (Ctrl+C)
                    if (pfd[1].revents & POLLIN) {
                        std::cout << "\nExiting..." << std::endl;
                        running_ = false;
                        return false;
                    }

                    // keyboard input
                    if (pfd[0].revents & POLLIN) 
                    {
                        if (read(STDIN_FILENO, &c, 1) <= 0) 
                        { 
                            running_ = false; 
                            break; 
                        }
                        
                        if (c == '\n' || c == '\r') 
                        {
                            break;
                        }

                        if (isprint(c)) 
                        {
                            username_ += c;
                            std::cout << c << std::flush;
                        }
                        else if ((c == 127 || c == 8) && !username_.empty()) 
                        {
                            username_.pop_back();
                            std::cout << "\b \b" << std::flush;
                        }
                    }
                }

                if (!running_) return false;

                std::cout << std::endl;

                if (username_.empty()) 
                {
                    std::cout << "Username cannot be empty. Please enter a username: " << std::flush;
                    continue;
                }
                break;
            }
            
            //send Username Packet
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

        if(login_success) {
            prompt_ = "[" + username_ + "]: ";
        }
    }
    return login_success;
}

void Client::run()
{
    set_nonblocking(sock_fd_);
    set_nonblocking(STDIN_FILENO);

    std::cout << prompt_ << std::flush;

    running_ = true;
    receiver_thread_ = std::thread(&Client::receive_loop, this);
    printer_thread_ = std::thread(&Client::printer_loop, this);

    input_loop();
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

void Client::stop()
{
    // already stopped
    if(!running_) return; 
    
    running_ = false;
    message_queue_.stop();
    
    // join threads if they were started
    if(receiver_thread_.joinable()) 
    {
        receiver_thread_.join();
    }
    if(printer_thread_.joinable())
    {
        printer_thread_.join();
    }
}

void Client::process_buffer()
{
    while(true)
    {
        if(in_buf_.size() < sizeof(uint32_t))
        {
            break;
        }
        uint32_t net_len;
        memcpy(&net_len, in_buf_.data(), sizeof(uint32_t));
        uint32_t payload_len = ntohl(net_len);

        if(in_buf_.size() < (sizeof(uint32_t) + payload_len))
        {
            break;
        }

        std::string msg = in_buf_.substr(sizeof(uint32_t), payload_len);
        in_buf_.erase(0, sizeof(uint32_t) + payload_len);

        if(!msg.empty() && msg.back()=='\n')
        {
            msg.pop_back();
        }
        message_queue_.push(msg);
    }
}

void Client::receive_loop()
{
    char buffer[BUFFER_SIZE];
    struct pollfd pfd;
    pfd.fd = sock_fd_;
    pfd.events = POLLIN | POLLRDHUP;

    if(!in_buf_.empty())
    {
        process_buffer();
    }

    while(running_)
    {
        int ret = poll(&pfd, 1, 200);
        if(ret < 0 && errno != EINTR)
        {
            break;
        }
        if(pfd.revents & (POLLERR | POLLHUP | POLLRDHUP))
        {
            message_queue_.push("Server connection lost.");
            running_ = false;
            // stop();
            break;
        }
        if(pfd.revents & POLLIN)
        {
            ssize_t n = recv(sock_fd_, buffer, sizeof(buffer), 0);
            if(n <= 0) break;
            in_buf_.append(buffer, n);
            process_buffer();
        }
    }
}

void Client::printer_loop()
{
    std::string msg;
    while(message_queue_.pop(msg))
    {
        std::lock_guard<std::mutex> lock(ui_mtx_);
        std::cout << "\r\033[K" << msg << std::endl;
        std::cout << prompt_ << current_line_ << std::flush;
    }
}

void Client::input_loop()
{
    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[1].fd = sig_fd_;
    fds[1].events = POLLIN;

    char c;
    while(running_)
    {
        int ret = poll(fds, 2, 200);
        if(ret < 0 && errno != EINTR)
        {
            break;
        }
        
        //ctrl+c
        if(fds[1].revents & POLLIN)
        {
            std::cout << "\nExiting..." << std::endl;
            stop();
            break;
        }

        if(fds[0].revents & POLLIN)
        {
            if(read(STDIN_FILENO, &c, 1) <= 0)
            {
                stop();
                break;
            }

            std::lock_guard<std::mutex> lock(ui_mtx_);
            if(c == '\n' || c == '\r')
            {
                if(current_line_ == "exit")
                {
                    stop();
                    break;
                }

                if(!current_line_.empty())
                {
                    
                    std::cout << "\r\033[K" << prompt_ << current_line_ << std::endl;
                    uint32_t len = current_line_.size();
                    uint32_t net_len = htonl(len);
                    if(send(sock_fd_, &net_len, sizeof(uint32_t), MSG_NOSIGNAL) < 0)
                    {
                        stop();
                        break;
                    }
                    if(send(sock_fd_, current_line_.c_str(), len, MSG_NOSIGNAL) < 0)
                    {
                        stop();
                        break;
                    }

                    current_line_.clear();
                    std::cout << prompt_ << std::flush;
                }
            }
            else if(c == 127 || c == 8)
            {
                if(!current_line_.empty())
                {
                    current_line_.pop_back();
                    std::cout << "\r\033[K" << prompt_ << current_line_ << std::flush;
                }
            }
            else if(isprint(c))
            {
                current_line_ += c;
                std::cout << c << std::flush;
            }
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