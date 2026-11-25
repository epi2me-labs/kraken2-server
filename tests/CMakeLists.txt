# Tests that talk to a running server directly over gRPC
add_executable(grpc_error_test integration/grpc_error_test.cc)
target_link_libraries(grpc_error_test
    kraken2_proto
    ${_PROTOBUF_LIBPROTOBUF}
    ${_GRPC_GRPCPP}
    Threads::Threads)

# Unit tests of the core. These need nothing running.
# The queue and the pool need nothing from kraken2
foreach(name blocking_queue_test worker_pool_test)
    add_executable(${name} unit/${name}.cc)
    target_link_libraries(${name} k2s_common)
    add_test(NAME ${name} COMMAND ${name})
endforeach()

add_executable(tally_test unit/tally_test.cc)
target_link_libraries(tally_test k2s_core)
add_test(NAME tally_test COMMAND tally_test)

# Needs a database and reads, so it is run by tests/integration/run_all_checks.sh
add_executable(classifier_test unit/classifier_test.cc)
target_link_libraries(classifier_test k2s_core)
