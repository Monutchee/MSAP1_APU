add_executable(msap1-web-backend
    main.cpp
    msap1_auth_provider.cpp
    systemd_notifier.cpp
)

target_link_libraries(msap1-web-backend PRIVATE
    msap1::apu-core
    webengine::webengine
    glaze::glaze
)
target_compile_features(msap1-web-backend PRIVATE cxx_std_23)
target_compile_options(msap1-web-backend PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS msap1-web-backend
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
    add_executable(systemd_notifier_test
        ${PROJECT_SOURCE_DIR}/tests/systemd_notifier_test.cpp
        systemd_notifier.cpp
    )
    target_include_directories(systemd_notifier_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(systemd_notifier_test PRIVATE cxx_std_23)
    target_compile_options(systemd_notifier_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME systemd_notifier_test COMMAND systemd_notifier_test)
endif()
