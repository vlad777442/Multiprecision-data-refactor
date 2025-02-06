cmake_minimum_required(VERSION 3.10)
project(AdaptiveTransfer)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Boost REQUIRED COMPONENTS system)
find_package(Protobuf REQUIRED)

# Add protobuf generated files
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ../fragment.proto)

# Add sender executable
add_executable(sender_sc2 
    sender_sc2.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

# Add receiver executable
add_executable(receiver_sc2 
    receiver_sc2.cpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)

# Include directories for both executables
foreach(target sender_sc2 receiver_sc2)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${Protobuf_INCLUDE_DIRS}
        ${Boost_INCLUDE_DIRS}
    )

    # Link libraries
    target_link_libraries(${target} PRIVATE
        ${Protobuf_LIBRARIES}
        ${Boost_LIBRARIES}
        pthread
    )
endforeach()