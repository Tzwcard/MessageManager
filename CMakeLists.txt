cmake_minimum_required(VERSION 3.16)

project(MessageManager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# Bundled protobuf (pinned submodule).
# ---------------------------------------------------------------------------
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_MSVC_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
set(protobuf_FORCE_FETCH_DEPENDENCIES ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/protobuf EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Generate ipc_frame.pb.cc/h from proto/ipc_frame.proto.
# ---------------------------------------------------------------------------
set(MM_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(MM_PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(MM_PROTO_SRC ${MM_PROTO_GEN_DIR}/ipc_frame.pb.cc)
set(MM_PROTO_HDR ${MM_PROTO_GEN_DIR}/ipc_frame.pb.h)

file(MAKE_DIRECTORY ${MM_PROTO_GEN_DIR})

add_custom_command(
    OUTPUT ${MM_PROTO_SRC} ${MM_PROTO_HDR}
    COMMAND $<TARGET_FILE:protobuf::protoc>
            --cpp_out=${MM_PROTO_GEN_DIR}
            -I${MM_PROTO_DIR}
            ${MM_PROTO_DIR}/ipc_frame.proto
    DEPENDS protobuf::protoc ${MM_PROTO_DIR}/ipc_frame.proto
    COMMENT "Generating ipc_frame.pb.cc/h"
    VERBATIM
)

add_custom_target(mm_proto_gen DEPENDS ${MM_PROTO_SRC} ${MM_PROTO_HDR})

# ---------------------------------------------------------------------------
# Core: framing + queues + request/response matching.
# It depends on protobuf for the wire body; transport remains abstract.
# ---------------------------------------------------------------------------
add_library(MessageManager STATIC
    src/ipc_framing.cpp
    src/MessageManager.cpp
    ${MM_PROTO_SRC}
)

add_library(MessageManager::MessageManager ALIAS MessageManager)
add_dependencies(MessageManager mm_proto_gen)

target_include_directories(MessageManager
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${MM_PROTO_GEN_DIR}
)

target_compile_options(MessageManager PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)

find_package(Threads REQUIRED)
target_link_libraries(MessageManager PUBLIC Threads::Threads protobuf::libprotobuf)

set_target_properties(MessageManager PROPERTIES
    OUTPUT_NAME MessageManager
)

include(GNUInstallDirs)

install(TARGETS MessageManager
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

install(FILES proto/ipc_frame.proto
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/msgmgr
)

# ---------------------------------------------------------------------------
# Default transport: AF_UNIX.
# It is a separate target; core does not depend on it. Consumers can ignore
# it and implement ITransport themselves.
# ---------------------------------------------------------------------------
add_subdirectory(transport/unix)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
add_executable(message_manager_test
    test/message_manager_test.cpp
)

target_include_directories(message_manager_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/transport/unix/include
)

target_link_libraries(message_manager_test PRIVATE
    MessageManager MessageManagerUnix
)

target_compile_options(message_manager_test PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)

enable_testing()
add_test(NAME message_manager_self_test COMMAND message_manager_test)
