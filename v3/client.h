#pragma once

#include <string>
#include <termios.h>

class Client {
private:
    void setup_signalfd();
    void setup_epoll();
    void handle_server_message();
    void handle_keyboard_input();

    static void set_nonblocking(int fd);
    static void enable_raw_mode();
    static void disable_raw_mode();

    std::string server_ip_;
    int port_;
    int sock_fd_{-1};
    int epoll_fd_{-1};
    int sig_fd_{-1};

    std::string username_;
    std::string prompt_;
    std::string current_line_;
    //v3
    std::string in_buf_;

    static struct termios orig_termios_;
    bool running_ = true;

public: 
    Client(const std::string& ip, int port);
    ~Client();
    bool connect_to_server();
    bool login();
    void run();
    
};