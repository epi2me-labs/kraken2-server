# Generic threading helpers used by both the server and the client. Nothing in
# here knows about kraken2 or gRPC.
add_library(k2s_common INTERFACE)
# Code under src/ includes its headers as "core/index.h", "common/blocking_queue.h", ...
target_include_directories(k2s_common INTERFACE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(k2s_common INTERFACE Threads::Threads)
