add_executable(modbus-map-dump main.cpp)
target_link_libraries(modbus-map-dump PRIVATE msap1::modbus)
target_compile_features(modbus-map-dump PRIVATE cxx_std_23)
target_compile_options(modbus-map-dump PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS modbus-map-dump
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
