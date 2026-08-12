add_executable(msap1-meter-stream
    main.cpp
    meter_stream_service.cpp
)
target_include_directories(msap1-meter-stream PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(msap1-meter-stream PRIVATE
    msap1::meter msap1::meter-stream msap1::settings mnc::meter-data-streamer
    mnc::ipc mnc::logging mnc::service)
target_compile_features(msap1-meter-stream PRIVATE cxx_std_23)
target_compile_options(msap1-meter-stream PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS msap1-meter-stream RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
