add_library(mnc_cli STATIC
    cli.cpp
    meter_commands.cpp
    adc_commands.cpp
    log_commands.cpp
    system_commands.cpp
)
target_link_libraries(mnc_cli PUBLIC msap1::apu-core mnc::logging)
target_include_directories(mnc_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mnc_cli PUBLIC cxx_std_23)
target_compile_options(mnc_cli PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mnc main.cpp)
target_link_libraries(mnc PRIVATE mnc_cli)
target_compile_features(mnc PRIVATE cxx_std_23)
target_compile_options(mnc PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS mnc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
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
