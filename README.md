## C++ TCP Server-Client: From Blocking I/O to a Multi-threaded Reactor

### Overview: 

**Version 1: The Foundation Model** 
A single-threaded TCP echo server that uses **blocking I/O**. This version serves as a baseline to understand the limitation of a basic server architecture. 

**Version 2: The Reactor Model**
A multi-threaded TCP chat server built on the **Reactor design pattern**. It uses **non-blocking I/O** with **epoll** and a thread pool to handle multiple clients concurrently. 

### Part 1: The Foundation - echoServerV1

echoServerV1 is the most elementary server architecture. It’s a single-threaded, iterative server that handles one client connection at a time. Its performance is significantly constrained by **blocking I/O** operations.

#### What is blocking and non-blocking I/O

A classic analogy for explaining **blocking I/O** and **non-blocking I/O** is ordering coffee at a shop. In the case of **blocking I/O**, after the barista takes your order, you must stand at the counter and wait until your coffee is ready. What’s worse is that you can’t do anything else while waiting; you are “blocked” from checking your phone, chatting to a friend, or stepping away. 

**Non-blocking I/O**, on the other hand, is like ordering your coffee with a buzzer. The barista takes your order and hands you a buzzer that will vibrate when your drink is ready. You are now free to return to your table, read your book, or browse your phone. You are not “blocked” this time, just need to keep an eye out for the buzzer’s signal. 

In network programming, I/O (Input/Output) operations refer to reading data from or writing data to a socket. 

**Blocking I/O**: When your program makes a blocking I/O call, its execution is paused, and it waits until the I/O operation (like receiving data) is complete. 
**Non-blocking I/O**: When your program makes a non-blocking I/O call, the call returns immediately, whether the operation is complete or not. If there is no data to read, it tells your program to try again later. 

With these concepts in mind, let’s see how the blocking model is implemented in the echoServerV1 code.

#### The Sequential Flow of echoServerV1

The echoServerV1 code demonstrates a classic, linear server setup. Due to its blocking nature, the program executes in a predictable, step-by-step manner, waiting for each system call to complete before proceeding to the next. 

The sequence of system calls is as follows: 

1. socket(): The server first creates a new socket, which is the endpoint for communication. This call asks the kernel for a file descriptor that represents the socket. 
2. bind(): The newly created socket is then bound to a specific IP address and port. In this case, it’s bound to port 8888 on all available network interfaces (INADDR_ANY). 
3. listen(): This call marks the socket as passive,  meaning that it will be used to accept incoming connection requests. 
4. accept(): This is a **blocking call**. The server’s execution stops here and waits indefinitely for a client to connect. 
5. recv(): Once a client is connected, the server enters a loop and calls recv(). This is another **blocking** call, meaning that the server waits here until the client sends data. 
6. send(): After receiving data, the server sends it back to the client (since this is an echo server). This call may also block if the network buffer is full. 

#### Limitations of the v1 Model

This simple architecture relies heavily on **blocking** system calls. As a result, if the server gets stuck in the recv() loop, it cannot accept new clients, even if there are pending connections in the backlog queue. The server remains blocked until the current client interaction completes. This fundamental limitation makes the single-threaded, blocking model unsuitable for any real world application that needs to handle multiple users. We need more advanced server architectures. 

#### Alternative Architecture: The Thread-per-Connection Model

To handle multiple clients concurrently, a common approach is the thread-per-connection model. In this architecture, the main server thread remains in a loop, blocking only on the accept() call. As soon as a new connection is accepted, the server spawns a dedicated worker thread to handle that client. The main thread then immediately loops back to accept() and waits for the next connection. 

This architecture has two primary advantages. First is programming simplicity; the worker thread can use **blocking I/O** calls (recv(), send()) as if it were a single-client server (just like v1). This avoids the complexity of managing multiple I/O states. Second, it provides client isolation. Since each client runs in its own thread, a slow or unresponsive connection won’t block the entire server process or affect other active clients.

However, this model also suffers from severe scalability limitations. As the number of clients grows, the server can quickly run out of memory or hit the operating system’s limit on the number of threads. Additionally, the operating system must spend CPU cycles on context switching between threads, which can degrade performance under heavy load.

### Part 2: A Non-Blocking, Multithreaded Reactor Model (v2)

This project’s v2 overcomes these limitations by implementing a more advanced and efficient architecture: the Reactor design pattern. This model uses **non-blocking I/O** combined with an **I/O multiplexing** mechanism (**epoll**) and a thread pool. 

#### What is I/O multiplexing 

From our previous discussion, it’s clear that we need to switch from **blocking** I/O to **non-blocking** I/O. While **non-blocking** I/O prevents the server from getting stuck on a single client, it introduces a new challenge: How do we know when a socket is ready to be read from or written to? A naive approach would be to loop through all active connections and try to read from each of them: 

```

while(server_is_running)
{  
    for(auto& connection : all_connnections)
    {  
        //non-blocking calls returns immediately, we have to keep polling
         int bytes_read = recv([connection.fd](http://connection.fd), buffer, size, 0);
         if(bytes_read > 0)
         {
             //handle the data    
         }
    }

```

This technique is called busy waiting. The loop wastes CPU cycles by repeatedly asking “Is there data yet?” for every single connection, and most of which has nothing to process. In our coffee shop analogy, it’s like a weird customer with a constantly buzzing pager, pestering the poor barista every few seconds: “Is my coffee ready? Is it ready now? How about now? Still not ready? …..”. The barista can’t focus on making coffee because she’s constantly interrupted, just like the CPU can’t work efficiently under busy waiting. 

This is where **I/O multiplexing** comes in. **I/O multiplexing** is like ordering americano, latte, espresso macchiato, etc,  several kinds of coffee at once. Instead of actively waiting at the counter, we can sit down and relax because this time the kind barista will notify us when any of our orders are ready. 

This follows the “Holloywood Principle” - Don’t call us, we’ll call you. Rather than our program constantly polling the kernel to ask if a socket is ready, we hand over the monitoring responsibility. This is a form of event-driven programming. Our server’s main loop simply waits for the kernel to notify it of an event. In Linux, this idea is implemented using system calls like epoll(), which allows a single thread to efficiently wait for events on thousands of sockets.

#### The Reactor Pattern: An Architecture for Event-Driven Systems

The v1 echoServer operates on a procedural, linear flow. It’s a simple set of instructions that runs in order, blocks at accept() and recv(), waiting for something to happen. The v2 echoServer, on the other hand, is built with a more efficient event-driven logic. This architecture is known as the **Reactor Pattern**, and it’s core logics are: 

1. Central Event Loop: The run() method in the WebServer class contains a while(running_) loop, this loop’s only job is to wait for and dispatch events.
2. Waiting for Events: The loop spends almost all its time blocked on a single epoll_wait() function call. It’s not actively doing anything, just need to passively wait for the kernel to notify it. 
3. Reacting to Events: When epoll_wait() returns, the server inspects the list of events (the ready list of epoll) and reacts accordingly, and dispatch the tasks to proper handler functions. 

In v2, there are four kinds of epoll events, each corresponding to a different file descriptor type being monitored: 

1. A New Connection Event on listen_fd_: When a client attempts to connect, the listen socket becomes readable (epoll will report this as an EPOLLIN event). The Reactor receives this event and calls the accept_loop() handler. This function will accept all pending new connections, create a Connection object for each one, set their sockets to non-blocking, and add them to the epoll instance to be monitored for future events. 
2. A Client Socket Event: 
	- EPOLLIN (data to read): If a client sends data, the EPOLLIN event will also be triggered. The Reactor then dispatches this event to the handle_read() function, which reads the data from the socket into a buffer. Once a complete message is complete (separated by a newline character \n), it’s pushed as a task to the thread pool for processing. 
	- EPOLLOUT (ready to write): This event signifies that the network buffer for a socket is clear and ready to accept more data. The Reactor calls handle_write(), which sends any pending data from that connection’s output buffer. 
	- EPOLLHUP/ EPOLLERR (connection closed or error): If a client disconnects or an error occurs, the Reactor calls close_conn() to clean up the connection, remove it from the epoll instance, and free its resources. 
3. A Signal Event on signal_fd_: This kind of event is for graceful shutdown. Signals like SIGINT (Ctrl+C) are captured by signalfd. When this event occurs, the main loop sets the running_ flag to false, which causes the event loop to terminate after handling the current events. 
4. A Notification Event on notify_fd_: When a worker thread has a message ready for broadcast, it writes to the notify_fd_. The Reactor sees this event and calls handle_broadcasts() to send the message to all connected clients.
---

### Getting Started

Prerequisites:
- A C++ 17 compliant compiler
- CMake (for v2)

For v1: 
Navigate to the v1 directory
Compile the server: 
```
g++ -std=c++17 -Wall -Wextra -pedantic -o server echoServerV1.cpp
```
Compile the client: 
```
g++ -std=c++17 -Wall -Wextra -pedantic -o client clientV1.cpp
```

For v2: 
Navigate to the v2 directory
Create and enter a build directory:
```
mkdir build && cd build
```
Run CMake to generate the build files: 
```
cmake ..
```
Compile the project: 
```
make
```
