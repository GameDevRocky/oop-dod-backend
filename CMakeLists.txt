cmake_minimum_required(VERSION 3.20)
project(oop_dod_backend VERSION 0.1.0 LANGUAGES CXX)

add_library(oop_dod_backend INTERFACE)
add_library(dod::dod ALIAS oop_dod_backend)
target_include_directories(
    oop_dod_backend
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_compile_features(oop_dod_backend INTERFACE cxx_std_20)
