set(EVA_INCLUDES "../external/compression_utils/include")

#Added for protobuf
INCLUDE(FindProtobuf)
FIND_PACKAGE(Protobuf REQUIRED)
INCLUDE_DIRECTORIES(${PROTOBUF_INCLUDE_DIR})
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS fragment.proto)
#add_library(proto ${PROTO_HDRS} ${PROTO_SRCS})
include_directories(${CMAKE_CURRENT_BINARY_DIR})
###
add_executable (mgard_ec_refactor mgard_ec_refactor.cpp ${PROTO_SRCS} ${PROTO_HDRS})
target_include_directories(mgard_ec_refactor PRIVATE ${MGARDx_INCLUDES} ${SZ3_INCLUDES} ${ZSTD_INCLUDES} ${ADIOS2_INCLUDES} {EC_INCLUDES} ${ROCKSDB_INCLUDES})
target_link_libraries(mgard_ec_refactor ${PROJECT_NAME} ${SZ3_LIB} ${ZSTD_LIB} ${ADIOS2_LIB} ${EC_LIB} ${ROCKSDB_LIB} ${Protobuf_LIBRARIES})


add_executable (mgard_ec_reconstruct mgard_ec_reconstruct.cpp)
target_include_directories(mgard_ec_reconstruct PRIVATE ${MGARDx_INCLUDES} ${SZ3_INCLUDES} ${ZSTD_INCLUDES} ${ADIOS2_INCLUDES} ${EC_INCLUDES} ${ROCKSDB_INCLUDES})
target_link_libraries(mgard_ec_reconstruct ${PROJECT_NAME} ${SZ3_LIB} ${ZSTD_LIB} ${ADIOS2_LIB} ${EC_LIB} ${ROCKSDB_LIB} ${Protobuf_LIBRARIES})
