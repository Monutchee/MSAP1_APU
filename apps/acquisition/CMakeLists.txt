add_executable(msap1-fpga-acquisition main.cpp)
target_link_libraries(msap1-fpga-acquisition PRIVATE
    msap1::apu-core
    mnc::logging
)
target_compile_features(msap1-fpga-acquisition PRIVATE cxx_std_23)
target_compile_options(msap1-fpga-acquisition PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS msap1-fpga-acquisition
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
