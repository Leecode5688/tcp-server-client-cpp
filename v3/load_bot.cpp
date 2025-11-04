#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <vector>
#include <thread>

#define PORT 8888
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 4096

std::string format_message(const std::string& msg)
{
    uint32_t len = msg.size();
    uint32_t net_len = htonl(len);
    std::string packet;
    packet.append(reinterpret_cast<const char*>(&net_len), sizeof(uint32_t));
    packet.append(msg);
    return packet;
}

std::string recv_message(int sock_fd)
{
    char header_buf[4];
    if(recv(sock_fd, header_buf, 4, MSG_WAITALL) != 4)
    {
        return "";
    }

    uint32_t net_len;
    memcpy(&net_len, header_buf, sizeof(uint32_t));
    uint32_t payload_len = ntohl(net_len);

    if(payload_len > 8192)
    {
        std::cerr<<"Payload too large: "<<payload_len<<std::endl;
        return "";
    }

    std::vector<char> payload_buf(payload_len);
    if(recv(sock_fd, payload_buf.data(), payload_len, MSG_WAITALL) != payload_len)
    {
        return "";
    }

    return std::string(payload_buf.begin(), payload_buf.end());
}

int main(int argc, char** argv)
{
    if(argc != 2)
    {
        std::cerr<<"Usage: "<<argv[0]<<" <username>"<<std::endl;
        return 1;
    }

    std::string username = argv[1];
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(sock_fd < 0)
    {
        perror("Socket creation failed!!");
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    if(connect(sock_fd, (struct sockaddr*)& server_addr, sizeof(server_addr)) < 0)
    {
        perror("Connection failed!!");
        close(sock_fd);
        return 1;
    }

    std::string welcome = recv_message(sock_fd);
    if(welcome.empty())
    {
        std::cerr<<"Bot "<<username<<": Did not receive welcone."<<std::endl;
        close(sock_fd);
        return 1;
    }

    std::string user_packet = format_message(username);
    if(send(sock_fd, user_packet.data(), user_packet.size(), 0) < 0)
    {
        perror("Bot send username failed!!");
        close(sock_fd);
        return 1;
    }

    std::string confirmation = recv_message(sock_fd);
    if(confirmation.find("Username accepted") == std::string::npos)
    {
        std::cerr<<"Bot "<<username<<": Login failed: "<<confirmation<<std::endl;
        close(sock_fd);
        return 1;
    }

    std::string spam_message = format_message("Hello from bot " + username + "!!");

    while(true)
    {
        if(send(sock_fd, spam_message.data(), spam_message.size(), 0) < 0)
        {
            perror("Bot send spam failed!!");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    close(sock_fd);
    return 0;
}