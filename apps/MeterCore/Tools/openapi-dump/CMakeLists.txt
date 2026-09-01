add_executable(msap1-openapi-dump
    main.cpp
    ${PROJECT_SOURCE_DIR}/apps/MeterCore/Services/web-backend/api/openapi.cpp
)
target_link_libraries(msap1-openapi-dump PRIVATE msap1-web-api)
target_compile_features(msap1-openapi-dump PRIVATE cxx_std_23)
target_compile_options(msap1-openapi-dump PRIVATE -Wall -Wextra -Wpedantic)

# This executable is a build-hosted contract exporter.  Yocto stages it only
# in the recipe sysroot and deliberately never installs it in the target image.
if(BUILD_TESTING)
    add_test(NAME openapi_contract_test COMMAND msap1-openapi-dump --check)
endif()
