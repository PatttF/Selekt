cmake_minimum_required(VERSION 3.24)
project(selekt_android C CXX)

set(CMAKE_CXX_STANDARD 17)

# We include the parent directory's CMakeLists
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. build_selekt)
