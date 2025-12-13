#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <vector>
#include <thread>
#include <chrono>
#include <netdb.h>
#include <random>
#include "chat.pb.h"

#define PORT 8888
#define BUFFER_SIZE 4096

bool send_proto(int sock_fd, const chat::ClientMessage& msg)
{
    std::string payload;
    if(!msg.SerializeToString(&payload))
    {
        return false;
    }

    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));

    if(send(sock_fd, &len, sizeof(uint32_t), 0) != sizeof(uint32_t))
    {
        return false;
    }

    if(send(sock_fd, payload.data(), payload.size(), 0) != static_cast<ssize_t>(payload.size()))
    {
        return false;
    }

    return true;
}

bool recv_proto(int sock_fd, chat::ServerMessage& out_msg)
{
    char header[4];
    ssize_t n = recv(sock_fd, header, sizeof(uint32_t), MSG_WAITALL);
    if(n != sizeof(uint32_t))
    {
        return false;
    }

    uint32_t net_len;
    std::memcpy(&net_len, header, sizeof(uint32_t));
    uint32_t len = ntohl(net_len);
    if(len > 10 * 1024 * 1024)
    {
        return false;
    }

    std::vector<char> buf(len);
    n = recv(sock_fd, buf.data(), len, MSG_WAITALL);
    if(n != static_cast<ssize_t>(len))
    {
        return false;
    }
    return out_msg.ParseFromArray(buf.data(), len);
}

const char* get_server_ip()
{
    const char* ip = std::getenv("SERVER_IP");
    return ip ? ip : "127.0.0.1";
}

void discard_loop(int fd)
{
    char buffer[BUFFER_SIZE];
    while(true)
    {
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if(n <= 0) break;
    }
}

int main(int argc, char** argv)
{
    if(argc < 2 || argc > 3)
    {
        std::cerr << "Usage: " << argv[0] << " <username> [interval_ms]" << std::endl;
        return 1;
    }

    std::string username = argv[1];
    int interval_ms = 1000;
    if(argc >= 3)
    {
        try 
        {
            interval_ms = std::stoi(argv[2]);
        }
        catch(...)
        {
            std::cerr << "Invalid interval argument. Using default 1000ms." << std::endl;
        }
    }
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(sock_fd < 0)
    {
        perror("Socket creation failed!!");
        return 1;
    }

    struct addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string server_host = get_server_ip();
    int err = getaddrinfo(server_host.c_str(), std::to_string(PORT).c_str(), &hints, &res);

    if(err != 0)
    {
        std::cerr<<"getaddrinfo failed: "<<gai_strerror(err)<<std::endl;
        close(sock_fd);
        return 1;
    }
    if(connect(sock_fd, res->ai_addr, res->ai_addrlen) < 0)
    {
        perror("Connection failed!!");
        freeaddrinfo(res);
        close(sock_fd);
        return 1;
    }

    freeaddrinfo(res);
    
    chat::ClientMessage login_msg;
    login_msg.mutable_login()->set_username(username);

    if(!send_proto(sock_fd, login_msg))
    {
        std::cerr<<"Bot "<<username<<": Failed to send login message."<<std::endl;
        close(sock_fd);
        return 1;
    }

    chat::ServerMessage resp;
    if (!recv_proto(sock_fd, resp)) {
        std::cerr << "Bot " << username << ": Disconnected during login." << std::endl;
        close(sock_fd);
        return 1;
    }

    if (!resp.has_login_response() || !resp.login_response().success()) {
        std::cerr << "Bot " << username << ": Login failed." << std::endl;
        close(sock_fd);
        return 1;
    }

    std::thread reader(discard_loop, sock_fd);
    reader.detach();
    // "render".detach(); stupid typo ^ ^
    int msg_count = 0;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> jitter_dist(-interval_ms * 0.2, interval_ms * 0.2);
    
    while(true)
    {
        chat::ClientMessage chat_msg;
        chat_msg.mutable_global_chat()->set_text(
            "Hello from bot " + username + "!! Message #" + std::to_string(++msg_count)
        );

        if(!send_proto(sock_fd, chat_msg))
        {
            std::cerr<<"Bot "<<username<<": Failed to send chat message."<<std::endl;
            break;
        }

        int variation = jitter_dist(gen);
        int actual_sleep = interval_ms + variation;
        if(actual_sleep < 10) actual_sleep = 10;
        std::this_thread::sleep_for(std::chrono::milliseconds(actual_sleep));
    }

    close(sock_fd);
    return 0;
}