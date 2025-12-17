cmake_minimum_required(VERSION 3.10)
project(lil-raft C)

set(CMAKE_C_STANDARD 11)

# Library
add_library(raft STATIC
        src/raft.c
        src/raft_log.c
        src/raft_election.c
        src/raft_replication.c
)
target_include_directories(raft PUBLIC src)

# Tests
enable_testing()

set(TESTS log core election replication clusters torture)

foreach(test ${TESTS})
    add_executable(test_${test} tests/test_${test}.c)
    target_link_libraries(test_${test} raft)
    add_test(NAME test_${test} COMMAND test_${test})
endforeach()