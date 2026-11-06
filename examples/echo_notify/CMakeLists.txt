add_executable(mm_sample_server server.cpp)
add_executable(mm_sample_client client.cpp)

foreach(_target mm_sample_server mm_sample_client)
    target_link_libraries(${_target} PRIVATE
        MessageManager
        MessageManagerUnix
    )

    target_compile_options(${_target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
    )
endforeach()
