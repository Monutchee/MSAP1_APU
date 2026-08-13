option(MNC_LOGGING_REQUIRE_SYSTEMD
    "Fail configuration when libsystemd journald support is unavailable"
    OFF
)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_library(SQLITE3_LIBRARY NAMES sqlite3)
if(NOT SQLITE3_LIBRARY)
    # Some development hosts install only the versioned runtime SONAME.
    # Yocto provides the regular development symlink through DEPENDS=sqlite3.
    find_file(SQLITE3_LIBRARY NAMES libsqlite3.so.0
        PATHS /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu)
endif()
if(NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR "SQLite runtime/development library was not found")
endif()

add_library(mnc_sqlite STATIC
    mnc/storage/sqlite/sqlite_database.cpp
)
add_library(mnc::sqlite ALIAS mnc_sqlite)
target_include_directories(mnc_sqlite PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/common>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mnc_sqlite PUBLIC cxx_std_23)
target_compile_options(mnc_sqlite PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(mnc_sqlite PUBLIC ${SQLITE3_LIBRARY})

add_library(mnc_meter_data_provider_stream STATIC
    mnc/MeterDataProvider/stream/durable_meter_spool.cpp
)
add_library(mnc::meter-data-provider-stream ALIAS
    mnc_meter_data_provider_stream)
target_include_directories(mnc_meter_data_provider_stream PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/common>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mnc_meter_data_provider_stream PUBLIC cxx_std_23)
target_compile_options(mnc_meter_data_provider_stream PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(mnc_meter_data_provider_stream PUBLIC mnc::sqlite)

add_library(mnc_ipc STATIC
    mnc/ipc/ipc.cpp
)
add_library(mnc::ipc ALIAS mnc_ipc)
target_include_directories(mnc_ipc PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/common>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mnc_ipc PUBLIC cxx_std_23)
target_compile_definitions(mnc_ipc PUBLIC BOOST_ERROR_CODE_HEADER_ONLY=1)
target_compile_options(mnc_ipc PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(mnc_ipc PUBLIC Threads::Threads)

# Transport-independent Modbus protocol handling plus Boost.Asio TCP/RTU
# adapters. Product register addresses deliberately live outside this target.
add_library(mnc_modbus STATIC
    mnc/modbus/encoding.cpp
    mnc/modbus/request_handler.cpp
    mnc/modbus/rtu_server.cpp
    mnc/modbus/server.cpp
    mnc/modbus/tcp_server.cpp
)
add_library(mnc::modbus ALIAS mnc_modbus)
target_include_directories(mnc_modbus PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/common>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mnc_modbus PUBLIC cxx_std_23)
target_compile_definitions(mnc_modbus PUBLIC BOOST_ERROR_CODE_HEADER_ONLY=1)
target_compile_options(mnc_modbus PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(mnc_modbus PUBLIC Threads::Threads)

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
    mnc/logging/logging.cpp
    mnc/logging/journal_reader.cpp
)
add_library(mnc::logging ALIAS mnc_logging)
target_include_directories(mnc_logging PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/common>
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
    mnc/service/service.cpp
    mnc/service/service_manager.cpp
)
add_library(mnc::service ALIAS mnc_service)
target_include_directories(mnc_service PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/common>
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
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/common>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(${target} PUBLIC cxx_std_23)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()

# Product-neutral latest-state meter access.  This library deliberately makes
# no durability promise: slow subscribers receive the newest snapshot only.
add_library(mnc_meter_data_provider STATIC
    mnc/MeterDataProvider/attributes/meter_attribute_catalog.cpp
)
add_library(mnc::meter-data-provider ALIAS mnc_meter_data_provider)
target_include_directories(mnc_meter_data_provider PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/common>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mnc_meter_data_provider PUBLIC cxx_std_23)
target_compile_options(mnc_meter_data_provider PRIVATE
    -Wall -Wextra -Wpedantic)

# Product data types and latest-state publication deliberately have no
# dependency on IPC, WebEngine, persistence, or command-line presentation.
msap1_library(msap1_meter msap1::meter
    msap1/meter/meter_data.cpp
    msap1/meter/MeterDataProvider/snapshot/in_process_meter_snapshot_provider.cpp
    msap1/meter/meter_config.cpp
    msap1/meter/meter_health.cpp
    msap1/meter/measurement_timebase.cpp
)
target_link_libraries(msap1_meter PUBLIC
    mnc::meter-data-provider
)
target_link_libraries(msap1_meter PRIVATE
    glaze::glaze
)

msap1_library(msap1_meter_data_provider_stream
    msap1::meter-data-provider-stream
    msap1/meter/MeterDataProvider/stream/meter_stream_ipc.cpp
    msap1/meter/history/meter_history.cpp
    msap1/meter/history/historian_ipc.cpp
)
target_link_libraries(msap1_meter_data_provider_stream PUBLIC
    msap1::meter
    mnc::meter-data-provider-stream
    mnc::ipc
)

add_library(mnc_settings STATIC
    mnc/settings/settings_repository.cpp
)
add_library(mnc::settings ALIAS mnc_settings)
target_include_directories(mnc_settings PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/common>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mnc_settings PUBLIC cxx_std_23)
target_compile_options(mnc_settings PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(mnc_settings PRIVATE glaze::glaze)

msap1_library(msap1_settings msap1::settings
    msap1/settings/settings.cpp
    msap1/settings/settings_ipc.cpp
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
    msap1/waveform/waveform_capture.cpp
)

# Hardware-neutral product protocol and the RPU endpoint adapter.  The public
# dependencies are intentional: acquisition_ipc.hpp binds the typed command
# structs to glaze BEVE payloads while mnc::ipc remains unaware of their
# meaning.
msap1_library(msap1_acquisition msap1::acquisition
	msap1/acquisition/ipc/acquisition_ipc.cpp
	msap1/meter/MeterDataProvider/snapshot/acquisition_meter_snapshot_provider.cpp
    msap1/acquisition/dma/meter_dma_reader.cpp
    msap1/acquisition/rpu/rpu_controller.cpp
    msap1/acquisition/rpu/protocol.cpp
    msap1/acquisition/rpu/rpmsg_endpoint.cpp
)
target_link_libraries(msap1_acquisition PUBLIC
    mnc::ipc
    msap1::meter
    msap1::waveform
    glaze::glaze
)
target_link_libraries(msap1_acquisition PRIVATE
    mnc::rpmsg_chrdev
    mnc::logging
)

# Static MSAP1 register contract and its snapshot-backed RegisterBank adapter.
# TCP/RTU framing remains wholly in the product-neutral mnc::modbus target.
msap1_library(msap1_modbus msap1::modbus
	msap1/modbus/modbus_register_map.cpp
)
target_link_libraries(msap1_modbus PUBLIC
	mnc::modbus
	mnc::meter-data-provider
)

msap1_library(msap1_system msap1::system
    msap1/system/soc_temperature.cpp
    msap1/system/system_identity.cpp
)
target_link_libraries(msap1_system PRIVATE OpenSSL::Crypto)

msap1_library(msap1_service_protocol msap1::service-protocol
    msap1/service/service_control.cpp
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
	add_executable(modbus_test ${PROJECT_SOURCE_DIR}/tests/modbus_test.cpp)
	target_link_libraries(modbus_test PRIVATE mnc::modbus)
	target_compile_options(modbus_test PRIVATE -Wall -Wextra -Wpedantic)
	add_test(NAME modbus_test COMMAND modbus_test)

	add_executable(msap1_modbus_test
		${PROJECT_SOURCE_DIR}/tests/msap1_modbus_test.cpp)
	target_link_libraries(msap1_modbus_test PRIVATE msap1::modbus)
	target_compile_options(msap1_modbus_test PRIVATE
		-Wall -Wextra -Wpedantic)
	add_test(NAME msap1_modbus_test COMMAND msap1_modbus_test)

    add_executable(ipc_test ${PROJECT_SOURCE_DIR}/tests/ipc_test.cpp)
    target_link_libraries(ipc_test PRIVATE mnc::ipc)
    target_compile_options(ipc_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ipc_test COMMAND ipc_test)

    add_executable(meter_data_test
        ${PROJECT_SOURCE_DIR}/tests/meter_data_test.cpp)
    target_link_libraries(meter_data_test PRIVATE msap1::meter)
    target_compile_options(meter_data_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME meter_data_test COMMAND meter_data_test)

    # The reference aggregator under tests/support is deliberately test-only:
    # the PL is the authoritative aggregator and production code never
    # computes aggregates, so only this test target sees tests/ includes.
    add_executable(aggregate_decode_test
        ${PROJECT_SOURCE_DIR}/tests/aggregate_decode_test.cpp)
    target_link_libraries(aggregate_decode_test PRIVATE msap1::meter)
    target_include_directories(aggregate_decode_test PRIVATE
        ${PROJECT_SOURCE_DIR}/tests)
    target_compile_options(aggregate_decode_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME aggregate_decode_test COMMAND aggregate_decode_test)

    # GET /api/v1/meter/aggregate is pinned by its JSON payload, and the
    # projection that renders it is deliberately WebEngine-free, so the
    # contract is tested without linking the HTTP stack.
    add_executable(meter_aggregate_route_test
        ${PROJECT_SOURCE_DIR}/tests/meter_aggregate_route_test.cpp)
    target_link_libraries(meter_aggregate_route_test PRIVATE
        msap1::acquisition
        glaze::glaze
    )
    target_include_directories(meter_aggregate_route_test PRIVATE
        ${PROJECT_SOURCE_DIR}/apps/MeterCore/Services/web-backend/api)
    target_compile_options(meter_aggregate_route_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME meter_aggregate_route_test
        COMMAND meter_aggregate_route_test)

    add_executable(meter_data_provider_test
        ${PROJECT_SOURCE_DIR}/tests/meter_data_provider_test.cpp)
    target_link_libraries(meter_data_provider_test PRIVATE msap1::meter)
    target_compile_options(meter_data_provider_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME meter_data_provider_test COMMAND meter_data_provider_test)

    add_executable(meter_stream_test
        ${PROJECT_SOURCE_DIR}/tests/meter_stream_test.cpp)
    target_link_libraries(meter_stream_test PRIVATE
        msap1::meter-data-provider-stream
        mnc::meter-data-provider-stream)
    target_compile_options(meter_stream_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME meter_stream_test COMMAND meter_stream_test)

    add_executable(service_manager_test
        ${PROJECT_SOURCE_DIR}/tests/service_manager_test.cpp)
    target_link_libraries(service_manager_test PRIVATE mnc::service)
    target_compile_options(service_manager_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME service_manager_test COMMAND service_manager_test)

    add_executable(logging_test ${PROJECT_SOURCE_DIR}/tests/logging_test.cpp)
    target_link_libraries(logging_test PRIVATE mnc::logging)
    target_include_directories(logging_test PRIVATE
        ${PROJECT_SOURCE_DIR}/common/mnc/logging)
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

    # The ingestor continuity rules are daemon code; compile the pipeline
    # source into the test rather than opening real devices.
    add_executable(acquisition_architecture_test
        ${PROJECT_SOURCE_DIR}/tests/acquisition_architecture_test.cpp
        ${PROJECT_SOURCE_DIR}/apps/MeterCore/Services/acquisition/pipeline/record_ingestor.cpp)
    target_include_directories(acquisition_architecture_test PRIVATE
        ${PROJECT_SOURCE_DIR}/apps/MeterCore/Services/acquisition)
    target_link_libraries(acquisition_architecture_test PRIVATE
        msap1::acquisition
        mnc::logging)
    target_compile_options(acquisition_architecture_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME acquisition_architecture_test
        COMMAND acquisition_architecture_test)

    add_executable(measurement_timebase_test
        ${PROJECT_SOURCE_DIR}/tests/measurement_timebase_test.cpp)
    target_link_libraries(measurement_timebase_test PRIVATE msap1::meter)
    target_compile_options(measurement_timebase_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME measurement_timebase_test COMMAND measurement_timebase_test)

    add_executable(settings_test
        ${PROJECT_SOURCE_DIR}/tests/settings_test.cpp)
    target_link_libraries(settings_test PRIVATE msap1::settings glaze::glaze)
    target_compile_definitions(settings_test PRIVATE
        PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_compile_options(settings_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME settings_test COMMAND settings_test)
endif()
