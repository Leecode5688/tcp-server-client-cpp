#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <thread>
#include <atomic>

#define PORT 8888
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 4096

std::atomic<bool> keep_running_client{true};

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

void receive_messages(int sock_fd) {
    char buffer[BUFFER_SIZE];
    while(keep_running_client)
    {
        ssize_t n = recv(sock_fd, buffer, BUFFER_SIZE-1, 0);
        if(n > 0)
        {
            buffer[n] = '\0';
            std::string received_msg(buffer);

            if(!received_msg.empty() && received_msg.back() == '\n')
            {
                received_msg.pop_back();
            }
            std::cout<<"\r\033[K"<<received_msg<<std::endl;
            std::cout<<"Enter message > "<<std::flush;
        }
        else
        {
            if(!keep_running_client)
            {
                break;
            }
            std::cout<<"\r\033[KServer connection lost!"<<std::endl;
            keep_running_client = false;
            break;
        }
    }
}

int main()
{
    int sock_fd = setup_client_socket(SERVER_IP, PORT);
    if(sock_fd < 0)
    {
        std::cerr<<"Client setup failed!! Exiting..."<<std::endl;
        return 1;
    }

    std::cout<<"Connected to server at: "<<SERVER_IP<<":"<<PORT<<
    " Type 'exit' to quit."<<std::endl;
        
    //start a spearate thread to listen for incoming messages. 
    std::thread receiver_thread(receive_messages, sock_fd);

    std::string line;
    std::cout<<"Enter message > "<<std::flush;
    while(keep_running_client && std::getline(std::cin, line))
    {
        if(line == "exit")
        {
            break;
        }
        if(!line.empty())
        {
            line += "\n";
            if(send(sock_fd, line.c_str(), line.size(), 0) < 0)
            {
                perror("Send failed");
                break;
            }
        }
        std::cout<<"Enter message > "<<std::flush;
    }

    //shutdown part
    keep_running_client = false;
    shutdown(sock_fd, SHUT_RDWR);
    
    if(receiver_thread.joinable())
    {
        receiver_thread.join();
    }
    close(sock_fd);
    std::cout<<"Disconnected!!"<<std::endl;
    
    return 0;
}