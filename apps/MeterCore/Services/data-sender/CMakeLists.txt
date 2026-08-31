add_executable(msap1-data-sender
    main.cpp
    data_sender_service.cpp
)
target_include_directories(msap1-data-sender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(msap1-data-sender PRIVATE
    msap1::datalogger
    msap1::settings
    mnc::service
    mnc::logging
    glaze::glaze
)
target_compile_features(msap1-data-sender PRIVATE cxx_std_23)
target_compile_options(msap1-data-sender PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS msap1-data-sender RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
