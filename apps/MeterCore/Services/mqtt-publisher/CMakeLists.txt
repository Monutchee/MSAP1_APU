if(TARGET mnc::paho-mqtt)
    add_executable(msap1-mqtt-publisher
        main.cpp
        mqtt_publisher_service.cpp
    )
    target_include_directories(msap1-mqtt-publisher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(msap1-mqtt-publisher PRIVATE
        msap1::mqtt
        msap1::acquisition
        msap1::settings
        mnc::paho-mqtt
        mnc::logging
        mnc::service
    )
    target_compile_features(msap1-mqtt-publisher PRIVATE cxx_std_23)
    target_compile_options(msap1-mqtt-publisher PRIVATE
        -Wall -Wextra -Wpedantic)
    install(TARGETS msap1-mqtt-publisher
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
