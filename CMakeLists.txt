cmake_minimum_required(VERSION 3.20)

project(SHA256_Project VERSION 1.0)

set(CMAKE_MACOSX_RPATH 1)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-Wno-deprecated-declarations)

find_package(OpenSSL REQUIRED)

include_directories(
    ${PROJECT_SOURCE_DIR}/common
    ${PROJECT_SOURCE_DIR}/server
    ${OPENSSL_INCLUDE_DIR}
)

add_executable(server
    server/server.c
    server/sha256_utils.c
)
target_link_libraries(server
    ${OPENSSL_LIBRARIES}
    pthread
)

add_executable(client
    client/client.c
)
target_link_libraries(client
    ${OPENSSL_LIBRARIES}
)