add_executable(msap1-service-manager main.cpp)
target_link_libraries(msap1-service-manager PRIVATE
    msap1::apu-core
    mnc::ipc
    mnc::logging
    mnc::service
)
target_compile_features(msap1-service-manager PRIVATE cxx_std_23)
target_compile_options(msap1-service-manager PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS msap1-service-manager
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
