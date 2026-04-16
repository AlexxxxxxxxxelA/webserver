cmake_minimum_required(VERSION 3.16)

project(mywebserver VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(mywebserver_lib
    src/Acceptor.cpp
    src/AgentDemo.cpp
    src/Buffer.cpp
    src/Channel.cpp
    src/CurrentThread.cpp
    src/DefaultPoller.cpp
    src/EPollPoller.cpp
    src/EventLoop.cpp
    src/EventLoopThread.cpp
    src/EventLoopThreadPool.cpp
    src/InetAddress.cpp
    src/Poller.cpp
    src/Socket.cpp
    src/TcpConnection.cpp
    src/TcpServer.cpp
    src/Thread.cpp
    src/ThreadPool.cpp
    src/Timer.cpp
    src/TimerQueue.cpp
    src/Timestamp.cpp
)

target_include_directories(mywebserver_lib
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/include/mywebserver
)

add_executable(mywebserver
    src/main.cpp
)

target_link_libraries(mywebserver
    PRIVATE
        mywebserver_lib
)
