cmake_minimum_required(VERSION 3.21)

project(msap1_apu VERSION 1.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MSAP1_BUILD_WEB_BACKEND "Build the MSAP1 WebEngine/nginx backend" ON)

add_subdirectory(libs/openamp-helper EXCLUDE_FROM_ALL)
set(glaze_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(glaze_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(glaze_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/glaze EXCLUDE_FROM_ALL)
add_subdirectory(lib)
add_subdirectory(apps/cli)
add_subdirectory(apps/acquisition)
add_subdirectory(apps/service-manager)

if(MSAP1_BUILD_WEB_BACKEND)
    set(WEBENGINE_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(WEBENGINE_FETCH_DEPS OFF CACHE BOOL "" FORCE)
    add_subdirectory(libs/webengine/backend EXCLUDE_FROM_ALL)
    add_subdirectory(apps/web-backend)
endif()
