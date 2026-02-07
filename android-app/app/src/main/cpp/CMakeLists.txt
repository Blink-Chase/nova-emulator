cmake_minimum_required(VERSION 3.22.1)

project("nova_native")

# Creates a shared library named 'nova_native' from the source file
add_library(
        nova_native
        SHARED
        native-lib.cpp)

# Links standard Android libraries (like logging)
find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(
        nova_native
        ${log-lib}
        ${android-lib})