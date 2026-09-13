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

set(DOD_ENABLE_HANDLE_CHECKS "AUTO" CACHE STRING
    "Handle validation mode: AUTO, ON, or OFF")
set_property(CACHE DOD_ENABLE_HANDLE_CHECKS PROPERTY STRINGS AUTO ON OFF)

if(DOD_ENABLE_HANDLE_CHECKS STREQUAL "ON")
    target_compile_definitions(
        oop_dod_backend INTERFACE DOD_ENABLE_HANDLE_CHECKS=1)
elseif(DOD_ENABLE_HANDLE_CHECKS STREQUAL "OFF")
    target_compile_definitions(
        oop_dod_backend INTERFACE DOD_ENABLE_HANDLE_CHECKS=0)
elseif(DOD_ENABLE_HANDLE_CHECKS STREQUAL "AUTO")
    target_compile_definitions(
        oop_dod_backend
        INTERFACE
            $<$<CONFIG:Debug>:DOD_ENABLE_HANDLE_CHECKS=1>
            $<$<NOT:$<CONFIG:Debug>>:DOD_ENABLE_HANDLE_CHECKS=0>)
else()
    message(FATAL_ERROR
        "DOD_ENABLE_HANDLE_CHECKS must be AUTO, ON, or OFF")
endif()
