add_subdirectory(settings)
add_subdirectory(meter-stream)
add_subdirectory(meter-historian)
add_subdirectory(acquisition)
add_subdirectory(modbus-server)
add_subdirectory(mqtt-publisher)
add_subdirectory(service-manager)

# webengine::webengine is added by the repository root under the same option.
if(MSAP1_BUILD_WEB_BACKEND)
    add_subdirectory(web-backend)
endif()
