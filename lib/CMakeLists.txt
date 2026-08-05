option(MNC_LOGGING_REQUIRE_SYSTEMD
    "Fail configuration when libsystemd journald support is unavailable"
    OFF
)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(mnc_ipc STATIC
    ${PROJECT_SOURCE_DIR}/src/ipc.cpp
)
add_library(mnc::ipc ALIAS mnc_ipc)
target_include_directories(mnc_ipc PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mnc_ipc PUBLIC cxx_std_23)
target_compile_definitions(mnc_ipc PUBLIC BOOST_ERROR_CODE_HEADER_ONLY=1)
target_compile_options(mnc_ipc PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(mnc_ipc PUBLIC Threads::Threads)

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

add_library(mnc_service STATIC
    ${PROJECT_SOURCE_DIR}/src/service.cpp
    ${PROJECT_SOURCE_DIR}/src/service_manager.cpp
)
add_library(mnc::service ALIAS mnc_service)
target_include_directories(mnc_service PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mnc_service PUBLIC cxx_std_23)
target_compile_options(mnc_service PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(mnc_service PUBLIC mnc::logging Threads::Threads)
if(TARGET PkgConfig::SYSTEMD)
    target_link_libraries(mnc_service PRIVATE PkgConfig::SYSTEMD)
    target_compile_definitions(mnc_service PRIVATE MNC_SERVICE_HAVE_SYSTEMD=1)
endif()

function(msap1_library target alias)
    add_library(${target} STATIC ${ARGN})
    add_library(${alias} ALIAS ${target})
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(${target} PUBLIC cxx_std_23)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()

# Product data types and durable meter storage deliberately have no dependency
# on IPC, WebEngine, or command-line presentation code.
msap1_library(msap1_meter msap1::meter
    ${PROJECT_SOURCE_DIR}/src/meter_data.cpp
    ${PROJECT_SOURCE_DIR}/src/meter_record_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/meter_config.cpp
    ${PROJECT_SOURCE_DIR}/src/meter_health.cpp
)
target_link_libraries(msap1_meter PRIVATE
    glaze::glaze
    ${CMAKE_DL_LIBS}
)

add_library(mnc_settings STATIC
    ${PROJECT_SOURCE_DIR}/src/settings_repository.cpp
)
add_library(mnc::settings ALIAS mnc_settings)
target_include_directories(mnc_settings PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mnc_settings PUBLIC cxx_std_23)
target_compile_options(mnc_settings PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(mnc_settings PRIVATE glaze::glaze)

msap1_library(msap1_settings msap1::settings
    ${PROJECT_SOURCE_DIR}/src/settings.cpp
    ${PROJECT_SOURCE_DIR}/src/settings_ipc.cpp
)
target_link_libraries(msap1_settings PUBLIC
    mnc::settings
    mnc::ipc
    msap1::meter
)
target_link_libraries(msap1_settings PRIVATE
    glaze::glaze
    OpenSSL::Crypto
)

# Waveform device/session ownership is isolated from the meter record path so
# future analysis code does not acquire access to the DMA descriptor.
msap1_library(msap1_waveform msap1::waveform
    ${PROJECT_SOURCE_DIR}/src/waveform_capture.cpp
)

# Hardware-neutral product protocol and the RPU endpoint adapter.  The public
# dependency is intentional: acquisition_ipc.hpp exposes typed meter/waveform
# DTOs while mnc::ipc remains unaware of their meaning.
msap1_library(msap1_acquisition msap1::acquisition
    ${PROJECT_SOURCE_DIR}/src/acquisition_ipc.cpp
    ${PROJECT_SOURCE_DIR}/src/acquisition/meter_dma_reader.cpp
    ${PROJECT_SOURCE_DIR}/src/acquisition/rpu_controller.cpp
    ${PROJECT_SOURCE_DIR}/src/protocol.cpp
    ${PROJECT_SOURCE_DIR}/src/rpmsg_endpoint.cpp
)
target_link_libraries(msap1_acquisition PUBLIC
    mnc::ipc
    msap1::meter
    msap1::waveform
)
target_link_libraries(msap1_acquisition PRIVATE
    mnc::rpmsg_chrdev
    mnc::logging
)

msap1_library(msap1_system msap1::system
    ${PROJECT_SOURCE_DIR}/src/soc_temperature.cpp
    ${PROJECT_SOURCE_DIR}/src/system_identity.cpp
)
target_link_libraries(msap1_system PRIVATE OpenSSL::Crypto)

msap1_library(msap1_service_protocol msap1::service-protocol
    ${PROJECT_SOURCE_DIR}/src/service_control.cpp
)
target_link_libraries(msap1_service_protocol PUBLIC mnc::ipc)

# Compatibility umbrella. Existing applications can migrate one dependency at
# a time while new code links only the focused architectural boundary it uses.
add_library(msap1_apu_core INTERFACE)
add_library(msap1::apu-core ALIAS msap1_apu_core)
target_link_libraries(msap1_apu_core INTERFACE
    msap1::meter
    msap1::waveform
    msap1::acquisition
    msap1::system
    msap1::service-protocol
    msap1::settings
)

# Reject accidental dependency inversion at configure time. These checks make
# the library boundaries executable rather than relying only on diagrams.
function(msap1_forbid_direct_dependencies target)
    get_target_property(direct_dependencies ${target} LINK_LIBRARIES)
    if(NOT direct_dependencies)
        return()
    endif()
    foreach(forbidden IN LISTS ARGN)
        if(forbidden IN_LIST direct_dependencies)
            message(FATAL_ERROR
                "${target} must not link application dependency ${forbidden}")
        endif()
    endforeach()
endfunction()

msap1_forbid_direct_dependencies(msap1_meter
    mnc::ipc
    webengine::webengine
)
msap1_forbid_direct_dependencies(msap1_waveform
    mnc::ipc
    webengine::webengine
)
msap1_forbid_direct_dependencies(mnc_ipc
    msap1::meter
    msap1::waveform
    webengine::webengine
)

if(BUILD_TESTING)
    add_executable(ipc_test ${PROJECT_SOURCE_DIR}/tests/ipc_test.cpp)
    target_link_libraries(ipc_test PRIVATE mnc::ipc)
    target_compile_options(ipc_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ipc_test COMMAND ipc_test)

    add_executable(meter_data_test
        ${PROJECT_SOURCE_DIR}/tests/meter_data_test.cpp)
    target_link_libraries(meter_data_test PRIVATE msap1::meter)
    target_compile_options(meter_data_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME meter_data_test COMMAND meter_data_test)

    add_executable(meter_record_stream_test
        ${PROJECT_SOURCE_DIR}/tests/meter_record_stream_test.cpp)
    target_link_libraries(meter_record_stream_test PRIVATE msap1::meter)
    target_compile_options(meter_record_stream_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME meter_record_stream_test COMMAND meter_record_stream_test)

    add_executable(service_manager_test
        ${PROJECT_SOURCE_DIR}/tests/service_manager_test.cpp)
    target_link_libraries(service_manager_test PRIVATE mnc::service)
    target_compile_options(service_manager_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME service_manager_test COMMAND service_manager_test)

    add_executable(logging_test ${PROJECT_SOURCE_DIR}/tests/logging_test.cpp)
    target_link_libraries(logging_test PRIVATE mnc::logging)
    target_include_directories(logging_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(logging_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME logging_test COMMAND logging_test)

    add_executable(protocol_test ${PROJECT_SOURCE_DIR}/tests/protocol_test.cpp)
    target_link_libraries(protocol_test PRIVATE msap1::acquisition)
    target_compile_options(protocol_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME protocol_test COMMAND protocol_test)

    add_executable(soc_temperature_test
        ${PROJECT_SOURCE_DIR}/tests/soc_temperature_test.cpp)
    target_link_libraries(soc_temperature_test PRIVATE msap1::system)
    target_compile_options(soc_temperature_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME soc_temperature_test COMMAND soc_temperature_test)

    add_executable(system_identity_test
        ${PROJECT_SOURCE_DIR}/tests/system_identity_test.cpp)
    target_link_libraries(system_identity_test PRIVATE msap1::system)
    target_compile_options(system_identity_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME system_identity_test COMMAND system_identity_test)

    add_executable(waveform_capture_test
        ${PROJECT_SOURCE_DIR}/tests/waveform_capture_test.cpp)
    target_link_libraries(waveform_capture_test PRIVATE msap1::waveform)
    target_compile_options(waveform_capture_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME waveform_capture_test COMMAND waveform_capture_test)

    add_executable(acquisition_architecture_test
        ${PROJECT_SOURCE_DIR}/tests/acquisition_architecture_test.cpp)
    target_link_libraries(acquisition_architecture_test PRIVATE
        msap1::acquisition)
    target_compile_options(acquisition_architecture_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME acquisition_architecture_test
        COMMAND acquisition_architecture_test)

    add_executable(settings_test
        ${PROJECT_SOURCE_DIR}/tests/settings_test.cpp)
    target_link_libraries(settings_test PRIVATE msap1::settings glaze::glaze)
    target_compile_definitions(settings_test PRIVATE
        PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_compile_options(settings_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME settings_test COMMAND settings_test)
endif()
