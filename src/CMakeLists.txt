set(WEBSERVER_CORE_SOURCES
    Acceptor.cc
    AgentDemo.cc
    AgentRuntime.cc
    BoundedThreadPool.cc
    Buffer.cc
    Channel.cc
    CurrentThread.cc
    ConversationContext.cc
    DefaultPoller.cc
    DeepSeekProtocol.cc
    EPollPoller.cc
    EventLoop.cc
    EventLoopThread.cc
    EventLoopThreadPool.cc
    HttpContext.cc
    HttpRequest.cc
    HttpResponse.cc
    HttpServer.cc
    InetAddress.cc
    Logger.cc
    Poller.cc
    Socket.cc
    TcpConnection.cc
    TcpServer.cc
    Thread.cc
    Timer.cc
    TimerQueue.cc
    Timestamp.cc
    ${PROJECT_SOURCE_DIR}/log/AsyncLogging.cc
    ${PROJECT_SOURCE_DIR}/log/FileUtil.cc
    ${PROJECT_SOURCE_DIR}/log/LogFile.cc
    ${PROJECT_SOURCE_DIR}/log/LogStream.cc
    ${PROJECT_SOURCE_DIR}/memory/memoryPool.cc
)

add_library(webserver_core STATIC ${WEBSERVER_CORE_SOURCES})
target_include_directories(webserver_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(webserver_core
    PUBLIC
        Threads::Threads
        CURL::libcurl
        nlohmann_json::nlohmann_json
        SQLite::SQLite3
)

add_executable(main main.cc)
target_link_libraries(main PRIVATE webserver_core)

