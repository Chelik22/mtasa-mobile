cmake_minimum_required(VERSION 3.24)
project(mta_android_headless_protocol LANGUAGES C CXX)

# This subproject reuses H1 packet/transport sources without registering host tests
# or pulling the Windows DLL/server targets into an Android configure.
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "x86_64")
    message(FATAL_ERROR "Use the installed Android NDK toolchain with ANDROID_ABI=x86_64")
endif()
if(NOT ANDROID_PLATFORM STREQUAL "android-26" OR NOT ANDROID_STL STREQUAL "c++_static")
    message(FATAL_ERROR "This experiment is pinned to android-26 and c++_static")
endif()
set(MTA_ENET_SOURCE "" CACHE PATH "Existing ENet source matching the pinned repository manifest")
if(NOT IS_DIRECTORY "${MTA_ENET_SOURCE}")
    message(FATAL_ERROR "MTA_ENET_SOURCE must name the existing cached ENet source; downloads are not supported")
endif()

get_filename_component(MODULE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(REPOSITORY_ROOT "${MODULE_ROOT}/../.." ABSOLUTE)
set(ENET_MANIFEST "${REPOSITORY_ROOT}/experiments/mobile-net/android/verification/dependency.json")
file(READ "${ENET_MANIFEST}" ENET_PROVENANCE)
string(JSON ENET_FILE_COUNT LENGTH "${ENET_PROVENANCE}" files)
string(JSON ENET_EXPECTED_COUNT GET "${ENET_PROVENANCE}" comparedFiles)
if(NOT ENET_FILE_COUNT EQUAL 38 OR NOT ENET_FILE_COUNT EQUAL ENET_EXPECTED_COUNT)
    message(FATAL_ERROR "Pinned ENet file manifest must contain all 38 archive files")
endif()
math(EXPR ENET_LAST_FILE "${ENET_FILE_COUNT} - 1")
foreach(INDEX RANGE 0 ${ENET_LAST_FILE})
    string(JSON RELATIVE_FILE MEMBER "${ENET_PROVENANCE}" files ${INDEX})
    string(JSON EXPECTED_HASH GET "${ENET_PROVENANCE}" files "${RELATIVE_FILE}")
    if(NOT EXISTS "${MTA_ENET_SOURCE}/${RELATIVE_FILE}")
        message(FATAL_ERROR "Missing pinned ENet source: ${RELATIVE_FILE}")
    endif()
    file(SHA256 "${MTA_ENET_SOURCE}/${RELATIVE_FILE}" ACTUAL_HASH)
    string(TOUPPER "${ACTUAL_HASH}" ACTUAL_HASH)
    if(NOT ACTUAL_HASH STREQUAL EXPECTED_HASH)
        message(FATAL_ERROR "ENet cache differs from pinned archive: ${RELATIVE_FILE}")
    endif()
endforeach()
message(STATUS "Verified all ${ENET_FILE_COUNT} pinned ENet source hashes; no download path exists")
add_subdirectory("${MTA_ENET_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/enet" EXCLUDE_FROM_ALL)

add_library(mta_bitstream STATIC "${MODULE_ROOT}/MtaBitStream.cpp" "${MODULE_ROOT}/ModuleRuntime.cpp")
target_compile_features(mta_bitstream PUBLIC cxx_std_20)
target_include_directories(mta_bitstream PUBLIC "${REPOSITORY_ROOT}/Shared/sdk" "${REPOSITORY_ROOT}/Server/sdk")

add_executable(headless_client "${MODULE_ROOT}/headless/main.cpp")
target_include_directories(headless_client PRIVATE "${MODULE_ROOT}" "${MTA_ENET_SOURCE}/include")
target_link_libraries(headless_client PRIVATE mta_bitstream enet)

add_executable(headless_codec_tests "${MODULE_ROOT}/headless/codec_tests.cpp")
target_include_directories(headless_codec_tests PRIVATE "${MODULE_ROOT}")
target_link_libraries(headless_codec_tests PRIVATE mta_bitstream)

add_executable(protocol_tests "${MODULE_ROOT}/protocol_tests.cpp")
target_compile_features(protocol_tests PRIVATE cxx_std_20)
target_include_directories(protocol_tests PRIVATE "${MODULE_ROOT}")

# Cross-compilation proves no runtime gate. Execute these binaries explicitly via
# the future owned ADB scenario; never invoke Android executables during configure.
