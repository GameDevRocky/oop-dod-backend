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

option(DOD_BUILD_TESTS "Build unit tests" ON)
option(DOD_BUILD_BENCHMARKS "Build the benchmark" ON)
option(DOD_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)

function(dod_apply_sanitizers target)
    if(DOD_ENABLE_SANITIZERS)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        elseif(MSVC)
            target_compile_options(${target} PRIVATE /fsanitize=address)
        else()
            message(WARNING "Sanitizers are not supported by this compiler configuration")
        endif()
    endif()
endfunction()

if(DOD_BUILD_TESTS)
    include(CTest)
    find_package(Threads REQUIRED)
    add_executable(dod_tests tests/tests.cpp)
    target_link_libraries(dod_tests PRIVATE dod::dod Threads::Threads)
    dod_apply_sanitizers(dod_tests)
    add_test(NAME dod_tests COMMAND dod_tests)
endif()
if(DOD_BUILD_BENCHMARKS)
    add_executable(dod_benchmark benchmarks/benchmark.cpp)
    target_link_libraries(dod_benchmark PRIVATE dod::dod)
    dod_apply_sanitizers(dod_benchmark)
endif()
