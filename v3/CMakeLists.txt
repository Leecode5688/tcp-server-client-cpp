cmake_minimum_required(VERSION 3.10)

project(ReactorServer VERSION 3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -g")

#------------ FOR SERVER --------------
set(SERVER_SOURCES
    main.cpp
    threadpool.cpp
    webserver.cpp)

add_executable(chat_server ${SERVER_SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(chat_server PRIVATE Threads::Threads)

#------------ FOR CLIENT --------------
add_executable(epoll_client
               main_epoll_client.cpp
               client.cpp)   
