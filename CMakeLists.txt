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
# Core: framing + queues + request/response matching.
# It only depends on the ITransport interface; no socket / poll / winsock.
# ---------------------------------------------------------------------------
add_library(MessageManager STATIC
    src/ipc_framing.cpp
    src/MessageManager.cpp
)

add_library(MessageManager::MessageManager ALIAS MessageManager)

target_include_directories(MessageManager
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(MessageManager PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)

find_package(Threads REQUIRED)
target_link_libraries(MessageManager PUBLIC Threads::Threads)

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
