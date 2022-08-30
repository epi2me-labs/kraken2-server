# Sources for file reading and messaging
add_library(server_client_utils
    src/utils.cc
    src/messages.cc)

# Specify the headers (include) for this lib (target) and declare them PUBLIC so are findable by other libs/executables
target_include_directories(server_client_utils PUBLIC ./include)

# Add dependencies / links for this lib
target_link_libraries(server_client_utils
    classify
    kraken2_proto)