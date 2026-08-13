add_executable(msap1-meter-historian main.cpp meter_historian_service.cpp)
target_include_directories(msap1-meter-historian PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(msap1-meter-historian PRIVATE
    msap1::meter msap1::meter-data-provider-stream msap1::settings
    mnc::logging mnc::service mnc::ipc)
target_compile_features(msap1-meter-historian PRIVATE cxx_std_23)
target_compile_options(msap1-meter-historian PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS msap1-meter-historian RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
