option(MNC_LOGGING_REQUIRE_SYSTEMD
    "Fail configuration when libsystemd journald support is unavailable"
    OFF
)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)

if(MNC_LOGGING_REQUIRE_SYSTEMD)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SYSTEMD QUIET IMPORTED_TARGET libsystemd)
    endif()
endif()

add_library(mnc_logging STATIC
    ${PROJECT_SOURCE_DIR}/src/logging.cpp
    ${PROJECT_SOURCE_DIR}/src/journal_reader.cpp
)
add_library(mnc::logging ALIAS mnc_logging)
target_include_directories(mnc_logging PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mnc_logging PUBLIC cxx_std_23)
target_compile_options(mnc_logging PRIVATE -Wall -Wextra -Wpedantic)
if(TARGET PkgConfig::SYSTEMD)
    target_link_libraries(mnc_logging PRIVATE PkgConfig::SYSTEMD)
    target_compile_definitions(mnc_logging PRIVATE MNC_LOGGING_HAVE_SYSTEMD=1)
else()
    message(WARNING
        "libsystemd development files not found; building the non-writing "
        "host logging fallback")
endif()

add_library(msap1_apu_core STATIC
    ${PROJECT_SOURCE_DIR}/src/acquisition_ipc.cpp
	${PROJECT_SOURCE_DIR}/src/meter_config.cpp
	${PROJECT_SOURCE_DIR}/src/meter_health.cpp
    ${PROJECT_SOURCE_DIR}/src/protocol.cpp
    ${PROJECT_SOURCE_DIR}/src/rpmsg_endpoint.cpp
    ${PROJECT_SOURCE_DIR}/src/soc_temperature.cpp
    ${PROJECT_SOURCE_DIR}/src/system_identity.cpp
    ${PROJECT_SOURCE_DIR}/src/waveform_capture.cpp
)
add_library(msap1::apu-core ALIAS msap1_apu_core)

target_link_libraries(msap1_apu_core PRIVATE
    mnc::rpmsg_chrdev
    mnc::logging
    glaze::glaze
    OpenSSL::Crypto
)
target_include_directories(msap1_apu_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(msap1_apu_core PUBLIC cxx_std_23)
target_compile_options(msap1_apu_core PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(logging_test ${PROJECT_SOURCE_DIR}/tests/logging_test.cpp)
    target_link_libraries(logging_test PRIVATE mnc::logging)
    target_include_directories(logging_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(logging_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME logging_test COMMAND logging_test)

    add_executable(protocol_test ${PROJECT_SOURCE_DIR}/tests/protocol_test.cpp)
    target_link_libraries(protocol_test PRIVATE msap1::apu-core)
    target_compile_options(protocol_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME protocol_test COMMAND protocol_test)

    add_executable(soc_temperature_test
        ${PROJECT_SOURCE_DIR}/tests/soc_temperature_test.cpp)
    target_link_libraries(soc_temperature_test PRIVATE msap1::apu-core)
    target_compile_options(soc_temperature_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME soc_temperature_test COMMAND soc_temperature_test)

    add_executable(system_identity_test
        ${PROJECT_SOURCE_DIR}/tests/system_identity_test.cpp)
    target_link_libraries(system_identity_test PRIVATE msap1::apu-core)
    target_compile_options(system_identity_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME system_identity_test COMMAND system_identity_test)

    add_executable(waveform_capture_test
        ${PROJECT_SOURCE_DIR}/tests/waveform_capture_test.cpp)
    target_link_libraries(waveform_capture_test PRIVATE msap1::apu-core)
    target_compile_options(waveform_capture_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME waveform_capture_test COMMAND waveform_capture_test)
endif()
