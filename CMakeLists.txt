cmake_minimum_required(VERSION 3.10)

## VitaSDK toolchain bootstrap ------------------------------------------------
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "VitaSDK toolchain")
  else()
    message(FATAL_ERROR "VITASDK is not set. `export VITASDK=/usr/local/vitasdk`.")
  endif()
endif()

project(cordless C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

## App identity ---------------------------------------------------------------
set(VITA_APP_NAME  "Cordless")
set(VITA_TITLEID   "CRDL00001")
set(VITA_VERSION   "01.00")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -O2 -g)
# Enable RTTI and exceptions; silence psabi notes.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions -Wno-psabi")

## Exit after auto-running spikes on boot.
option(CORDLESS_AUTOEXIT "Exit after auto-running spikes (for Vita3K capture)" OFF)

## Keep chain-trust verification on but skip TLS hostname matching.
option(CORDLESS_INSECURE_TLS "Disable TLS hostname matching (Vita3K only)" OFF)

## Sources --------------------------------------------------------------------
add_executable(${PROJECT_NAME}
  src/main.c
  src/console.c
  src/net_common.c
  src/tls/vita_mbedtls_port.c
  src/tls/https_get.c
  src/tls/ca_bundle.c
  src/tls/ws_client.c
  src/media/mp4.c
  src/media/video_player.c
  src/util/b64.c
  src/util/clipboard.c
  src/auth/racrypto.c
  src/auth/remoteauth.c
  src/auth/tokenstore.c
  src/api/discord.c
  src/gateway/gateway.c
  src/voice/voice.c
  src/voice/voice_crypto.c
  src/voice/voice_audio.c
  src/voice/dave_frame.c
  src/voice/dave.cpp
  src/model/store.c
  src/model/settings.c
  src/ui/qr.c
  src/ui/ime.c
  src/ui/inlinekbd.c
  src/ui/photo.c
  src/ui/camera.c
  src/ui/theme.c
  src/ui/ui.c
  src/ui/browser.c
  src/ui/imagecache.c
  third_party/qrcodegen.c
  third_party/stb_impl.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
  src
  third_party
  third_party/mbedtls/include
  # vendored mlspp headers plus bytes/tls/hpke helpers
  third_party/mlspp/include
  third_party/mlspp/lib/bytes/include
  third_party/mlspp/lib/tls_syntax/include
  third_party/mlspp/lib/hpke/include
)

## Pass the title ID to the app as APP_TITLEID.
target_compile_definitions(${PROJECT_NAME} PRIVATE APP_TITLEID="${VITA_TITLEID}")

if(CORDLESS_AUTOEXIT)
  target_compile_definitions(${PROJECT_NAME} PRIVATE CORDLESS_AUTOEXIT=1)
endif()
if(CORDLESS_INSECURE_TLS)
  target_compile_definitions(${PROJECT_NAME} PRIVATE CORDLESS_INSECURE_TLS=1)
endif()
# Optional override for the Discord client_build_number.
# Refresh with scripts/refresh_build_number.sh, then pass -DCLIENT_BUILD_NUMBER=NNN.
if(DEFINED CLIENT_BUILD_NUMBER)
  target_compile_definitions(${PROJECT_NAME} PRIVATE CLIENT_BUILD_NUMBER=${CLIENT_BUILD_NUMBER})
endif()

## Link libraries.
target_link_libraries(${PROJECT_NAME}
  # JSON
  jansson
  # MLS engine for DAVE voice E2EE (mlspp core + mbedTLS HPKE shim).
  ${CMAKE_SOURCE_DIR}/third_party/mlspp/lib/libmlspp_core.a
  # mbedTLS 3.6 (vendored) TLS libraries
  ${CMAKE_SOURCE_DIR}/third_party/mbedtls/lib/libmbedtls.a
  ${CMAKE_SOURCE_DIR}/third_party/mbedtls/lib/libmbedx509.a
  ${CMAKE_SOURCE_DIR}/third_party/mbedtls/lib/libmbedcrypto.a
  # graphics libraries
  vita2d
  freetype png jpeg webpdemux webp pthread bz2 z
  # Opus codec for voice audio (VitaSDK portlib)
  opus
  ScePgf_stub ScePvf_stub
  # Sce stubs
  SceNet_stub SceNetCtl_stub
  SceLibKernel_stub SceKernelThreadMgr_stub SceKernelModulemgr_stub
  SceSysmodule_stub SceProcessmgr_stub SceIofilemgr_stub
  SceRtc_stub SceCommonDialog_stub SceIme_stub
  SceDisplay_stub SceGxm_stub SceCtrl_stub
  SceAppMgr_stub SceAppUtil_stub SceTouch_stub SceHid_stub ScePower_stub
  SceVideodec_stub SceCodecEngine_stub
  SceAudiodec_stub SceAudio_stub SceAudioIn_stub
  SceClipboard_stub
  m
)

## VPK assembly ---------------------------------------------------------------
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME    ${VITA_APP_NAME}
  # Custom LiveArea art (scripts/gen_livearea.py).
  FILE    sce_sys/icon0.png                     sce_sys/icon0.png
  FILE    sce_sys/pic0.png                      sce_sys/pic0.png
  FILE    sce_sys/livearea/contents/bg.png      sce_sys/livearea/contents/bg.png
  FILE    sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE    sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE    assets/cacert.pem            data/cacert.pem
  FILE    assets/fonts/Inter-Regular.ttf   data/fonts/Inter-Regular.ttf
  FILE    assets/fonts/Inter-SemiBold.ttf  data/fonts/Inter-SemiBold.ttf
  FILE    assets/emoji_atlas.png           data/emoji_atlas.png
)
