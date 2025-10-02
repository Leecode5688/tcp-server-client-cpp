#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 8888
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 1024

int setup_client_socket(const char* ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
    {
        perror("Socket creation failed!!");
        return -1;
    }
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if(inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported!!");
        close(fd);
        return -1;
    }

    if(connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Connection failed!!");
        close(fd);
        return -1;
    }

    return fd;
}

int main()
{
    int sock_fd = setup_client_socket(SERVER_IP, PORT);
    if(sock_fd < 0)
    {
        std::cerr<<"Client setup failed!! Exiting...\n";
        return 1;
    }

    std::cout<<"Connected to server at: "<<SERVER_IP<<":"<<PORT<<"\n";
    char buffer[BUFFER_SIZE];

    while(true)
    {
        std::string message;
        std::cout<<"Enter message (type 'exit' to quit): ";
        std::getline(std::cin, message);

        if(message == "exit") break;
        message += "\n";
        
        //send to server
        //TCP => need to specify message size
        if(send(sock_fd, message.c_str(), message.size(), 0) < 0)
        {
            perror("Send failed!!");
            break;
        }
        //receive echo from server
        ssize_t n = recv(sock_fd, buffer, BUFFER_SIZE-1, 0);

        if(n > 0)
        {
            buffer[n] = '\0';
            std::cout<<"Server echoed: "<<buffer<<"\n";
        }
        else if(n == 0)
        {
            std::cout<<"Server closed the connection!!\n";
            break;
        }
        else
        {
            perror("Recv failed!!");
            break;
        }
    }

    close(sock_fd);
    return 0;
}