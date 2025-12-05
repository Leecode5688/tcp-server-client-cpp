#include "chatapp.h"
#include <iostream>

int main(int argc, char** argv)
{
    int port = 8888;
    if(argc > 1)
    {
        port = std::stoi(argv[1]);
    }
    std::cout<<"Starting ChatApp on port: "<<port<<"....\n";
    ChatApp app(port);
    app.Run();


    return 0;
}