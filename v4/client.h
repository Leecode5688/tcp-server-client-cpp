#pragma once

#include <string>
#include <termios.h>
#include <thread>
#include <mutex>
#include "ringbuffer.h"
#include "codec.h"
#include "safe_queue.h"

class Client {
private:
    void setup_signalfd();
    void stop();

    //network => queue
    void receive_loop();
    //queue => terminal
    void printer_loop();
    //keyboard => network
    void input_loop();

    void handle_server_message(const chat::ServerMessage& msg);

    static void set_nonblocking(int fd);
    static void enable_raw_mode();
    static void disable_raw_mode();

    std::string server_ip_;
    int port_;
    int sock_fd_{-1};
    int sig_fd_{-1}; 

    std::string username_;
    std::string current_room_;
    std::string prompt_;
    std::string current_line_;
    std::mutex ui_mtx_;

    std::unique_ptr<ProtobufCodec> codec_;
    RingBuffer in_buf_{8192};
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