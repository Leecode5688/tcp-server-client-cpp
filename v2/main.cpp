#include "webserver.h"

#define PORT 8888
#define DEFAULT_WORKERS 5

int main(int argc, char** argv)
{
    int workers = DEFAULT_WORKERS;
    //parse command line argument for number of worker threads
    if(argc > 1)
    {
        workers = std::stoi(argv[1]);
    }

    WebServer server(PORT, workers);
    server.run();
    return 0;
}