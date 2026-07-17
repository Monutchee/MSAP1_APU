add_executable(msap1-apu-app main.cpp)
target_link_libraries(msap1-apu-app PRIVATE msap1::apu-core)
target_compile_features(msap1-apu-app PRIVATE cxx_std_23)
target_compile_options(msap1-apu-app PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS msap1-apu-app RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
