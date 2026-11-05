add_library(MessageManagerUnix STATIC
    src/UnixSocksTransport.cpp
    src/UnixSocket.cpp
)

add_library(MessageManager::Unix ALIAS MessageManagerUnix)

target_include_directories(MessageManagerUnix
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(MessageManagerUnix
    PUBLIC MessageManager Threads::Threads
)

if(WIN32)
    # AF_UNIX + WSAPoll + WSADuplicateSocket
    target_link_libraries(MessageManagerUnix PUBLIC ws2_32)
endif()

target_compile_options(MessageManagerUnix PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)

set_target_properties(MessageManagerUnix PROPERTIES
    OUTPUT_NAME MessageManagerUnix
)

install(TARGETS MessageManagerUnix
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)
