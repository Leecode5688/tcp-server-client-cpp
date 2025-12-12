cmake_minimum_required(VERSION 3.10)

project(ReactorServer VERSION 4.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -g")

find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS chat.proto)

#------------ FOR SERVER --------------
set(SERVER_SOURCES
    main.cpp
    threadpool.cpp
    webserver.cpp
    ${PROTO_SRCS})

find_package(PkgConfig REQUIRED)

add_executable(chat_server ${SERVER_SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(chat_server PRIVATE 
    Threads::Threads 
    jemalloc
    ${Protobuf_LIBRARIES})

target_include_directories(chat_server PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

#------------ FOR CLIENT --------------
add_executable(chat_client
               main_client.cpp
               client.cpp
               ${PROTO_SRCS})   
target_link_libraries(chat_client PRIVATE Threads::Threads ${Protobuf_LIBRARIES})
target_include_directories(chat_client PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
#------------ FOR LOAD TESTER --------------
# add_executable(load_bot 
#                load_bot.cpp
#                ${PROTO_SRCS}) 
# target_link_libraries(load_bot PRIVATE 
#                       Threads::Threads
#                       ${Protobuf_LIBRARIES})
# target_include_directories(load_bot PRIVATE ${CMAKE_CURRENT_BINARY_DIR})