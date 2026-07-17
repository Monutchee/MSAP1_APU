cmake_minimum_required(VERSION 3.16)

project(msap1_apu VERSION 1.0.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

add_subdirectory(libs/openamp-helper EXCLUDE_FROM_ALL)

add_library(msap1_apu STATIC
    src/protocol.cpp
    src/rpmsg_endpoint.cpp
    src/visualizer.cpp
)
target_link_libraries(msap1_apu PRIVATE mnc::rpmsg_chrdev)
target_include_directories(msap1_apu PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(msap1_apu PUBLIC cxx_std_17)
target_compile_options(msap1_apu PRIVATE -Wall -Wextra -Wpedantic)

add_executable(msap1-apu-app src/main.cpp)
target_link_libraries(msap1-apu-app PRIVATE msap1_apu)
target_compile_options(msap1-apu-app PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS msap1-apu-app RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
    add_executable(protocol_test tests/protocol_test.cpp)
    target_link_libraries(protocol_test PRIVATE msap1_apu)
    add_test(NAME protocol_test COMMAND protocol_test)
endif()
