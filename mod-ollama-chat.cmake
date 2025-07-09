if(TARGET modules)
    # 添加头文件路径
    target_include_directories(modules PRIVATE
        ${CMAKE_SOURCE_DIR}/deps/curl
        ${CMAKE_SOURCE_DIR}/deps/nlohmann
    )

    # 直接链接本地 curl.lib 文件
    target_link_libraries(modules PRIVATE
        ${CMAKE_SOURCE_DIR}/deps/curl/curl/lib/curl.lib
    )
endif()