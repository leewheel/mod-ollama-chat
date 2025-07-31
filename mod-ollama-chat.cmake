# Ensure the module is correctly registered before linking
if(TARGET modules)
    # Include nlohmann/json library
    if(EXISTS "${CMAKE_SOURCE_DIR}/deps/nlohmann")
        target_include_directories(modules PRIVATE ${CMAKE_SOURCE_DIR}/deps/nlohmann)
    else()
        # Try to find nlohmann-json via find_package (vcpkg)
        find_package(nlohmann_json CONFIG)
        if(nlohmann_json_FOUND)
            target_link_libraries(modules PRIVATE nlohmann_json::nlohmann_json)
        else()
            message(WARNING "nlohmann/json not found. Please install it or place in deps/nlohmann/")
        endif()
    endif()
    
    # Include cpp-httplib library (header-only)
    target_include_directories(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
    
    # Platform-specific threading and networking libraries
    if(WIN32)
        # Windows requires winsock for networking
        target_link_libraries(modules PRIVATE ws2_32)
    else()
        # Linux/macOS requires pthread
        target_link_libraries(modules PRIVATE pthread)
    endif()
endif()