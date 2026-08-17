cmake_minimum_required(VERSION 3.21)
project(tiny-host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "Use the static MSVC runtime")
set(WEBVIEW2_VERSION 1.0.2903.40 CACHE STRING "WebView2 SDK version")
set(WEBVIEW2_ROOT "${CMAKE_BINARY_DIR}/webview2-sdk")
set(WEBVIEW2_PACKAGE "${CMAKE_BINARY_DIR}/webview2-${WEBVIEW2_VERSION}.nupkg")
set(WEBVIEW2_PART "${WEBVIEW2_PACKAGE}.part")

if(NOT EXISTS "${WEBVIEW2_ROOT}/build/native/include/WebView2.h")
  if(NOT EXISTS "${WEBVIEW2_PACKAGE}")
    file(DOWNLOAD
      "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${WEBVIEW2_VERSION}/microsoft.web.webview2.${WEBVIEW2_VERSION}.nupkg"
      "${WEBVIEW2_PART}"
      SHOW_PROGRESS
      INACTIVITY_TIMEOUT 30
      TIMEOUT 120
      STATUS WEBVIEW2_DOWNLOAD_STATUS
    )
    list(GET WEBVIEW2_DOWNLOAD_STATUS 0 WEBVIEW2_DOWNLOAD_CODE)
    if(NOT WEBVIEW2_DOWNLOAD_CODE EQUAL 0)
      message(FATAL_ERROR "Could not download the WebView2 SDK: ${WEBVIEW2_DOWNLOAD_STATUS}")
    endif()
    file(RENAME "${WEBVIEW2_PART}" "${WEBVIEW2_PACKAGE}")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${WEBVIEW2_PACKAGE}" DESTINATION "${WEBVIEW2_ROOT}")
endif()

set(WEBVIEW2_INCLUDE "${WEBVIEW2_ROOT}/build/native/include")
set(WEBVIEW2_LIB "${WEBVIEW2_ROOT}/build/native/x64/WebView2LoaderStatic.lib")

if(NOT EXISTS "${WEBVIEW2_INCLUDE}/WebView2.h" OR NOT EXISTS "${WEBVIEW2_LIB}")
  message(FATAL_ERROR "The WebView2 SDK is missing. Delete ${WEBVIEW2_ROOT} and reconfigure.")
endif()

set(TINY_ICON_PATH "" CACHE FILEPATH "Optional application .ico file")
if(TINY_ICON_PATH)
  if(NOT EXISTS "${TINY_ICON_PATH}")
    message(FATAL_ERROR "TINY_ICON_PATH does not exist: ${TINY_ICON_PATH}")
  endif()
  set(TINY_ICON_SOURCE "${TINY_ICON_PATH}")
else()
  set(TINY_ICON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tiny.ico")
endif()
configure_file("${TINY_ICON_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/tiny.ico" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/tiny.rc" "${CMAKE_CURRENT_BINARY_DIR}/tiny.rc" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resource.h" "${CMAKE_CURRENT_BINARY_DIR}/resource.h" COPYONLY)

add_executable(tiny-host WIN32 main.cpp "${CMAKE_CURRENT_BINARY_DIR}/resource.h" "${CMAKE_CURRENT_BINARY_DIR}/tiny.rc" "${CMAKE_CURRENT_BINARY_DIR}/tiny.ico")
target_compile_definitions(tiny-host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(tiny-host PRIVATE "${WEBVIEW2_INCLUDE}")
target_link_libraries(tiny-host PRIVATE "${WEBVIEW2_LIB}" dwmapi version ole32 shell32 shlwapi user32)
