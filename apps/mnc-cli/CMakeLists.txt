find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(mnc_cli STATIC
    core/cli.cpp
    commands/meter_commands.cpp
    commands/rpu_commands.cpp
    commands/adc_commands.cpp
    commands/waveform_commands.cpp
    commands/log_commands.cpp
    commands/machine_commands.cpp
    commands/system_commands.cpp
    commands/service_commands.cpp
    commands/settings_commands.cpp
)
target_link_libraries(mnc_cli PUBLIC
    msap1::acquisition
    msap1::meter
	msap1::meter-data-provider-stream
    msap1::service-protocol
    msap1::system
    msap1::settings
	msap1::waveform
    mnc::logging
	OpenSSL::Crypto
    glaze::glaze
)
target_include_directories(mnc_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mnc_cli PUBLIC cxx_std_23)
target_compile_options(mnc_cli PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mnc main.cpp)
target_link_libraries(mnc PRIVATE mnc_cli)
target_compile_features(mnc PRIVATE cxx_std_23)
target_compile_options(mnc PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mnc-ssh-gateway gateway_main.cpp)
target_link_libraries(mnc-ssh-gateway PRIVATE mnc_cli)
target_compile_features(mnc-ssh-gateway PRIVATE cxx_std_23)
target_compile_options(mnc-ssh-gateway PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS mnc mnc-ssh-gateway
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES completions/mnc
    DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions)

if(BUILD_TESTING)
    add_executable(mnc_cli_test ${PROJECT_SOURCE_DIR}/tests/cli_test.cpp)
    target_link_libraries(mnc_cli_test PRIVATE mnc_cli)
    target_compile_features(mnc_cli_test PRIVATE cxx_std_23)
    target_compile_options(mnc_cli_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mnc_cli_test COMMAND mnc_cli_test)

    find_program(BASH_EXECUTABLE bash)
    if(BASH_EXECUTABLE)
        add_test(NAME mnc_bash_completion_syntax
            COMMAND ${BASH_EXECUTABLE} -n
                ${CMAKE_CURRENT_SOURCE_DIR}/completions/mnc)
    endif()
endif()
