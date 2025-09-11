#include <iostream>
#include <string>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <queue>
#include <mutex>
#include <condition_variable>

#define PORT 8888
#define BACKLOG 5
#define BUFFER_SIZE 1024
#define MAX_EVENTS 64
#define NUM_THREADS 5

volatile sig_atomic_t keep_running = 1;

struct Task {
    int client_fd;
    std::string data;
};

std::queue<Task> task_queue;
std::mutex queue_mutex;
std::condition_variable condition;

void* worker_thread(void* arg) {
    while (keep_running) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            condition.wait(lock, []{ return !task_queue.empty() || !keep_running; });

            if(!keep_running && task_queue.empty()) break;

            task = task_queue.front();
            task_queue.pop();
        }

        std::cout<<"Worker thread processing data from fd "<<task.client_fd<<": "<<task.data<<std::endl;

        send(task.client_fd, task.data.c_str(), task.data.length(), 0);
    }
        return nullptr;
}

void signal_handler(int signum)
{
    if(signum == SIGINT)
    {
        std::cout<<"\nSIGINT received, shutting down server..."<<std::endl;
        keep_running = 0;
        condition.notify_all();
    }
}

int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0)
    {
        perror("fcntl(F_GETFL) failed!!");
        return -1;
    }

    if(fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("fcntl(F_SETFL, O_NONBLOCK) failed!!");
        return -1;
    }

    return 0;
}

int setup_server_socket(int port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
    {
        perror("Socket creation failed!!");
        return -1;
    }

    //enable SO_REUSEADDR
    int opt = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("Setsockopt failed!!");
        close(fd);
        return -1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

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

    if(set_nonblocking(fd) < 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

int main()
{
    std::signal(SIGINT, signal_handler);

    pthread_t threads[NUM_THREADS];

    for(int i = 0; i < NUM_THREADS; i++)
    {
        pthread_create(&threads[i], nullptr, worker_thread, nullptr);
    }

    int server_fd = setup_server_socket(PORT, BACKLOG);
    if(server_fd < 0)
    {
        std::cerr<<"Server setup failed, exiting......\n";
        return -1;
    }

    std::cout<<"Server listening on port "<<PORT<<"...\n";

    int epoll_fd = epoll_create1(0);
    if(epoll_fd == -1)
    {
        perror("epoll_create1 failed");
        close(server_fd);
        return -1;
    }

    epoll_event event{};
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = server_fd;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1)
    {
        perror("epoll_ctl failed to add server_fd!!");
        close(server_fd);
        close(epoll_fd);
        return -1;
    }

    epoll_event events[MAX_EVENTS];

    while(keep_running)
    {
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if(num_events == -1)
        {
            if(errno == EINTR) continue;

            perror("epoll_wait failed!!");
            break;
        }

        for(int i = 0; i < num_events; i++)
        {
            if(events[i].data.fd == server_fd)
            {
                while(true)
                {
                    sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

                    if(client_fd == -1)
                    {
                        if(errno == EWOULDBLOCK || errno == EAGAIN)
                        {
                            break;
                        }

                        perror("Accept failed !!");
                        break;
                    }
                    
                    char client_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
                    std::cout<<"Client connected from "<<client_ip<<" : "<<ntohs(client_addr.sin_port)<<"\n";

                    set_nonblocking(client_fd);

                    event.events = EPOLLIN | EPOLLET;
                    event.data.fd = client_fd;
                    
                    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1)
                    {
                        perror("epoll_ctl failed to add client_fd !!");
                        close(client_fd);
                    }   
                }
            }
            else
            {
                int client_fd = events[i].data.fd;
                while(true)
                {
                    char buffer[BUFFER_SIZE];
                    ssize_t n = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);

                    if(n > 0)
                    {
                        buffer[n] = '\0';
                        Task task = {client_fd, std::string(buffer)};
                        {
                            std::lock_guard<std::mutex> lock(queue_mutex);
                            task_queue.push(task);
                        }

                        condition.notify_one();
                    }
                    else if(n == 0)
                    {
                        std::cout<<"Client disconnected!!\n";
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                        close(client_fd);
                        break;
                    }
                    else
                    {
                        if(errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            break;
                        }

                        perror("recv failed!!");
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                        close(client_fd);
                        break;
                    }
                }
            }
        }
        
    }

    for(int i = 0; i < NUM_THREADS; i++)
    {
        pthread_join(threads[i], nullptr);
    }

    close(server_fd);
    close(epoll_fd);
    std::cout<<"Server has shut down gracefully!!\n";
    return 0;
}