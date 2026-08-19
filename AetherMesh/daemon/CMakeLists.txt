cmake_minimum_required(VERSION 3.20)

project(aetherd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(aetherd
	src/main.cpp
	src/ipc.cpp
	src/socket_manager.cpp
	src/discovery.cpp
	src/sha256.cpp
)

target_include_directories(aetherd PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(aetherd PRIVATE -Wall -Wextra -O2 -pthread)
target_link_libraries(aetherd PRIVATE dl pthread)
