add_executable(msap1-fpga-acquisition
    main.cpp
    acquisition_service.cpp
    support/options.cpp
    config/runtime_config.cpp
    pipeline/capture_coordinator.cpp
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
    msap1::waveform
    mnc::logging
    mnc::service
    mnc::meter-data-streamer
    msap1::meter-stream
)
target_compile_features(msap1-fpga-acquisition PRIVATE cxx_std_23)
target_compile_options(msap1-fpga-acquisition PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS msap1-fpga-acquisition
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
