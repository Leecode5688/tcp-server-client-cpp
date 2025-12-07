#include "chatapp.h"
#include <iostream>
#include <string>
#include "utils.h"

int main(int argc, char** argv)
{
    int port = 8888;
    if(argc > 1)
    {
        port = std::stoi(argv[1]);
    }
    std::cout << "=======================================\n";
    std::cout << " Starting Generic Reactor Engine v3.2  \n";
    std::cout << " Application: ChatApp                  \n";
    std::cout << " Protocol:    Length-Prefix Binary     \n";
    std::cout << " Port:        " << port << "\n";
    std::cout << "=======================================\n";    

    LOG_INFO("Starting ChatApp on port " + std::to_string(port));
    LOG_INFO("Protocol: Length-Prefix Binary");

    try 
    {
        ChatApp app(port);
        app.Run();
    }
    catch(const std::exception& e)
    {
        std::cerr<<"Fatal error: " << e.what() << std::endl;
        LOG_ERROR("Fatal error: " + std::string(e.what()));
        return 1;
    }

    LOG_INFO("Server shut down gracefully.");
    return 0;
}