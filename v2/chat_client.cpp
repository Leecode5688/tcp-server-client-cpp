#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>
#include <cstdlib>

#define PORT 8888
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 4096 

std::mutex stdout_mtx;
std::atomic<bool> keep_running_client{true};
struct termios orig_termios;

void enable_raw_mode()
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);

    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void disable_raw_mode()
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void cleanup_handler(int)
{
    keep_running_client = false;
    disable_raw_mode();
    std::cout<<"\nExiting...\n";
    exit(0);
}

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

void receive_messages(int sock_fd, const std::string& prompt, std::string& current_line)
{
    char buffer[BUFFER_SIZE];
    while(keep_running_client)
    {
        ssize_t n = recv(sock_fd, buffer, BUFFER_SIZE-1, 0);
        if(n > 0)
        {
            buffer[n] = '\0';
            std::string received_data(buffer);

            std::lock_guard<std::mutex> lock(stdout_mtx);

            std::cout<<"\r\033[K";
            std::cout<<received_data;

            if(received_data.back() != '\n')
            {
                std::cout<<std::endl;
            }

            std::cout<<prompt<<current_line<<std::flush;
        }
        else
        {
            if(keep_running_client)
            {
                std::cout<<"\r\033[KServer connection lost!"<<std::endl;
                keep_running_client = false;
            }
            break;
        }
    }
}

int main()
{
    signal(SIGINT, cleanup_handler);
    signal(SIGTERM, cleanup_handler);

    int sock_fd = setup_client_socket(SERVER_IP, PORT);
    if(sock_fd < 0)
    {
        std::cerr<<"Client setup failed!! Exiting..."<<std::endl;
        return 1;
    }

    char welcome_buffer[BUFFER_SIZE];
    ssize_t n = recv(sock_fd, welcome_buffer, BUFFER_SIZE-1, 0);
    if(n > 0)
    {
        welcome_buffer[n] = '\0';
        std::cout<<welcome_buffer;
    }
    else
    {
        std::cerr<<"Didn't receive welcom message from server."<<std::endl;
        close(sock_fd);
        return 1;
    }

    std::string username;
    std::getline(std::cin, username);

    if(username.empty())
    {
        std::cout<<"Username can't be empty!!"<<std::endl;
        close(sock_fd);
        return 1;
    }

    std::string user_message = username + "\n";
    if(send(sock_fd, user_message.c_str(), user_message.size(), 0) < 0)
    {
        perror("Failed to send username!!");
        close(sock_fd);
        return 1;
    }

    std::string prompt = "[" + username + "]: ";
    std::string current_line;
    std::thread receiver_thread(receive_messages, sock_fd, std::ref(prompt), std::ref(current_line));
    enable_raw_mode();
    
    {
        std::lock_guard<std::mutex> lock(stdout_mtx);
        std::cout<<"Connected!! Type 'exit' to quit.\n";
        std::cout<<prompt<<std::flush;
    }

    while(keep_running_client)
    {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if(n <= 0 || !keep_running_client)
        {
            break;
        }

        std::lock_guard<std::mutex> lock(stdout_mtx);

        if(c == '\n' || c == '\r')
        {
            if(!current_line.empty())
            {
                if(current_line == "exit")
                {
                    break;
                }

                std::cout<<"\r\033[K"<<prompt<<current_line<<std::endl;

                std::string message_to_send = current_line + "\n";
                if(send(sock_fd, message_to_send.c_str(), message_to_send.size(), 0) < 0)
                {
                    perror("Send failed!!");
                    break;
                }
                current_line.clear();
                std::cout<<prompt<<std::flush;
            }
            else continue;
        }
        else if(c == 127 || c == 8)
        {
            if(!current_line.empty())
            {
                current_line.pop_back();
                std::cout<<"\r\033[K"<<prompt<<current_line<<std::flush;
            }
        }
        else if(c >= 32 && c <= 126)
        {
            current_line += c;
            std::cout<<"\r\033[K"<<prompt<<current_line<<std::flush;
        }
    }
    keep_running_client = false;

    disable_raw_mode();
    shutdown(sock_fd, SHUT_RDWR);

    if(receiver_thread.joinable())
    {
        receiver_thread.join();
    }
    close(sock_fd);
    std::cout<<"\nDisconnected!!"<<std::endl;
    return 0;
}

