# Ensure the module is correctly registered before linking
if(TARGET modules)
    # Include cpp-httplib (header-only library, no linking required)
    target_include_directories(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
    
    # Include nlohmann-json path
    target_include_directories(modules PRIVATE /usr/local/include /usr/local/include/nlohmann)
    
    # Platform-specific threading and networking libraries
    if(WIN32)
        # Windows requires winsock for networking
        target_link_libraries(modules PRIVATE ws2_32)
    else()
        # Linux/macOS requires pthread
        target_link_libraries(modules PRIVATE pthread)
    endif()
endif()