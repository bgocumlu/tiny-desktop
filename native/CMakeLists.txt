cmake_minimum_required(VERSION 3.21)
project(tiny-host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "Use the static MSVC runtime")
  set(WEBVIEW2_VERSION 1.0.4078.44 CACHE STRING "WebView2 SDK version")
  set(WEBVIEW2_ROOT "${CMAKE_BINARY_DIR}/webview2-sdk-${WEBVIEW2_VERSION}")
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

  add_executable(tiny-host WIN32 windows/main.cpp windows/resource.h windows/tiny.rc windows/tiny.ico)
  target_compile_definitions(tiny-host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(tiny-host PRIVATE "${WEBVIEW2_INCLUDE}")
  target_link_libraries(tiny-host PRIVATE "${WEBVIEW2_LIB}" dwmapi version ole32 shell32 shlwapi user32)
elseif(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
  pkg_check_modules(WEBKITGTK IMPORTED_TARGET webkit2gtk-4.1)
  if(NOT WEBKITGTK_FOUND)
    pkg_check_modules(WEBKITGTK REQUIRED IMPORTED_TARGET webkit2gtk-4.0)
  endif()

  add_executable(tiny-host linux/main.cpp)
  target_link_libraries(tiny-host PRIVATE PkgConfig::GTK3 PkgConfig::WEBKITGTK)
elseif(APPLE)
  enable_language(OBJCXX)
  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)

  add_executable(tiny-host macos/main.mm)
  target_link_libraries(tiny-host PRIVATE ${COCOA_FRAMEWORK} ${WEBKIT_FRAMEWORK})
  set_target_properties(tiny-host PROPERTIES MACOSX_DEPLOYMENT_TARGET "12.0")
else()
  message(FATAL_ERROR "Tiny native host is not implemented for this platform yet.")
endif()
