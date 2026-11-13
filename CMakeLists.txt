cmake_minimum_required(VERSION 3.22.1)
project(FCT_Base LANGUAGES CXX)

include(platform/platform.cmake)
include(config/copy_headers.cmake)
include(config/set_utf8_compiler_encoding.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(FCT_ENABLE_TRACY "Enable Tracy profiler in FCT_Base" ON)
option(FCT_TRACY_ON_DEMAND "Enable on-demand profiling for Tracy" ON)

if(FCT_ENABLE_TRACY)
    set(TRACY_ENABLE ON CACHE BOOL "Enable profiling" FORCE)
else()
    set(TRACY_ENABLE OFF CACHE BOOL "Enable profiling" FORCE)
endif()

if(FCT_TRACY_ON_DEMAND)
    set(TRACY_ON_DEMAND ON CACHE BOOL "On-demand profiling" FORCE)
else()
    set(TRACY_ON_DEMAND OFF CACHE BOOL "On-demand profiling" FORCE)
endif()

set(TRACY_STATIC OFF CACHE BOOL "Build Tracy as shared library" FORCE)

if(NOT TARGET TracyClient)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/tracy/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/tracy" "${CMAKE_CURRENT_BINARY_DIR}/tracy")
    else()
        include(FetchContent)
        FetchContent_Declare(
            tracy
            GIT_REPOSITORY https://github.com/MaaBlock/tracy.git
            GIT_TAG master
        )
        FetchContent_MakeAvailable(tracy)
    endif()
endif()

file(GLOB_RECURSE FCT_BASE_HEADERS
    LIST_DIRECTORIES false
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
)
list(REMOVE_ITEM FCT_BASE_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/FCT_Base.h")

file(GLOB_RECURSE FCT_BASE_SOURCES
    LIST_DIRECTORIES false
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_library(FCT_Base STATIC
    ${FCT_BASE_SOURCES}
    ${FCT_BASE_HEADERS}
)

set_utf8_for_target(FCT_Base)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/FCT_Base")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/FCT_Base.h")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/FCT_Base.h" "${CMAKE_CURRENT_SOURCE_DIR}/include/FCT_Base.h" COPYONLY)
endif()

foreach(header_file ${FCT_BASE_HEADERS})
    file(RELATIVE_PATH relative_path "${CMAKE_CURRENT_SOURCE_DIR}/src" "${header_file}")
    get_filename_component(header_dir "${relative_path}" DIRECTORY)
    if(header_dir)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/FCT_Base/${header_dir}")
    endif()
    configure_file("${header_file}" "${CMAKE_CURRENT_SOURCE_DIR}/include/FCT_Base/${relative_path}" COPYONLY)
endforeach()

add_copy_target_headers(FCT_Base "${FCT_BASE_HEADERS}")

target_include_directories(FCT_Base PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

find_package(absl CONFIG REQUIRED)

target_link_libraries(FCT_Base PUBLIC
    Tracy::TracyClient
    absl::status
    absl::strings
)

function(target_link_fct_base_libraries target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "Target ${target_name} does not exist")
    endif()

    target_link_libraries(${target_name} FCT_Base)

    if(WIN32 AND TARGET TracyClient AND NOT TRACY_STATIC)
        get_target_property(_target_type ${target_name} TYPE)
        if(_target_type STREQUAL "EXECUTABLE" OR _target_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:TracyClient>
                $<TARGET_FILE_DIR:${target_name}>
                COMMENT "Copying TracyClient.dll for ${target_name}"
            )
        endif()
    endif()
endfunction()
