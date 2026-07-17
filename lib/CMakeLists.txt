add_library(msap1_apu_core STATIC
    ${PROJECT_SOURCE_DIR}/src/acquisition_ipc.cpp
    ${PROJECT_SOURCE_DIR}/src/protocol.cpp
    ${PROJECT_SOURCE_DIR}/src/rpmsg_endpoint.cpp
    ${PROJECT_SOURCE_DIR}/src/shared_ring.cpp
    ${PROJECT_SOURCE_DIR}/src/visualizer.cpp
)
add_library(msap1::apu-core ALIAS msap1_apu_core)

target_link_libraries(msap1_apu_core PRIVATE mnc::rpmsg_chrdev)
target_include_directories(msap1_apu_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(msap1_apu_core PUBLIC cxx_std_23)
target_compile_options(msap1_apu_core PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(protocol_test ${PROJECT_SOURCE_DIR}/tests/protocol_test.cpp)
    target_link_libraries(protocol_test PRIVATE msap1::apu-core)
    target_compile_options(protocol_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME protocol_test COMMAND protocol_test)
endif()
