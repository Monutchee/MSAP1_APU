find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)

add_library(msap1-web-api STATIC
    gateway/acquisition_gateway.cpp
    gateway/database_gateway.cpp
	gateway/data_sender_gateway.cpp
    gateway/settings_gateway.cpp
    gateway/mqtt_gateway.cpp
    waveform_export_task_manager.cpp
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
    api/documentation_routes.cpp
    api/documentation_store.cpp
)
target_include_directories(msap1-web-api PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(msap1-web-api PUBLIC
    msap1::acquisition
    msap1::meter
    msap1::settings
	msap1::datalogger
	msap1::waveform
    msap1::mqtt
    msap1::meter-data-provider-stream
    msap1::system
    mnc::logging
    mnc::service
    webengine::webengine
    glaze::glaze
    OpenSSL::Crypto
    ZLIB::ZLIB
)
target_compile_features(msap1-web-api PUBLIC cxx_std_23)
target_compile_options(msap1-web-api PRIVATE
    -Wall -Wextra -Wpedantic
    -ffunction-sections -fdata-sections)

add_executable(msap1-web-backend
    main.cpp
    auth/msap1_auth_provider.cpp
)
target_link_libraries(msap1-web-backend PRIVATE msap1-web-api)
target_compile_features(msap1-web-backend PRIVATE cxx_std_23)
target_compile_options(msap1-web-backend PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(msap1-web-backend PRIVATE -Wl,--gc-sections)

install(TARGETS msap1-web-backend
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
    add_executable(waveform_export_task_manager_test
        ${PROJECT_SOURCE_DIR}/tests/waveform_export_task_manager_test.cpp)
    target_link_libraries(waveform_export_task_manager_test PRIVATE
        msap1-web-api)
    target_compile_features(waveform_export_task_manager_test PRIVATE
        cxx_std_23)
    target_compile_options(waveform_export_task_manager_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME waveform_export_task_manager_test
        COMMAND waveform_export_task_manager_test)

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

    add_executable(documentation_store_test
        ${PROJECT_SOURCE_DIR}/tests/documentation_store_test.cpp
        api/documentation_store.cpp
    )
    target_include_directories(documentation_store_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(documentation_store_test PRIVATE
        webengine::webengine)
    target_compile_features(documentation_store_test PRIVATE cxx_std_23)
    target_compile_options(documentation_store_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME documentation_store_test COMMAND documentation_store_test)
endif()
