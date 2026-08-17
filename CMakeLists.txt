cmake_minimum_required(VERSION 3.20)
project(ZeroCopyIPC CXX)

# Force C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Use g++-13 if available
set(CMAKE_CXX_COMPILER "g++-13")

add_subdirectory(framework)
add_subdirectory(developer)