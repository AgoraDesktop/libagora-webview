# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: LGPL-3.0-or-later

#===----------------------------------------------------------------------===#
#
# Agora's build for the web engine seam (libagora-webview).
#
# **THIS COMPONENT IS THE FIRST ONE THAT SPEAKS TO A BROWSER ENGINE, AND IT IS A COMPONENT RATHER THAN PART OF THE
# APPKIT ON PURPOSE.** A web view is an application-level idea — no part of Cocoa's contract needs one — and a stack
# that can draw a window should not carry a rendering engine inside it. So the engine is here, behind `AGWebEngine.h`,
# and AppKit stays the thing it was; the day a `WKWebView`-shaped class exists it is a thin client of this seam.
#
# THE ENGINE IS A SYSTEM LIBRARY, ON THE SAME TERMS AS WAYLAND, XKBCOMMON, ICU AND LIBCURL. WPE WebKit is LGPL-2.1
# (dynamically linked, which is what this project's rules allow for LGPL), `libwpe` and `libwpebackend-fdo` are BSD-2,
# and none of them is vendored, patched or pinned by this repository. A machine without them does not build this
# component — which is the honest failure, since the component IS the engine's host and has nothing to do without one.
#
#===----------------------------------------------------------------------===#

# **A LOCALLY BUILT WPE WEBKIT, FOR THE ONE FEATURE THE DISTRO SWITCHES OFF.** Empty means "the system one", which is the
# normal case. Point it at a fork built with `ENABLE_DRAG_SUPPORT=ON` (see `~/Development/wpewebkit`) and the tests — and
# the demo, through the same variable — use that library and let the view send real drags instead of emulating them.
set(AGORA_WPE_PREFIX "" CACHE PATH "Install prefix of a locally built WPE WebKit; empty uses the system one")

cmake_minimum_required(VERSION 3.24)

project(AgoraWebView LANGUAGES C OBJC)

set(CMAKE_INSTALL_RPATH "\$ORIGIN")
include(GNUInstallDirs)

set(CMAKE_POSITION_INDEPENDENT_CODE YES)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WPE_WEBBACKEND REQUIRED IMPORTED_TARGET wpe-webkit-2.0 wpebackend-fdo-1.0 wpe-1.0)
# **THE SHM ACCESSORS LIVE IN libwayland-server**, and a host that reads an exported frame needs them: see the note in
# `AGWebEngine-WPE.c`. This is a link dependency and not a protocol one — nothing here is a compositor.
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)

find_package(AgoraApp REQUIRED)

# ---------------------------------------------------------------------------
# Include shim: the header is included as <AgoraWebView/AGWebEngine.h>, which is how it is INSTALLED and therefore how a
# consumer writes it — in the build tree the same spelling has to work, so one symlink does what a second include path
# would do less honestly.
# ---------------------------------------------------------------------------
set(_agora_webview_include_dir "${CMAKE_BINARY_DIR}/agora-include")
file(REMOVE_RECURSE "${_agora_webview_include_dir}")
file(MAKE_DIRECTORY "${_agora_webview_include_dir}")
file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}" "${_agora_webview_include_dir}/AgoraWebView" SYMBOLIC)

add_library(WebView SHARED
  AGWebEngine-WPE.c
  WKWebView.m)

target_include_directories(WebView PUBLIC
  "$<BUILD_INTERFACE:${_agora_webview_include_dir}>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# **PUBLIC, AND THAT IS THE INTERFACE PROMISE.** `AGWebEngine.h` names no engine type and no WPE type, so the header is
# the whole of what a consumer sees; the WPE link is here so that the library's own .so resolves its imports.
target_link_libraries(WebView
  PUBLIC AgoraApp::App
  PRIVATE PkgConfig::WPE_WEBBACKEND PkgConfig::WAYLAND_SERVER)

set_target_properties(WebView PROPERTIES OUTPUT_NAME agora-webview)

# **AND ONE APPLICATION, BECAUSE A CLASS NOBODY CAN LOOK AT IS A CLASS NOBODY TRUSTS.** Not installed as a tool: it is a
# demo client, started by `demo/agora-webview.sh`, and it is what makes `WKWebView` a thing on a screen rather than a thing
# in a header.
add_executable(agora-webview tools/webview.m)
target_link_libraries(agora-webview PRIVATE WebView)

enable_testing()
add_subdirectory(tests)

install(TARGETS WebView
    EXPORT AgoraWebViewTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES AGWebEngine.h WKWebView.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/AgoraWebView)

include(CMakePackageConfigHelpers)

install(EXPORT AgoraWebViewTargets
    FILE AgoraWebViewTargets.cmake
    NAMESPACE AgoraWebView::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraWebView)

configure_package_config_file(cmake/AgoraWebViewConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraWebViewConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraWebView)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraWebViewConfigVersion.cmake
    VERSION 1.0.0
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraWebViewConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraWebViewConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraWebView)
