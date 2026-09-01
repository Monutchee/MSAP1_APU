# Development utilities derived from the same product contracts as runtime.
add_subdirectory(modbus-map-dump)

if(MSAP1_BUILD_WEB_BACKEND)
    add_subdirectory(openapi-dump)
endif()
