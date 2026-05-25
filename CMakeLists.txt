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

# ImGui + SDL2 桌面 GUI 客户端
find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
file(GLOB IMGUI_SOURCES ${CMAKE_HOME_DIRECTORY}/third/imgui/imgui.cpp
    ${CMAKE_HOME_DIRECTORY}/third/imgui/imgui_draw.cpp
    ${CMAKE_HOME_DIRECTORY}/third/imgui/imgui_tables.cpp
    ${CMAKE_HOME_DIRECTORY}/third/imgui/imgui_widgets.cpp
    ${CMAKE_HOME_DIRECTORY}/third/imgui/imgui_impl_sdl2.cpp
    ${CMAKE_HOME_DIRECTORY}/third/imgui/imgui_impl_opengl3.cpp
)
add_executable(chat_gui
    ${CMAKE_HOME_DIRECTORY}/chatting_room/client_gui.cpp
    ${IMGUI_SOURCES}
)
target_include_directories(chat_gui PRIVATE
    ${CMAKE_HOME_DIRECTORY}/chatting_room
    ${CMAKE_HOME_DIRECTORY}/third/imgui
)
target_link_libraries(chat_gui PRIVATE
    SDL2::SDL2
    OpenGL::GL
)

# 分布式服务节点（直连 Redis，水平可扩展）
add_executable(server_node ${CMAKE_HOME_DIRECTORY}/chatting_room/server_node.cpp)
target_include_directories(server_node PRIVATE ${CMAKE_HOME_DIRECTORY}/chatting_room)
target_link_libraries(server_node PRIVATE hiredis sqlite3 mysqlclient)

# 持久化服务（独立进程，各 server_node 通过 mRPC 调用）
add_executable(sqlite_service ${CMAKE_HOME_DIRECTORY}/chatting_room/sqlite_service.cpp)
target_include_directories(sqlite_service PRIVATE ${CMAKE_HOME_DIRECTORY}/chatting_room)
target_link_libraries(sqlite_service PRIVATE hiredis mysqlclient)

# Echo 服务（用于压力测试纯 RPC 吞吐）
add_executable(echo_server ${CMAKE_HOME_DIRECTORY}/chatting_room/echo_server.cpp)
target_include_directories(echo_server PRIVATE ${CMAKE_HOME_DIRECTORY}/chatting_room)

# 压力测试
add_executable(stress_test ${CMAKE_HOME_DIRECTORY}/chatting_room/stress_test.cpp)
target_include_directories(stress_test PRIVATE ${CMAKE_HOME_DIRECTORY}/chatting_room)
