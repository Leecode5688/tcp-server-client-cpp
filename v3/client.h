#pragma once

#include <string>
#include <termios.h>
#include <thread>
#include <mutex>
#include "safe_queue.h"

class Client {
private:
    void setup_signalfd();
    void stop();
    
    // void handle_server_message();
    // void handle_keyboard_input();

    //network => queue
    void receive_loop();
    //queue => terminal
    void printer_loop();
    //keyboard => network
    void input_loop();

    void process_buffer();
    static void set_nonblocking(int fd);
    static void enable_raw_mode();
    static void disable_raw_mode();

    std::string server_ip_;
    int port_;
    int sock_fd_{-1};
    int sig_fd_{-1}; 

    std::string username_;
    std::string prompt_;
    std::string current_line_;
    std::mutex ui_mtx_;

    std::string in_buf_; 
    SafeQueue<std::string> message_queue_;

    std::thread receiver_thread_;
    std::thread printer_thread_;

    static struct termios orig_termios_;
    bool running_ = true;

public: 
    Client(const std::string& ip, int port);
    ~Client();
    bool connect_to_server();
    bool login();
    void run();
};