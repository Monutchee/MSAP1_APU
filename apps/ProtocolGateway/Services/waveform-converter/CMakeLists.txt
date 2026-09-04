find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_executable(msap1-waveform-converter
    main.cpp
    waveform_conversion_service.cpp
)
target_include_directories(msap1-waveform-converter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(msap1-waveform-converter PRIVATE
    msap1::waveform
    msap1::waveform-ipc
    mnc::service
    mnc::logging
    glaze::glaze
    OpenSSL::Crypto
)
target_compile_features(msap1-waveform-converter PRIVATE cxx_std_23)
target_compile_options(msap1-waveform-converter PRIVATE
    -Wall -Wextra -Wpedantic)
install(TARGETS msap1-waveform-converter
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
