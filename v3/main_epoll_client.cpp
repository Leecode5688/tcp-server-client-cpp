#include "client.h"
#include <iostream>
#include <cstdlib>

#define PORT 8888
// #define SERVER_IP "127.0.0.1"

int main()
{
    const char* env_ip = std::getenv("SERVER_IP");
    std::string server_ip = env_ip ? env_ip : "127.0.0.1";

    Client client(server_ip, PORT);
    if(client.connect_to_server())
    {
        if(client.login())
        {
            client.run();
        }
    }
    std::cout<<"\nDisconnected!!"<<std::endl;
    return 0;
}