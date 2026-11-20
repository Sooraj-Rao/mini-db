cmake_minimum_required(VERSION 3.20)
project(MiniDB VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(minidb_c_engine src/c_engine/storage_engine.c)
target_include_directories(minidb_c_engine PUBLIC src/c_engine)

add_library(minidb_storage src/storage.cpp)
target_include_directories(minidb_storage PUBLIC src)
target_link_libraries(minidb_storage PUBLIC minidb_c_engine)

if(MSVC)
    target_compile_options(minidb_storage PRIVATE /W4)
    target_compile_options(minidb_c_engine PRIVATE /W4)
else()
    target_compile_options(minidb_storage PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(minidb_c_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(minidb src/main.cpp)
target_link_libraries(minidb PRIVATE minidb_storage)

add_executable(minidb_server src/server.cpp)
target_link_libraries(minidb_server PRIVATE minidb_storage)

add_executable(minidb_client src/client.cpp)
if(WIN32)
    target_link_libraries(minidb_server PRIVATE ws2_32)
    target_link_libraries(minidb_client PRIVATE ws2_32)
endif()

add_executable(minidb_tests tests/test_storage.cpp)
target_link_libraries(minidb_tests PRIVATE minidb_storage)

add_executable(minidb_bench benchmarks/benchmark.cpp)
target_link_libraries(minidb_bench PRIVATE minidb_storage)

add_executable(minidb_c_api_tests tests/test_c_api.c)
target_link_libraries(minidb_c_api_tests PRIVATE minidb_c_engine)

add_executable(minidb_concurrency_tests tests/test_concurrency.cpp)
target_link_libraries(minidb_concurrency_tests PRIVATE minidb_storage)

add_executable(minidb_integration_stress tests/test_integration_stress.cpp)
target_link_libraries(minidb_integration_stress PRIVATE minidb_storage)

enable_testing()
add_test(NAME minidb_storage_tests COMMAND minidb_tests)
add_test(NAME minidb_c_api_tests COMMAND minidb_c_api_tests)
add_test(NAME minidb_concurrency_tests COMMAND minidb_concurrency_tests)
add_test(NAME minidb_integration_stress COMMAND minidb_integration_stress)
