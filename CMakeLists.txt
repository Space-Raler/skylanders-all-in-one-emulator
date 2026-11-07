# SkyPortal as a plain PS5 payload (ELF). Build with the PS5 payload SDK toolchain; libhijacker's headers
# and libhijacker.a come from an etaHEN-SDK checkout (ETAHEN_SDK) -- only that library is used, not etaHEN.
#
#   export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
#   export ETAHEN_SDK=/path/to/etaHEN-SDK
#   cmake --preset ps5 && cmake --build --preset ps5     # -> build-ps5/bin/skyportal.elf
cmake_minimum_required(VERSION 3.20)
project(SkyPortalPayload C CXX)

set(ETAHEN_SDK "" CACHE PATH "etaHEN-SDK checkout providing libhijacker (headers in include/, libhijacker.a)")
if(NOT ETAHEN_SDK AND DEFINED ENV{ETAHEN_SDK})
    set(ETAHEN_SDK "$ENV{ETAHEN_SDK}")
endif()
if(NOT ETAHEN_SDK OR NOT EXISTS "${ETAHEN_SDK}/include/hijacker/hijacker.hpp")
    message(FATAL_ERROR "Set ETAHEN_SDK to an etaHEN-SDK checkout (expects include/hijacker/hijacker.hpp)")
endif()
set(HIJACKER_LIB "")
foreach(_cand "${ETAHEN_SDK}/lib/libhijacker.a" "${ETAHEN_SDK}/libhijacker/libhijacker.a")
    if(NOT HIJACKER_LIB AND EXISTS "${_cand}")
        set(HIJACKER_LIB "${_cand}")
    endif()
endforeach()
if(NOT HIJACKER_LIB)
    message(FATAL_ERROR "libhijacker.a not found in ${ETAHEN_SDK}/lib or ${ETAHEN_SDK}/libhijacker")
endif()
message(STATUS "libhijacker: ${HIJACKER_LIB}")

foreach(h hook_blob.h web_ui.h)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/prebuilt/${h}")
        message(FATAL_ERROR "prebuilt/${h} missing: run `make prebuilt` (host cc, ld, python3)")
    endif()
endforeach()

if(EXISTS "${PS5_PAYLOAD_SDK}/include")
    set(_payload_include "${PS5_PAYLOAD_SDK}/include")
else()
    set(_payload_include "${PS5_PAYLOAD_SDK}/target/include")
endif()

add_executable(skyportal
    plugin/main_payload.cpp plugin/proc_hijacker.cpp plugin/offsets_override.cpp plugin/http_srv.c
    src/daemon.c src/injector.c src/api.c src/sky_portal.c)
set_target_properties(skyportal PROPERTIES OUTPUT_NAME "skyportal.elf"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Same target flags as the Onion/etaHEN SDKs use for PS5 ELFs.
target_compile_definitions(skyportal PRIVATE PPR PS5 SKY_HAVE_PS5_SDK=1 _DEFAULT_SOURCE)
target_compile_options(skyportal PRIVATE
    --target=x86_64-sie-ps5 -march=znver2 -fPIC -fPIE
    -ffunction-sections -fdata-sections -Wall -Wno-keyword-macro -fstack-protector-all)
target_compile_features(skyportal PRIVATE c_std_11 cxx_std_20)
target_include_directories(skyportal PRIVATE
    include hook plugin prebuilt
    "${PS5_PAYLOAD_SDK}" "${_payload_include}"
    "${ETAHEN_SDK}/include")
if(EXISTS "${PS5_PAYLOAD_SDK}/target/lib")
    target_link_directories(skyportal PRIVATE "${PS5_PAYLOAD_SDK}/target/lib")
endif()
target_link_directories(skyportal PRIVATE "${ETAHEN_SDK}/lib")
# Libraries that export what we call (see the etaHEN build notes): SceSysCore (_sceApplicationGetAppId),
# SceSystemService (running-app queries), kernel_sys (suspend/resume, notifications), SceNet (HTTP sockets).
target_link_libraries(skyportal PRIVATE
    "${HIJACKER_LIB}" SceLibcInternal SceSystemService SceNet SceSysCore kernel_sys)
