cmake_minimum_required(VERSION 3.5)

project(UdpProtobufExample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt5 packages
find_package(Qt5 REQUIRED COMPONENTS Core Network)

# Find Protobuf package
find_package(Protobuf REQUIRED)

# Include directories for Protobuf
include_directories(${PROTOBUF_INCLUDE_DIRS})

# Include directories for Qt5
include_directories(${Qt5Core_INCLUDE_DIRS} ${Qt5Network_INCLUDE_DIRS})

# Protobuf library
find_package(Protobuf REQUIRED)
include_directories(${PROTOBUF_INCLUDE_DIRS})

# Sender
add_executable(sender sender.cpp message.pb.cc)
target_link_libraries(sender Qt5::Core Qt5::Network ${PROTOBUF_LIBRARIES})

# Receiver
add_executable(receiver receiver.cpp message.pb.cc)
target_link_libraries(receiver Qt5::Core Qt5::Network ${PROTOBUF_LIBRARIES})
