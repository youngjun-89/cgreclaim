cmake_minimum_required(VERSION 3.10)
project(cgreclaim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Library sources
set(CGRECLAIM_SOURCES
    cgroup.c
    cgreclaim.c
    cgr_config.c
    cgr_log.c
    monitor.c
)

# Static library
add_library(cgreclaim STATIC ${CGRECLAIM_SOURCES})
target_include_directories(cgreclaim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cgreclaim PRIVATE pthread)

# cgrd — cgreclaim daemon binary
add_executable(cgrd cgrd.c)
target_link_libraries(cgrd PRIVATE cgreclaim pthread)
target_include_directories(cgrd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Test suite (standalone build only)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(test_cgreclaim
        test/test_main.c
        test/fake_cgroup.c
    )
    target_include_directories(test_cgreclaim PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/test
    )
    target_link_libraries(test_cgreclaim PRIVATE cgreclaim pthread)

    # `make test` or `ctest` runs the suite
    enable_testing()
    add_test(NAME cgreclaim_tests COMMAND test_cgreclaim)
endif()
