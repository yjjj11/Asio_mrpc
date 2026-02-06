cmake_minimum_required(VERSION 3.6)
project(server_register)

add_definitions(-DPROJECT_NAME="${PROJECT_NAME}" )
include_directories( ${PROJECT_SOURCE_DIR}/src/ )
file(GLOB SOURCE_FILES ./src/*.cpp ./src/*.h ./src/server/*.cpp ./src/server/*.h)

add_executable(${PROJECT_NAME} ${SOURCE_FILES})
if(CMAKE_HOST_UNIX)
    target_link_libraries(${PROJECT_NAME} pthread)
endif()
