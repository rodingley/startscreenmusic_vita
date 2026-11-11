cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "VITASDK is not set")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")
endif()

project(StartScreenMusic VERSION 1.0 LANGUAGES C CXX ASM)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

add_executable(startscreenmusic src/main.cpp src/cover_image.cpp src/updater.cpp third_party/picojpeg/picojpeg.c
    third_party/uzlib/tinflate.c third_party/uzlib/adler32.c third_party/uzlib/crc32.c
    src/appmgr_stub.S src/sysmem_stub.S)
target_include_directories(startscreenmusic PRIVATE third_party/paf/include third_party/picojpeg third_party/uzlib)
# No C runtime inside SceShell; PAF's 16-bit wchar_t; no static constructors.
target_compile_options(startscreenmusic PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-O2;-fno-rtti;-fno-exceptions;-fno-unwind-tables;-fno-builtin;-fshort-wchar;-fno-threadsafe-statics;-fno-sized-deallocation;-Wno-template-body;-D__declspec(x)=>"
    "$<$<COMPILE_LANGUAGE:C>:-O2;-fno-unwind-tables;-fno-builtin;-fshort-wchar;-Wno-shift-negative-value>")
target_compile_definitions(startscreenmusic PRIVATE SSM_VERSION="${PROJECT_VERSION}")
target_link_options(startscreenmusic PRIVATE -nostartfiles -nostdlib)
target_link_libraries(startscreenmusic
    taihen_stub
    SceLibKernel_stub
    SceIofilemgr_stub
    SceKernelThreadMgr_stub
    SceKernelModulemgr_stub
    SceAVConfig_stub
    SceTouch_stub
    gcc)

vita_create_self(startscreenmusic.suprx startscreenmusic
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml UNSAFE)

# make release: the plugin into release/, the copy the updater downloads
# (commit it, then tag the release).
add_custom_target(release
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target all
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/release
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/startscreenmusic.suprx ${CMAKE_SOURCE_DIR}/release/
    COMMAND ${CMAKE_COMMAND} -E echo "release/startscreenmusic.suprx is ${PROJECT_VERSION}. Commit it and tag v${PROJECT_VERSION}.")

# make send: the plugin to the Vita's ur0:tai over VitaShell's FTP (press
# SELECT in VitaShell first); make log: its log and new screenshots into build/.
# Set once with: cmake -DVITA_IP=<the IP VitaShell shows> ..
set(VITA_IP "" CACHE STRING "The PS Vita's IP, as VitaShell's FTP screen shows it")
if(VITA_IP)
    # Builds first (a target's DEPENDS names files, so without this, send could
    # send the old one), then sends.
    add_custom_target(send
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target all
        COMMAND curl --fail -sS -T ${CMAKE_BINARY_DIR}/startscreenmusic.suprx ftp://${VITA_IP}:1337/ur0:/tai/
        COMMAND ${CMAKE_COMMAND} -E echo "Sent ${PROJECT_VERSION}. Restart the Vita to load it.")
    add_custom_target(log
        COMMAND sh ${CMAKE_SOURCE_DIR}/tools/fetch_log.sh ${VITA_IP} ${CMAKE_BINARY_DIR})
endif()
