#include <iostream>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define PORT 8888
#define BACKLOG 5
#define BUFFER_SIZE 1024

int main()
{
    int server_fd, client_fd;
    struct sockaddr_in server_addr{}, client_addr{};
    socklen_t client_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd < 0)
    {
        perror("Socket creation failed!");
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if(bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed!");
        return 1;
    }
    
    if(listen(server_fd, BACKLOG) < 0)
    {
        perror("Listen failed!");
        return 1;
    }

    std::cout<<"Server listening on port "<<PORT<<"...\n";

    while(true)
    {
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if(client_fd < 0)
        {
            perror("Accept failed!");
            continue;
        }

        std::cout<<"Client connected!\n";

        ssize_t n;
        while((n = recv(client_fd, buffer, BUFFER_SIZE-1, 0)) > 0)
        {
            buffer[n] = '\0';
            std::cout<<"Received: "<<buffer<<std::endl;
            send(client_fd, buffer, n, 0);
        }

        if(n == 0)
        {
            std::cout<<"Client disconnected!\n";
        }
        else
        {
            perror("Recv failed!");
        }

        close(client_fd);
    }
    close(server_fd);
    return 0;
}