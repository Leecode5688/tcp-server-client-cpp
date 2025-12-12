#include "client.h"
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <iostream>
#include <unistd.h>
#include <sstream>
#include <fcntl.h>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <netdb.h>
#include <poll.h>

#define BUFFER_SIZE 4096

struct termios Client::orig_termios_;

Client::Client(const std::string& ip, int port) : server_ip_(ip), port_(port) 
{
    codec_ = std::make_unique<ProtobufCodec>();
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
    disable_raw_mode();

    std::cout<<"Enter username: "<<std::flush;
    std::string name;
    std::getline(std::cin, name);

    if(name.empty())
    {
        return false;
    }

    enable_raw_mode();
    std::cout<<"\nLogged in as "<<name<<std::endl;
    username_ = name;
    chat::ClientMessage req;
    req.mutable_login()->set_username(name);

    auto packet = codec_->encode(req);
    if(send(sock_fd_, packet.data(), packet.size(), 0) < 0)
    {
        perror("Failed to send login message");
        return false;
    }


    char header[4];

    if(recv(sock_fd_, header, 4, MSG_WAITALL) != 4) return false;

    uint32_t net_len;
    std::memcpy(&net_len, header, 4);
    uint32_t len = ntohl(net_len);

    std::vector<char> buf(len);
    if(recv(sock_fd_, buf.data(), len, MSG_WAITALL) != static_cast<ssize_t>(len)) return false;

    chat::ServerMessage resp;

    if(!resp.ParseFromArray(buf.data(), len))
    {
        std::cerr<<"Failed to parse server response"<<std::endl;
        return false;
    }

    if(resp.has_login_response() && resp.login_response().success())
    {
        prompt_ = "[" + name + "@Lobby]: ";
        return true;
    }
    else
    {
        std::cout<<"Login failed: "<<resp.login_response().message()<<std::endl;
        return false;
    }
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
                    std::cout << "\r\033[K" << std::flush;
                    // std::cout << "\r\033[K" << prompt_ << current_line_ << std::endl;
                    chat::ClientMessage msg;
                    bool valid = false;
                    
                    if(current_line_[0] == '/')
                    {
                        std::stringstream ss(current_line_);
                        std::string cmd;
                        ss >> cmd;

                        if(cmd == "/join")
                        {
                            std::string room;
                            ss >> room;
                            if(!room.empty())
                            {
                                msg.mutable_join_room()->set_room_name(room);
                                current_room_ = room;
                                prompt_ = "[" + username_ + "@" + current_room_ + "]: ";
                                valid = true;
                            }
                        }
                        else if(cmd == "/leave")
                        {
                            msg.mutable_join_room()->set_room_name("");
                            current_room_ = "";
                            prompt_ = "["+username_+"@Lobby]: ";
                            std::cout<<"Switched to global chatroom."<<std::endl;
                            valid = true;
                        }
                        else if(cmd == "/msg")
                        {
                            std::string target;
                            std::string text;
                            ss>>target;
                            std::getline(ss, text);
                            if(!target.empty() && text.length() > 1)
                            {
                                auto* pm = msg.mutable_private_chat();
                                pm->set_target_username(target);
                                pm->set_text(text.substr(1));
                                valid = true;
                            }
                        }
                        else if(cmd == "/exit" || cmd == "/quit")
                        {
                            stop();
                            break;
                        }
                    }
                    else
                    {
                        if(current_room_.empty())
                        {
                            msg.mutable_global_chat()->set_text(current_line_);
                        }
                        else
                        {
                            auto* rm = msg.mutable_room_chat();
                            rm->set_room_name(current_room_);
                            rm->set_text(current_line_);
                        }

                        valid = true;
                    }
                    if(valid)
                    {
                        auto packet = codec_->encode(msg);
                        send(sock_fd_, packet.data(), packet.size(), MSG_NOSIGNAL);
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

void Client::receive_loop()
{
    while(running_)
    {
        struct pollfd pfd{sock_fd_, POLLIN | POLLRDHUP, 0};
        int ret = poll(&pfd, 1, 200);

        if(ret < 0 && errno != EINTR) break;
        if(pfd.revents & (POLLERR | POLLHUP | POLLRDHUP))
        {
            message_queue_.push("Server connection lost.");
            running_ = false;
            break;
        }
        if(pfd.revents & POLLIN)
        {
            auto iovecs = in_buf_.get_writeable_iovecs();
            if(iovecs.empty())
            {
                //buffer full, for now just discard and reset
                in_buf_.consume(in_buf_.size());
                continue;
            }

            ssize_t n = readv(sock_fd_, iovecs.data(), iovecs.size());
            if(n <= 0) break;

            in_buf_.commit_write(n);

            //decode loop
            while(true)
            {
                auto opt_msg = codec_->decode<chat::ServerMessage>(in_buf_);
                if(!opt_msg) break;

                handle_server_message(*opt_msg);
            }
        }
    }
}

void Client::handle_server_message(const chat::ServerMessage& msg)
{
    std::string text;

    switch (msg.payload_case())
    {
        case chat::ServerMessage::kChatBroadcast:
        {
            const auto& b = msg.chat_broadcast();
            if(b.sender() == "Server")
            {
                text = "[Server]: " + b.text();
            }
            else
            {
                std::string room_display = b.origin_room();
                if(room_display.empty())
                {
                    room_display = "Lobby";
                }

                text = "[" + b.sender() + "@" + room_display + "]: " + b.text();;
            }
            break;
        }
        
        case chat::ServerMessage::kPrivateChat:
        {
            const auto& pm = msg.private_chat();
            text = "[DM from " + pm.sender() + "]: " + pm.text();
            break;
        }

        case chat::ServerMessage::kError: 
        {
            text = "[Error]: "+ msg.error().reason();
            break;
        }

        case chat::ServerMessage::kLoginResponse:
        {
            text = "[Server]: "+msg.login_response().message();
            break; 
        }

        default: return;
    }
    message_queue_.push(text);
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