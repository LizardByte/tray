cmake_minimum_required(VERSION 3.13)
# https://github.com/google/oss-policies-info/blob/main/foundational-cxx-support-matrix.md#foundational-c-support

project(test_tray)

# Add GoogleTest directory to the project
set(GTEST_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third-party/googletest")

# For Windows: prevent overriding parent compiler/linker runtime settings.
if(WIN32)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # cmake-lint: disable=C0103
endif()

set(INSTALL_GTEST OFF)
set(INSTALL_GMOCK OFF)
add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")
include_directories("${GTEST_SOURCE_DIR}/googletest/include" "${GTEST_SOURCE_DIR}")

# extra libraries for tests
if (APPLE)
    set(TEST_LIBS "-framework Cocoa")
elseif (WIN32)
    set(TEST_LIBS gdi32 gdiplus)
endif()

file(GLOB_RECURSE TEST_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/conftest.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/utils.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/screenshot_utils.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_*.cpp"
)

add_executable(${PROJECT_NAME}
        ${TEST_SOURCES}
)
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)
target_include_directories(${PROJECT_NAME}
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(${PROJECT_NAME}
        ${TEST_LIBS}
        tray::tray
        gtest
        gtest_main  # if we use this we don't need our own main function
)
target_compile_definitions(${PROJECT_NAME} PUBLIC ${TEST_DEFINITIONS})
target_link_options(${PROJECT_NAME} PRIVATE)

tray_copy_default_icons(${PROJECT_NAME})

add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})
