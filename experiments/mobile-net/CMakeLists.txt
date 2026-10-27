cmake_minimum_required(VERSION 3.24)
project(mta_mobile_net_readiness LANGUAGES C CXX)

include(FetchContent)
# Tag v1.3.18 was resolved through the official GitHub API; hash pins the archive too.
FetchContent_Declare(enet
    URL https://codeload.github.com/lsalzman/enet/tar.gz/2662c0de09e36f2a2030ccc2c528a3e4c9e8138a
    URL_HASH SHA256=37381831c5ccbb41c3c71e867203fee847670f67717f297f930290d18985ad5f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(enet)

add_executable(wire_tests wire_tests.cpp)
add_executable(net_probe net_probe.cpp)
foreach(target wire_tests net_probe)
    target_compile_features(${target} PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()
target_include_directories(net_probe PRIVATE ${enet_SOURCE_DIR}/include)
target_link_libraries(net_probe PRIVATE enet)
if(WIN32)
    target_link_libraries(net_probe PRIVATE ws2_32 winmm)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()
add_test(NAME wire_contract COMMAND wire_tests)
add_test(NAME enet_three_process_loopback
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/smoke.py
        --probe $<TARGET_FILE:net_probe> --evidence ${CMAKE_CURRENT_BINARY_DIR}/evidence/loopback.json)
set_tests_properties(enet_three_process_loopback PROPERTIES TIMEOUT 45 PROCESSORS 2)
