#
# Project configuration
#
cmake_minimum_required(VERSION 3.13 FATAL_ERROR) # target_link_directories
project(tray VERSION 0.0.0
        DESCRIPTION "A cross-platform system tray library"
        HOMEPAGE_URL "https://app.lizardbyte.dev"
        LANGUAGES C CXX)

set(PROJECT_LICENSE "MIT")

set(TRAY_IS_TOP_LEVEL OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(TRAY_IS_TOP_LEVEL ON)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

# Add our custom CMake modules without overriding parent project settings.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

#
# Project optional configuration
#
option(BUILD_DOCS "Build documentation" ${TRAY_IS_TOP_LEVEL})
option(BUILD_TESTS "Build tests" ${TRAY_IS_TOP_LEVEL})
option(BUILD_EXAMPLE "Build example app" ${TRAY_IS_TOP_LEVEL})

# Generate 'compile_commands.json' for clang_complete
set(CMAKE_COLOR_MAKEFILE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig)

file(GLOB TRAY_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

set(TRAY_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/icons/icon.ico")
set(TRAY_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/icons/icon.png")
set(TRAY_ICON_SVG "${CMAKE_CURRENT_SOURCE_DIR}/icons/icon.svg")
set(TRAY_ICON_FILES
        "${TRAY_ICON_ICO}"
        "${TRAY_ICON_PNG}"
        "${TRAY_ICON_SVG}"
)

set(_TRAY_ICON_ICO "${TRAY_ICON_ICO}" CACHE INTERNAL "Default tray ICO icon path")
set(_TRAY_ICON_PNG "${TRAY_ICON_PNG}" CACHE INTERNAL "Default tray PNG icon path")
set(_TRAY_ICON_SVG "${TRAY_ICON_SVG}" CACHE INTERNAL "Default tray SVG icon path")

# Copy default tray icon files into the output directory of the specified target.
function(tray_copy_default_icons target_name)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "tray_copy_default_icons expected an existing target: ${target_name}")
    endif()

    foreach(icon_file IN LISTS TRAY_ICON_FILES)
        add_custom_command(TARGET "${target_name}" POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${icon_file}"
                "$<TARGET_FILE_DIR:${target_name}>"
                COMMENT "Copying ${icon_file} to $<TARGET_FILE_DIR:${target_name}>")
    endforeach()
endfunction()

if(WIN32)
    list(APPEND TRAY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/tray_windows.c")
else()
    if(UNIX)
        if(APPLE)
            find_library(COCOA Cocoa REQUIRED)
            list(APPEND TRAY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/tray_darwin.m")
        else()
            find_package(Qt6 COMPONENTS Widgets DBus Svg)
            if(Qt6_FOUND)
                set(TRAY_QT_VERSION 6)
            else()
                find_package(Qt5 REQUIRED COMPONENTS Widgets DBus Svg)
                set(TRAY_QT_VERSION 5)
            endif()
            set(TRAY_QT_VERSION  # cmake-lint: disable=C0103
                    "${TRAY_QT_VERSION}"
                    CACHE INTERNAL "Qt major version selected by tray"
            )
            set(CMAKE_AUTOMOC ON)
            list(APPEND TRAY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/tray_linux.cpp")
        endif()
    endif()
endif()

add_library(${PROJECT_NAME} STATIC ${TRAY_SOURCES})
set_property(TARGET ${PROJECT_NAME} PROPERTY C_STANDARD 99)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
target_include_directories(${PROJECT_NAME}
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>)

if(WIN32)
    if(MSVC)
        list(APPEND TRAY_COMPILE_OPTIONS "/MT$<$<CONFIG:Debug>:d>")
    endif()
else()
    if(UNIX)
        if(APPLE)
            list(APPEND TRAY_EXTERNAL_LIBRARIES ${COCOA})
        else()
            if(TRAY_QT_VERSION EQUAL 6)
                list(APPEND TRAY_EXTERNAL_LIBRARIES Qt6::Widgets Qt6::DBus Qt6::Svg)
            else()
                list(APPEND TRAY_EXTERNAL_LIBRARIES Qt5::Widgets Qt5::DBus Qt5::Svg)
            endif()
        endif()
    endif()
endif()

add_library(tray::tray ALIAS ${PROJECT_NAME})

if(BUILD_EXAMPLE)
    add_executable(tray_example "${CMAKE_CURRENT_SOURCE_DIR}/src/example.c")
    target_link_libraries(tray_example tray::tray)
    tray_copy_default_icons(tray_example)
endif()

if(TRAY_IS_TOP_LEVEL)
    install(TARGETS tray DESTINATION lib)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/tray.h" DESTINATION include)
    install(FILES ${TRAY_ICON_FILES} DESTINATION share/tray/icons)
endif()

target_compile_options(${PROJECT_NAME} PRIVATE ${TRAY_COMPILE_OPTIONS})
target_link_directories(${PROJECT_NAME} PRIVATE ${TRAY_EXTERNAL_DIRECTORIES})
target_link_libraries(${PROJECT_NAME} PRIVATE ${TRAY_EXTERNAL_LIBRARIES})

#
# Testing and documentation are only available if this is the main project
#
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    if(BUILD_DOCS)
        add_subdirectory(third-party/doxyconfig docs)
    endif()

    if(BUILD_TESTS)
        #
        # Additional setup for coverage
        # https://gcovr.com/en/stable/guide/compiling.html#compiler-options
        #
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            set(CMAKE_CXX_FLAGS "-fprofile-arcs -ftest-coverage -ggdb -O0")
            set(CMAKE_C_FLAGS "-fprofile-arcs -ftest-coverage -ggdb -O0")
        endif()

        enable_testing()
        add_subdirectory(tests)
    endif()
endif()
