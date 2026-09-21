cmake_minimum_required(VERSION 3.18)

# V100 compute capability; initialize before CUDA compiler detection.
set(CMAKE_CUDA_ARCHITECTURES 70)
project(CGHV100Backend
    VERSION 1.1.0
    LANGUAGES CXX CUDA
)

find_package(Threads REQUIRED)
add_library(cgh_wire INTERFACE)
target_include_directories(cgh_wire INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(cgh_wire INTERFACE cxx_std_17)
add_executable(cgh_v100_server
    src/main.cpp
    src/solver/DummyPointFocus.cpp
    src/solver/CudaPointFocus.cu
)
set_target_properties(cgh_v100_server PROPERTIES
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    CUDA_SEPARABLE_COMPILATION ON
)
target_link_libraries(cgh_v100_server PRIVATE cgh_wire Threads::Threads)
# Host warning flags are not nvcc command-line options.
target_compile_options(cgh_v100_server PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>"
    $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false>
)
install(TARGETS cgh_v100_server RUNTIME DESTINATION bin)

include(CTest)
option(CGH_ENABLE_CUDA_TESTS "Run numerical PointFocus tests on the visible CUDA GPU" OFF)
if(BUILD_TESTING)
    add_executable(cgh_wire_tests tests/wire_tests.cpp)
    target_link_libraries(cgh_wire_tests PRIVATE cgh_wire)
    target_compile_options(cgh_wire_tests PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
    add_test(NAME wire_codec COMMAND cgh_wire_tests)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME tcp_integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:cgh_v100_server>)
    set_tests_properties(tcp_integration PROPERTIES TIMEOUT 40)
    if(CGH_ENABLE_CUDA_TESTS)
        add_test(NAME cuda_point_focus COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda_integration.py $<TARGET_FILE:cgh_v100_server>)
        set_tests_properties(cuda_point_focus PROPERTIES TIMEOUT 60)
    endif()
endif()
