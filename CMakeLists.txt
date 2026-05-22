cmake_minimum_required(VERSION 3.6)

project(mrpc)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate compile_commands.json for clangd" FORCE)
add_definitions(-DASIO_STANDALONE -DRPC_USE_LOG -D_LOG_CONSOLE)

add_definitions(-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_OFF)
if(CMAKE_HOST_WIN32)
    add_definitions(-D_WIN32_WINNT=0x0701 -DNTDDI_VERSION=NTDDI_WIN7 )
    add_definitions(/std:c++17 /utf-8)
else()
    add_definitions(-g -O0 -Wall -std=c++2a)
    # 查找pthread线程库（Linux/macOS）
    find_package(Threads REQUIRED)
endif()

set(EXECUTABLE_OUTPUT_PATH      ${CMAKE_HOME_DIRECTORY}/bin/)
set(RPC_INCLUDE_DIR             ${CMAKE_HOME_DIRECTORY}/include/)
set(ASIO_INCLUDE_DIR            ${CMAKE_HOME_DIRECTORY}/third/asio-1.18.1/include/)
set(SPDLOG_INCLUDE_DIR          ${CMAKE_HOME_DIRECTORY}/third/spdlog/include/)
set(WLOG_INCLUDE_DIR            ${CMAKE_HOME_DIRECTORY}/third/wlog/include/)
set(NLOHMANN_INCLUDE_DIR        ${CMAKE_HOME_DIRECTORY}/third/nlohmann/include/)
set(SQLITE_ORM_INCLUDE_DIR      ${CMAKE_HOME_DIRECTORY}/third/sqlite_orm/include/)

message(VERBOSE "rpc        directory: " ${RPC_INCLUDE_DIR})
message(VERBOSE "asio       directory: " ${ASIO_INCLUDE_DIR})
message(VERBOSE "spdlog     directory: " ${SPDLOG_INCLUDE_DIR})
message(VERBOSE "wlog       directory: " ${WLOG_INCLUDE_DIR})
message(VERBOSE "nlohmann   directory: " ${NLOHMANN_INCLUDE_DIR})
include_directories(
    ${RPC_INCLUDE_DIR}
    ${ASIO_INCLUDE_DIR}
    ${SPDLOG_INCLUDE_DIR}
    ${WLOG_INCLUDE_DIR}
    ${NLOHMANN_INCLUDE_DIR}
    ${SQLITE_ORM_INCLUDE_DIR}
)

link_libraries(
        Threads::Threads
)

# 递归查找example下的所有子项目
# add_subdirectory(${CMAKE_HOME_DIRECTORY}/example/async_call_client)
# add_subdirectory(${CMAKE_HOME_DIRECTORY}/example/block_call_client)
add_subdirectory(${CMAKE_HOME_DIRECTORY}/example/coro_call_client)
add_subdirectory(${CMAKE_HOME_DIRECTORY}/example/server_register)
# add_subdirectory(${CMAKE_HOME_DIRECTORY}/example/two_way_call)

# 聊天室服务器
add_executable(chat_server ${CMAKE_HOME_DIRECTORY}/chatting_room/server.cpp)
target_include_directories(chat_server PRIVATE ${CMAKE_HOME_DIRECTORY}/chatting_room)
target_link_libraries(chat_server PRIVATE hiredis sqlite3)
# 聊天室客户端
add_executable(chat_client ${CMAKE_HOME_DIRECTORY}/chatting_room/client.cpp)
