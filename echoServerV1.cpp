#include <iostream>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8888
#define BACKLOG 5
#define BUFFER_SIZE 1024

int setup_server_socket(int port, int backlog)
{
    //creating the socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
    {
        perror("Socket creation failed!!");
        return -1;
    }
    // This option allows the socket to be bound to the same address and port
    // even if it's in a TIME_WAIT state, preventing "Address already in use" errors.
    int opt = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("Setsockopt failed!!");
        return -1;
    }

    sockaddr_in server_addr{};
    //configuring the server address
    //IPv4, need to match the socket type AF_INET
    server_addr.sin_family = AF_INET;
    //Accepts connections on all local IP addresses
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);
    
    //binding socket to address
    if(bind(fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed!!");
        close(fd);
        return -1;
    }

    if(listen(fd, backlog) < 0)
    {
        perror("Listen failed!!");
        close(fd);
        return -1;
    }

    return fd;
}
int main()
{
    // int server_fd, client_fd;
    int server_fd = setup_server_socket(PORT, BACKLOG);
    if(server_fd < 0)
    {
        std::cerr<<"Server setup failed, exiting....\n";
        return -1;
    }

    std::cout<<"Server listening on port "<<PORT<<" ...\n";
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    while(true)
    {
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if(client_fd < 0)
        {
            perror("Accept failed!!");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout<<"Client connected from "<<client_ip<<":"<<ntohs(client_addr.sin_port)<<"\n";

        ssize_t n;
        while((n = recv(client_fd, buffer, BUFFER_SIZE-1, 0)) > 0)
        {
            buffer[n] = '\0';
            std::cout<<"Received: "<<buffer<<std::endl;
            send(client_fd, buffer, n, 0);
        }

        if(n == 0)
        {
            std::cout<<"Client disconnected!!\n";
        }
        else if(n < 0)
        {
            perror("Recv failed!!");
        }

        close(client_fd);
    }
    close(server_fd);
    return 0;
}

