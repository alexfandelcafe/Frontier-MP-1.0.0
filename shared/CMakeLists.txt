add_library(frontier_shared STATIC
    src/bitstream.cpp
)

target_include_directories(frontier_shared PUBLIC
    include
)

set_target_properties(frontier_shared PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
