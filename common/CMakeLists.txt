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

# The MQTT scheduler depends only on this narrow interface. The Paho adapter
# is a separate optional target so host-side unit tests can inject a fake even
# when the embedded Paho development package is not installed.
add_library(mnc_mqtt STATIC
    mnc/mqtt/mqtt_client.cpp
)
add_library(mnc::mqtt ALIAS mnc_mqtt)
target_include_directories(mnc_mqtt PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/common>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mnc_mqtt PUBLIC cxx_std_23)
target_compile_definitions(mnc_mqtt PUBLIC BOOST_ERROR_CODE_HEADER_ONLY=1)
target_compile_options(mnc_mqtt PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(mnc_mqtt PUBLIC Threads::Threads)

find_path(PAHO_MQTT_CPP_INCLUDE_DIR mqtt/async_client.h)
find_library(PAHO_MQTT_CPP_LIBRARY NAMES paho-mqttpp3)
find_library(PAHO_MQTT_C_SSL_LIBRARY NAMES paho-mqtt3as)
if(PAHO_MQTT_CPP_INCLUDE_DIR AND PAHO_MQTT_CPP_LIBRARY AND
   PAHO_MQTT_C_SSL_LIBRARY)
    add_library(mnc_paho_mqtt STATIC
        mnc/mqtt/paho_mqtt_client.cpp
    )
    add_library(mnc::paho-mqtt ALIAS mnc_paho_mqtt)
    target_include_directories(mnc_paho_mqtt PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/common>
        $<INSTALL_INTERFACE:include>
        ${PAHO_MQTT_CPP_INCLUDE_DIR}
    )
    target_compile_features(mnc_paho_mqtt PUBLIC cxx_std_23)
    target_compile_definitions(mnc_paho_mqtt PUBLIC
        BOOST_ERROR_CODE_HEADER_ONLY=1)
    target_compile_options(mnc_paho_mqtt PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(mnc_paho_mqtt PUBLIC
        mnc::mqtt
        ${PAHO_MQTT_CPP_LIBRARY}
        ${PAHO_MQTT_C_SSL_LIBRARY}
        Threads::Threads)
else()
    message(STATUS
        "Paho MQTT C++ not found; MQTT service is omitted from this host build")
endif()

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
	msap1/meter/energy_demand.cpp
	msap1/meter/harmonic_spectrum.cpp
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
	msap1/meter/energy_ledger.cpp
    msap1/meter/MeterDataProvider/stream/meter_stream_ipc.cpp
    msap1/meter/history/meter_history.cpp
    msap1/meter/history/historian_ipc.cpp
)
target_link_libraries(msap1_meter_data_provider_stream PUBLIC
    msap1::meter
    mnc::meter-data-provider-stream
    mnc::ipc
)
target_link_libraries(msap1_meter_data_provider_stream PRIVATE
	mnc::sqlite
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
target_link_libraries(msap1_settings PUBLIC
    glaze::glaze
)
target_link_libraries(msap1_settings PRIVATE
    OpenSSL::Crypto
)

# Waveform device/session ownership is isolated from the meter record path so
# future analysis code does not acquire access to the DMA descriptor.
msap1_library(msap1_waveform msap1::waveform
    msap1/waveform/mncwf_v4.cpp
    msap1/waveform/waveform_capture.cpp
)
target_link_libraries(msap1_waveform PRIVATE mnc::logging)

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
	msap1/modbus/register_map/register_map_export.cpp
)
target_link_libraries(msap1_modbus PUBLIC
	mnc::modbus
	mnc::meter-data-provider
)
target_link_libraries(msap1_modbus PRIVATE glaze::glaze)

msap1_library(msap1_mqtt msap1::mqtt
    msap1/mqtt/meter_publication_catalog.cpp
    msap1/mqtt/meter_snapshot_payload_encoder.cpp
    msap1/mqtt/meter_publication_scheduler.cpp
    msap1/mqtt/mqtt_ipc.cpp
)
target_link_libraries(msap1_mqtt PUBLIC
    mnc::mqtt
    mnc::meter-data-provider
    mnc::ipc
    msap1::meter
)
target_link_libraries(msap1_mqtt PRIVATE glaze::glaze)

msap1_library(msap1_system msap1::system
    msap1/system/rpu_runtime_status.cpp
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

    # Public meter-record IDs are a co-release contract with PL/R5C1.
    add_executable(meter_record_contract_test
        ${PROJECT_SOURCE_DIR}/tests/meter_record_contract_test.cpp)
    target_link_libraries(meter_record_contract_test PRIVATE msap1::meter)
    target_compile_options(meter_record_contract_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME meter_record_contract_test COMMAND meter_record_contract_test)

    # The reference aggregator under tests/support is deliberately test-only:
    # R5C1 is the authoritative aggregator and APU production code never
    # computes aggregates, so only this test target sees tests/ includes.
    add_executable(aggregate_decode_test
        ${PROJECT_SOURCE_DIR}/tests/aggregate_decode_test.cpp)
    target_link_libraries(aggregate_decode_test PRIVATE msap1::meter)
    target_include_directories(aggregate_decode_test PRIVATE
        ${PROJECT_SOURCE_DIR}/tests)
    target_compile_options(aggregate_decode_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME aggregate_decode_test COMMAND aggregate_decode_test)

    # The ADC-simulator golden model (tests/support/waveform_golden.hpp)
    # and the engineering-units -> counts wire conversion must agree; this
    # is the host half of the simulator verification framework.
    add_executable(waveform_golden_test
        ${PROJECT_SOURCE_DIR}/tests/waveform_golden_test.cpp)
    target_link_libraries(waveform_golden_test PRIVATE msap1::meter)
    target_include_directories(waveform_golden_test PRIVATE
        ${PROJECT_SOURCE_DIR}/tests)
    target_compile_options(waveform_golden_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME waveform_golden_test COMMAND waveform_golden_test)

    # SCYC-v2 single-cycle diagnostic decode pinned against the PL word map.
    add_executable(single_cycle_decode_test
        ${PROJECT_SOURCE_DIR}/tests/single_cycle_decode_test.cpp)
    target_link_libraries(single_cycle_decode_test PRIVATE msap1::meter)
    target_compile_options(single_cycle_decode_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME single_cycle_decode_test COMMAND single_cycle_decode_test)

    # PQEVT-v1 power-quality decode pinned against the PL word map.
    add_executable(pq_event_decode_test
        ${PROJECT_SOURCE_DIR}/tests/pq_event_decode_test.cpp)
    target_link_libraries(pq_event_decode_test PRIVATE msap1::meter)
    target_compile_options(pq_event_decode_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME pq_event_decode_test COMMAND pq_event_decode_test)

	# M18 final FLICKER-v1 public record decode and reserved-field guards.
	add_executable(flicker_decode_test
		${PROJECT_SOURCE_DIR}/tests/flicker_decode_test.cpp)
	target_link_libraries(flicker_decode_test PRIVATE msap1::meter)
	target_compile_options(flicker_decode_test PRIVATE
		-Wall -Wextra -Wpedantic)
	add_test(NAME flicker_decode_test COMMAND flicker_decode_test)

	# Final MAINS-SIGNAL-v1 public record decode and provenance guards.
	add_executable(mains_signal_decode_test
		${PROJECT_SOURCE_DIR}/tests/mains_signal_decode_test.cpp)
	target_link_libraries(mains_signal_decode_test PRIVATE msap1::meter)
	target_compile_options(mains_signal_decode_test PRIVATE
		-Wall -Wextra -Wpedantic)
	add_test(NAME mains_signal_decode_test COMMAND mains_signal_decode_test)

	# M18 durable lifecycle catalogue, replay idempotence, and waveform links.
	add_executable(pq_event_history_test
		${PROJECT_SOURCE_DIR}/tests/pq_event_history_test.cpp)
	target_link_libraries(pq_event_history_test PRIVATE
		msap1::meter-data-provider-stream)
	target_compile_options(pq_event_history_test PRIVATE
		-Wall -Wextra -Wpedantic)
	add_test(NAME pq_event_history_test COMMAND pq_event_history_test)

	# M16 HARMONIC-v1 decode and atomic 42-record family assembly.
	add_executable(harmonic_decode_test
		${PROJECT_SOURCE_DIR}/tests/harmonic_decode_test.cpp)
	target_link_libraries(harmonic_decode_test PRIVATE msap1::meter)
	target_compile_options(harmonic_decode_test PRIVATE
		-Wall -Wextra -Wpedantic)
	add_test(NAME harmonic_decode_test COMMAND harmonic_decode_test)

	# M17 ENERGY-v1 two-part family and DEMAND-v1 typed decode.
	add_executable(energy_demand_decode_test
		${PROJECT_SOURCE_DIR}/tests/energy_demand_decode_test.cpp)
	target_link_libraries(energy_demand_decode_test PRIVATE msap1::meter)
	target_compile_options(energy_demand_decode_test PRIVATE
		-Wall -Wextra -Wpedantic)
	add_test(NAME energy_demand_decode_test COMMAND energy_demand_decode_test)

	add_executable(energy_ledger_test
		${PROJECT_SOURCE_DIR}/tests/energy_ledger_test.cpp)
	target_link_libraries(energy_ledger_test PRIVATE
		msap1::meter-data-provider-stream)
	target_compile_options(energy_ledger_test PRIVATE
		-Wall -Wextra -Wpedantic)
	add_test(NAME energy_ledger_test COMMAND energy_ledger_test)

	add_executable(energy_dto_test
		${PROJECT_SOURCE_DIR}/tests/energy_dto_test.cpp)
	target_include_directories(energy_dto_test PRIVATE
		${PROJECT_SOURCE_DIR}/apps/MeterCore/Services/web-backend/api)
	target_link_libraries(energy_dto_test PRIVATE msap1::meter glaze::glaze)
	target_compile_options(energy_dto_test PRIVATE -Wall -Wextra -Wpedantic)
	add_test(NAME energy_dto_test COMMAND energy_dto_test)

    # GET /api/v1/meter/aggregate is pinned by its typed snapshot selection
    # and JSON payload. The projection is deliberately WebEngine-free, so the
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

    # GET /api/v1/meter/minutes-10 projects the typed provider snapshot;
    # pin that JSON contract without linking the HTTP stack.
    add_executable(meter_ten_minute_route_test
        ${PROJECT_SOURCE_DIR}/tests/meter_ten_minute_route_test.cpp)
    target_link_libraries(meter_ten_minute_route_test PRIVATE
        msap1::acquisition
        glaze::glaze
    )
    target_include_directories(meter_ten_minute_route_test PRIVATE
        ${PROJECT_SOURCE_DIR}/apps/MeterCore/Services/web-backend/api)
    target_compile_options(meter_ten_minute_route_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME meter_ten_minute_route_test
        COMMAND meter_ten_minute_route_test)

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

    # Live target-only M15 probe. It is deliberately excluded from the normal
    # build and CTest: it requires a running meter-stream service and must span
    # a programmed UTC ten-minute boundary. Build it explicitly when producing
    # target evidence; tests/target/README.md documents the safe procedure.
    add_executable(m15_utc_overlap_target_test EXCLUDE_FROM_ALL
        ${PROJECT_SOURCE_DIR}/tests/target/m15_utc_overlap_target_test.cpp)
    target_link_libraries(m15_utc_overlap_target_test PRIVATE
        msap1::meter-data-provider-stream)
    target_compile_options(m15_utc_overlap_target_test PRIVATE
        -Wall -Wextra -Wpedantic)

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

    add_executable(mncwf_v4_reader_test
        ${PROJECT_SOURCE_DIR}/tests/mncwf_v4_reader_test.cpp)
    target_link_libraries(mncwf_v4_reader_test PRIVATE msap1::waveform)
    target_compile_options(mncwf_v4_reader_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME mncwf_v4_reader_test COMMAND mncwf_v4_reader_test)

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

    add_executable(mqtt_test
        ${PROJECT_SOURCE_DIR}/tests/mqtt_test.cpp)
    target_link_libraries(mqtt_test PRIVATE
        msap1::mqtt
        glaze::glaze)
    target_compile_options(mqtt_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME mqtt_test COMMAND mqtt_test)
endif()
