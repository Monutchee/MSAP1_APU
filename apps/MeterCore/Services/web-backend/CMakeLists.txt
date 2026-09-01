add_executable(msap1-web-backend
    main.cpp
    gateway/acquisition_gateway.cpp
    gateway/database_gateway.cpp
	gateway/data_sender_gateway.cpp
    gateway/settings_gateway.cpp
    gateway/mqtt_gateway.cpp
    auth/msap1_auth_provider.cpp
    api/health_routes.cpp
	api/attribute_routes.cpp
    api/meter_routes.cpp
	api/power_quality_routes.cpp
	api/energy_routes.cpp
    api/adc_routes.cpp
    api/waveform_routes.cpp
    api/settings_routes.cpp
    api/developer_routes.cpp
    api/database_routes.cpp
    api/history_routes.cpp
    api/mqtt_routes.cpp
	api/data_logging_routes.cpp
)
target_include_directories(msap1-web-backend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(msap1-web-backend PRIVATE
    msap1::acquisition
    msap1::meter
    msap1::settings
	msap1::datalogger
    msap1::mqtt
    msap1::meter-data-provider-stream
    msap1::system
    mnc::logging
    mnc::service
    webengine::webengine
    glaze::glaze
)
target_compile_features(msap1-web-backend PRIVATE cxx_std_23)
target_compile_options(msap1-web-backend PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS msap1-web-backend
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
    add_executable(systemd_notifier_test
        ${PROJECT_SOURCE_DIR}/tests/systemd_notifier_test.cpp
        support/systemd_notifier.cpp
    )
    target_include_directories(systemd_notifier_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(systemd_notifier_test PRIVATE cxx_std_23)
    target_compile_options(systemd_notifier_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME systemd_notifier_test COMMAND systemd_notifier_test)
endif()
