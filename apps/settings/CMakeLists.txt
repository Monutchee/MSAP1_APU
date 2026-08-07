add_executable(msap1-settings
    main.cpp
    settings_daemon.cpp
    ipc/request_router.cpp
    apply/settings_apply.cpp
)
target_include_directories(msap1-settings PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(msap1-settings PRIVATE
    msap1::settings
    msap1::acquisition
    mnc::logging
    mnc::service
)
target_compile_features(msap1-settings PRIVATE cxx_std_23)
target_compile_options(msap1-settings PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS msap1-settings
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
