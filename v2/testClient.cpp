#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <thread>
#include <vector>
#include <string>
#include <chrono>

#define PORT 8888
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 1024    

//the test parameters 
const int NUM_THREADS = 50;
const int MESSAGE_PER_THREAD = 10;

int set_up_client_socket(const char *ip, int port)
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

    if(inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0)
    {
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

//The function that each client thread will run
void client_task(int thread_id)
{
    int sock_fd = set_up_client_socket(SERVER_IP, PORT);
    if(sock_fd < 0)
    {
        std::cerr<<"Thread "<<thread_id<<" failed to connect.\n";
        return;
    }
    
    char buffer[BUFFER_SIZE];

    for(int i = 0; i < MESSAGE_PER_THREAD; i++)
    {
        std::string message = "Message " + std::to_string(i) + " from thread " 
        + std::to_string(thread_id) + "\n";

        std::string expected_response = "Echo: "+ message;
        
        if(send(sock_fd, message.c_str(), message.size(), 0) < 0)
        {
            perror("Send failed!!");
            break;
        }
        
        ssize_t n = recv(sock_fd, buffer, BUFFER_SIZE - 1, 0);
        if(n > 0)
        {
            buffer[n] = '\0';
            std::string received_response(buffer);

            if(received_response != expected_response)
            {
                std::cerr<<"[Thread "<<thread_id<<" ] FAILED :( !! Sent: "<<message.substr(0, message.size()-1)
                         <<" Got: "<<received_response.substr(0, received_response.size()-1)<<"\n";
            }
        }
        else
        {
            std::cerr<<"Thread "<<thread_id<<": Recv failed or server closed connection \n";
            break;
        }
    }
    close(sock_fd);
}

int main()
{
    std::vector<std::thread> threads;
    std::cout<<"Starting "<<NUM_THREADS<<" client threads to test the server... :) \n";

    for(int i = 0; i < NUM_THREADS; i++)
    {
        threads.emplace_back(client_task, i);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    for(auto& th : threads)
    {
        th.join();
    }

    std::cout<<"Test end :) \n";
    std::cout<<"Check for any 'FAILED' message above.\n";
    return 0;
}