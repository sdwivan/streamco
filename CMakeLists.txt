cmake_minimum_required(VERSION 4.2)
project(streamco)

set(CMAKE_CXX_STANDARD 20)

add_executable(streamco main.cpp)

add_subdirectory(screencapture)
add_subdirectory(screendisplay)
add_subdirectory(chromecapture)
add_subdirectory(streamsender)
add_subdirectory(streamreceiver)
