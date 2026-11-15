cmake_minimum_required(VERSION 3.20)
project(CabinServiceMiddleware VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(CABIN_BUILD_TESTS "Build CABIN tests" ON)
option(CABIN_ENABLE_SANITIZERS "Enable a sanitizer build" OFF)
set(CABIN_SANITIZER "address" CACHE STRING "Sanitizer to enable: address, thread, undefined")
option(CABIN_ENABLE_COVERAGE "Enable coverage instrumentation" OFF)
option(CABIN_ENABLE_FUZZ "Build libFuzzer targets (Clang only)" OFF)
option(CABIN_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(CABIN_ENABLE_SANITIZERS)
  if(CABIN_SANITIZER STREQUAL "thread")
    set(CABIN_SAN_FLAGS -fsanitize=thread)
  elseif(CABIN_SANITIZER STREQUAL "undefined")
    set(CABIN_SAN_FLAGS -fsanitize=undefined -fno-sanitize-recover=all)
  else()
    set(CABIN_SAN_FLAGS -fsanitize=address -fno-omit-frame-pointer)
  endif()
  add_compile_options(${CABIN_SAN_FLAGS})
  add_link_options(${CABIN_SAN_FLAGS})
endif()

if(CABIN_ENABLE_COVERAGE)
  add_compile_options(--coverage -fno-inline -fno-inline-functions)
  add_link_options(--coverage)
endif()

function(cabin_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(CABIN_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

find_package(Threads REQUIRED)

set(CABIN_CORE_SOURCES
  src/core/log.cpp
  src/core/trace.cpp
  src/core/frame.cpp
  src/core/runtime.cpp
  generated/cabin_serdes.cpp
  generated/cabin_can_map.cpp
)

add_library(cabin_core STATIC ${CABIN_CORE_SOURCES})
target_include_directories(cabin_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                            ${CMAKE_CURRENT_SOURCE_DIR}/generated)
target_link_libraries(cabin_core PUBLIC Threads::Threads)
cabin_set_warnings(cabin_core)

enable_testing()

if(CABIN_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  add_subdirectory(tests)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/broker)
  add_subdirectory(src)
endif()
