#include <iostream>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <csignal>
#include <cerrno>
#include <fcntl.h>

#define PORT 8888
#define BACKLOG 5
#define BUFFER_SIZE 1024

//use this flag to control the main loop of our server
//so we can shutdown gracefully
volatile sig_atomic_t keep_running = 1;

void signal_handler(int signum)
{
    if(signum == SIGINT)
    {
        std::cout<<"\nSIGINT received. Shutting down gracefully...\n";
        keep_running = 0;
    }
}

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
        close(fd);
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

    //set the server socket to non-blocking mode
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("Failed to set non-blocking mode on server socket :( !!");
        close(fd);
        return -1;
    }

    return fd;
}
int main()
{
    std::signal(SIGINT, signal_handler);
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

    while(keep_running)
    {
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if(client_fd < 0)
        {
            //no pending client, or interrupted => just continue
            if(errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)
            {
                continue;
            }
            perror("Accept failed!!");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout<<"Client connected from "<<client_ip<<":"<<ntohs(client_addr.sin_port)<<"\n";

        //make the client_fd also nonblocking
        int client_flags = fcntl(client_fd, F_GETFL, 0);
        if(client_flags < 0 || fcntl(client_fd, F_SETFL, client_flags | O_NONBLOCK) < 0)
        {
            perror("Failed to set non-blocking mode on client_fd :( !!");
            close(client_fd);
            continue;
        }

        ssize_t n;
        while(keep_running)
        {
            n = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
            if(n > 0)
            {
                buffer[n] = '\0';
                std::cout<<"Received: "<<buffer<<std::endl;
                send(client_fd, buffer, n, 0);
            }
            else if(n == 0)
            {
                std::cout<<"Client disconnected!!\n";
                break;
            }
            else
            {
                if(errno == EWOULDBLOCK || errno == EAGAIN)
                {
                    continue;
                }
                else if(errno == EINTR)
                {
                    break;
                }
                else
                {
                    perror("Recv failed!!");
                    break;
                }
            }
        }

        close(client_fd);
    }
    close(server_fd);
    std::cout<<"Server has shut down gracefully...\n";
    return 0;
}

