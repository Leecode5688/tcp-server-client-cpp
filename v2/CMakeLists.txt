cmake_minimum_required(VERSION 3.10)

# Project name and language
project(ReactorServer VERSION 2.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")

#------------ FOR SERVER --------------
# Add source files
set(SERVER_SOURCES
    main.cpp
    connection.h
    threadpool.h
    threadpool.cpp
    webserver.h
    webserver.cpp
    )

# Create executable
add_executable(chat_server ${SERVER_SOURCES})

# Link pthread library
find_package(Threads REQUIRED)
target_link_libraries(chat_server PRIVATE Threads::Threads)

#------------ FOR CLIENT --------------
# the multi-threaded client
add_executable(chat_client_mt chat_client.cpp)
target_link_libraries(chat_client_mt PRIVATE Threads::Threads)

# the epoll client
add_executable(epoll_client
    main_epoll_client.cpp
    client.cpp
    client.h
)