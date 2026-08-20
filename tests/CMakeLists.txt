add_executable(agent_runtime_test AgentRuntimeTest.cc)
target_link_libraries(agent_runtime_test PRIVATE webserver_core)

# 测试程序留在构建目录，避免把开发期二进制文件放进源码树的 bin/。
set_target_properties(agent_runtime_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

add_test(NAME agent_runtime_test COMMAND agent_runtime_test)

add_executable(conversation_context_test ConversationContextTest.cc)
target_link_libraries(conversation_context_test PRIVATE webserver_core)
set_target_properties(conversation_context_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)
add_test(NAME conversation_context_test COMMAND conversation_context_test)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME chat_client_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/ChatClientTest.py
                ${PROJECT_SOURCE_DIR}
    )

    add_test(
        NAME agent_http_integration_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/AgentHttpIntegrationTest.py
                $<TARGET_FILE:main>
                ${PROJECT_SOURCE_DIR}
    )
    set_tests_properties(agent_http_integration_test PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 20
    )

    add_test(
        NAME agent_sse_integration_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/AgentSseIntegrationTest.py
                $<TARGET_FILE:main>
                ${PROJECT_SOURCE_DIR}
    )
    set_tests_properties(agent_sse_integration_test PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 25
    )

    add_test(
        NAME conversation_restart_integration_test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/ConversationRestartIntegrationTest.py
                $<TARGET_FILE:main>
                ${PROJECT_SOURCE_DIR}
    )
    set_tests_properties(conversation_restart_integration_test PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 25
    )
endif()
