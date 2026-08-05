# Yocto SDK cross-toolchain for the MSAP1 APU (aarch64 Linux).
#
# The SDK is produced with:  bitbake msap1-image -c populate_sdk
# and installed by running the resulting tmp/deploy/sdk/*.sh installer.
#
# The SDK install root is resolved in this order:
#   1. -DMSAP1_SDK_ROOT=<path> on the cmake command line, or the
#      MSAP1_SDK_ROOT environment variable
#   2. SDKTARGETSYSROOT from a sourced environment-setup-* script
#   3. The default install location /opt/monutchee/workspace/msap1/mncos-msap1-toolchain
#   4. Newest aarch64 SDK found under /opt/*/ or /opt/*/*/

if(NOT DEFINED MSAP1_SDK_ROOT OR MSAP1_SDK_ROOT STREQUAL "")
    if(DEFINED ENV{MSAP1_SDK_ROOT} AND NOT "$ENV{MSAP1_SDK_ROOT}" STREQUAL "")
        set(MSAP1_SDK_ROOT "$ENV{MSAP1_SDK_ROOT}")
    elseif(DEFINED ENV{SDKTARGETSYSROOT} AND NOT "$ENV{SDKTARGETSYSROOT}" STREQUAL "")
        get_filename_component(MSAP1_SDK_ROOT "$ENV{SDKTARGETSYSROOT}/../.." ABSOLUTE)
    elseif(IS_DIRECTORY "/opt/monutchee/workspace/msap1/mncos-msap1-toolchain/sysroots")
        set(MSAP1_SDK_ROOT "/opt/monutchee/workspace/msap1/mncos-msap1-toolchain")
    else()
        file(GLOB _msap1_env_scripts
            "/opt/*/environment-setup-*"
            "/opt/*/*/environment-setup-*")
        foreach(_msap1_script IN LISTS _msap1_env_scripts)
            get_filename_component(_msap1_name "${_msap1_script}" NAME)
            # Only accept 64-bit ARM SDKs; other product SDKs (e.g. armv7)
            # may be installed on the same machine.
            if(_msap1_name MATCHES "aarch64|cortexa")
                get_filename_component(MSAP1_SDK_ROOT "${_msap1_script}" DIRECTORY)
            endif()
        endforeach()
    endif()
endif()
set(MSAP1_SDK_ROOT "${MSAP1_SDK_ROOT}" CACHE PATH "Yocto SDK install root for the MSAP1 APU toolchain")

if(NOT MSAP1_SDK_ROOT OR NOT IS_DIRECTORY "${MSAP1_SDK_ROOT}/sysroots")
    message(FATAL_ERROR
        "MSAP1 Yocto SDK not found (looked for <sdk>/sysroots).\n"
        "Build it with:   bitbake msap1-image -c populate_sdk\n"
        "install the tmp/deploy/sdk/*.sh installer, then either install it "
        "under /opt or pass its location with -DMSAP1_SDK_ROOT=<path> "
        "(or export MSAP1_SDK_ROOT).")
endif()

# sysroots/ contains exactly two entries: the x86_64 host tools and the
# target sysroot (name varies with the tune, e.g. cortexa72-cortexa53-*).
file(GLOB _msap1_all_sysroots "${MSAP1_SDK_ROOT}/sysroots/*")
set(_msap1_native_sysroot "")
set(_msap1_target_sysroot "")
foreach(_msap1_dir IN LISTS _msap1_all_sysroots)
    get_filename_component(_msap1_name "${_msap1_dir}" NAME)
    if(_msap1_name MATCHES "^x86_64-")
        set(_msap1_native_sysroot "${_msap1_dir}")
    else()
        set(_msap1_target_sysroot "${_msap1_dir}")
    endif()
endforeach()

file(GLOB _msap1_gxx
    "${_msap1_native_sysroot}/usr/bin/*/aarch64-*-g++"
    "${_msap1_native_sysroot}/usr/bin/aarch64-*-g++")
file(GLOB _msap1_gcc
    "${_msap1_native_sysroot}/usr/bin/*/aarch64-*-gcc"
    "${_msap1_native_sysroot}/usr/bin/aarch64-*-gcc")
if(NOT _msap1_gxx OR NOT _msap1_gcc OR NOT _msap1_target_sysroot)
    message(FATAL_ERROR
        "SDK at ${MSAP1_SDK_ROOT} looks incomplete: could not locate the "
        "aarch64 g++/gcc or the target sysroot under sysroots/.")
endif()
list(GET _msap1_gxx 0 _msap1_gxx)
list(GET _msap1_gcc 0 _msap1_gcc)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   "${_msap1_gcc}")
set(CMAKE_CXX_COMPILER "${_msap1_gxx}")
set(CMAKE_SYSROOT      "${_msap1_target_sysroot}")

# Search only the target sysroot for headers/libs/packages; never run
# target binaries from it.
set(CMAKE_FIND_ROOT_PATH "${_msap1_target_sysroot}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Make pkg_check_modules() (libsystemd, ...) resolve against the target
# sysroot instead of the host.
set(ENV{PKG_CONFIG_LIBDIR}
    "${_msap1_target_sysroot}/usr/lib/pkgconfig:${_msap1_target_sysroot}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_msap1_target_sysroot}")
