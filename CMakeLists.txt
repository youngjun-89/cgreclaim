cmake_minimum_required(VERSION 3.10)
project(cgreclaim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Library sources
set(CGRECLAIM_SOURCES
    cgroup.c
    cgreclaim.c
    cgr_log.c
    monitor.c
)

# Static library
add_library(cgreclaim STATIC ${CGRECLAIM_SOURCES})
target_include_directories(cgreclaim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cgreclaim PRIVATE pthread)

# Test binary (optional, only when built standalone)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(test_cgreclaim test.c)
    target_link_libraries(test_cgreclaim PRIVATE cgreclaim pthread)
endif()
