cmake_minimum_required(VERSION 3.10)
project(coro-httpd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall")

add_executable(coro-httpd
    main.cpp
    coroutine.cpp
    http_server.cpp
)

target_link_libraries(coro-httpd pthread)
