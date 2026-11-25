# The server's view of kraken2: a loaded database, and classification of reads
# against it, with no gRPC or protobuf in sight.
add_library(k2s_core STATIC
    index.cc
    upstream_port.cc
    classifier.cc
    tally.cc)

target_link_libraries(k2s_core PUBLIC k2s_common kraken2_upstream)

# Built as release code, without assertions. This is the classification hot
# loop, and building it with them on measured about 5% slower on a large input
# (the code was built this way when it lived in the server). kraken2's own
# sources, in kraken2_upstream, keep their assertions as upstream builds them.
target_compile_definitions(k2s_core PUBLIC NDEBUG)
