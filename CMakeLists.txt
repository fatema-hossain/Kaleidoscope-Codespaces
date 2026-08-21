cmake_minimum_required(VERSION 3.20)
project(kaleidoscope LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(LLVM_LIBS
  Core
  ExecutionEngine
  OrcJIT
  RuntimeDyld
  native
  nativecodegen
  Support
)

add_executable(kaleidoscope src/main.cpp)
target_link_libraries(kaleidoscope PRIVATE ${LLVM_LIBS})

enable_testing()
add_test(NAME arithmetic COMMAND bash ${CMAKE_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:kaleidoscope>)

if(UNIX AND NOT APPLE)
  target_link_options(kaleidoscope PRIVATE "-Wl,--export-dynamic")
endif()
