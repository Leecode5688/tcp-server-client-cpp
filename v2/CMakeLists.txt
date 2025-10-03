cmake_minimum_required(VERSION 3.10)

# Project name and language
project(ReactorServer VERSION 1.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#------------ FOR SERVER --------------
# Add source files
set(SOURCES
    main.cpp
    threadpool.cpp
    webserver.cpp)

# Create executable
add_executable(echo_server ${SOURCES})

# Add include directories
target_include_directories(echo_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Extra compiler warnings
target_compile_options(echo_server PRIVATE -Wall -Wextra -pedantic)

# Link pthread library
find_package(Threads REQUIRED)
target_link_libraries(echo_server PRIVATE Threads::Threads)

#------------ FOR CLIENT --------------
add_executable(chat_client chat_client.cpp)
target_compile_options(chat_client PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(chat_client PRIVATE Threads::Threads)
