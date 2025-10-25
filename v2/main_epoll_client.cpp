#include "client.h"
#include <iostream>
#define PORT 8888
#define SERVER_IP "127.0.0.1"

int main()
{
    Client client(SERVER_IP, PORT);
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