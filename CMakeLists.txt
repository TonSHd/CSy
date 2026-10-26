cmake_minimum_required(VERSION 3.16)
project(csy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra -Wno-unused-parameter")

include_directories(${CMAKE_SOURCE_DIR}/src)

set(CSY_SOURCES
    src/resource.cpp
    src/ccm.cpp
    src/osinfo.cpp
    src/lexer.cpp
    src/ast_printer.cpp
    src/parser.cpp
    src/project.cpp
    src/object_file.cpp
    src/linker.cpp
    src/explain.cpp
    src/diagnostic.cpp
    src/suggest.cpp
    src/semantic.cpp
    src/codegen.cpp
    src/ir.cpp
    src/value.cpp
    src/builtins_ext.cpp
    src/ai_ops.cpp
    src/pi.cpp
    src/gpu_nvidia.cpp
    src/native_jit.cpp
    src/tui.cpp
    src/vm.cpp
    src/main.cpp
)

add_executable(csyc ${CSY_SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(csyc PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
target_link_options(csyc PRIVATE -static-libstdc++ -static-libgcc)

# GPU
find_program(CSY_NVCC nvcc)
if(CSY_NVCC)
    set(CSY_CUDA_PLUGIN_DIR "${CMAKE_SOURCE_DIR}/third_party/csy_ai_cuda")
    set(CSY_CUDA_PLUGIN "${CSY_CUDA_PLUGIN_DIR}/libcsy_ai_cuda.so")
    add_custom_command(
        OUTPUT "${CSY_CUDA_PLUGIN}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CSY_CUDA_PLUGIN_DIR}"
        COMMAND "${CSY_NVCC}" -O3 -std=c++17 -Xcompiler=-fPIC -shared
                "${CMAKE_SOURCE_DIR}/src/ai_cuda_kernels.cu" -lcublas
                -o "${CSY_CUDA_PLUGIN}"
        DEPENDS "${CMAKE_SOURCE_DIR}/src/ai_cuda_kernels.cu"
        VERBATIM)
    add_custom_target(csy_ai_cuda DEPENDS "${CSY_CUDA_PLUGIN}")
    add_dependencies(csy_ai_cuda csyc)
else()
    message(STATUS "nvcc not found; CUDA AI training plugin target is unavailable")
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GMPXX QUIET IMPORTED_TARGET gmpxx)
endif()
if(GMPXX_FOUND)
    target_compile_definitions(csyc PRIVATE CSY_WITH_GMP=1)
    target_link_libraries(csyc PRIVATE PkgConfig::GMPXX)
else()
    message(STATUS "GMP C++ library not found; bc::pi_digits will report unavailable")
endif()

install(TARGETS csyc DESTINATION bin)
