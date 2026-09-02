add_executable(msap1-fpga-acquisition
    main.cpp
    acquisition_service.cpp
    support/options.cpp
    support/meter_time_control.cpp
    config/runtime_config.cpp
    pipeline/capture_coordinator.cpp
	pipeline/event_waveform_linker.cpp
    pipeline/aggregation_health_monitor.cpp
    pipeline/health_monitor.cpp
    pipeline/record_ingestor.cpp
    ipc/ipc_channel.cpp
    ipc/command_handlers.cpp
)
target_include_directories(msap1-fpga-acquisition PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(msap1-fpga-acquisition PRIVATE
    msap1::acquisition
    msap1::meter
    msap1::settings
    msap1::system
    msap1::waveform
    mnc::logging
    mnc::service
    mnc::meter-data-provider-stream
    msap1::meter-data-provider-stream
)
target_compile_features(msap1-fpga-acquisition PRIVATE cxx_std_23)
target_compile_options(msap1-fpga-acquisition PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(utc_clock_test
        ${PROJECT_SOURCE_DIR}/tests/utc_clock_test.cpp)
    target_include_directories(utc_clock_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(utc_clock_test PRIVATE cxx_std_23)
    target_compile_options(utc_clock_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME utc_clock_test COMMAND utc_clock_test)
endif()

install(TARGETS msap1-fpga-acquisition
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
