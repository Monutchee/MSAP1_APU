add_executable(msap1-modbus-server
    main.cpp
    modbus_service.cpp
)
target_include_directories(msap1-modbus-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(msap1-modbus-server PRIVATE
    msap1::modbus
    msap1::acquisition
    msap1::settings
    mnc::logging
    mnc::service
)
target_compile_features(msap1-modbus-server PRIVATE cxx_std_23)
target_compile_options(msap1-modbus-server PRIVATE
    -Wall -Wextra -Wpedantic)

install(TARGETS msap1-modbus-server
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
