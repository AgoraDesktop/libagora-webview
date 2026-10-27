# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# libagora-webview's tests.
#
# **THE ENGINE IS MEASURED AGAINST AGORA'S OWN WINDOW SERVER, AND THE ASSERTION IS THE COMPOSITED SCREENSHOT.** A page
# that renders is the whole claim this component makes, so the probe draws what the engine gives it into a real window
# and the server photographs the result: red where the page's strip is, blue where its background is. Nothing here
# asserts a number the engine itself reported, because an engine reporting its own progress is not evidence that
# anything was drawn.

set(_agora_webview_test_env
    "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:WebView>:$<TARGET_FILE_DIR:AgoraApp::App>")

# **A COMPONENT'S TESTS MUST NOT NEED A SIBLING'S BUILD TO BE PRESENT**, so the window server is a path that may not
# exist and the runner SKIPS (77) when it does not. Declared as a cache entry above its first use, which is the mistake
# this tree has already made once (see libagora-app's tests).
set(AGORA_WINDOW_SERVER_BINARY "${CMAKE_CURRENT_SOURCE_DIR}/../../window-server/build/window-server"
    CACHE FILEPATH "window-server's built binary, for the tests that need a compositor of our own")

add_executable(webengine_probe webengine_probe.c)
target_link_libraries(webengine_probe PRIVATE WebView)

# **AND THE LIFETIME, WHICH NEEDS NO COMPOSITOR AT ALL.** See the probe's header for why the page test cannot ask this.
add_executable(webengine_teardown webengine_teardown.c)
target_link_libraries(webengine_teardown PRIVATE WebView)

add_test(NAME webengine-teardown COMMAND webengine_teardown)
set_tests_properties(webengine-teardown PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")

# **INPUT, ASSERTED BY COLOUR RATHER THAN BY DISPATCH.** Four doors, four colours: `:hover`, an inline style set by the
# page's own script on a click, a `keydown` listener with no field focused, and a scroll whose assertion needs the
# DOCUMENT to have moved. No compositor is involved — the engine is independent of the window server — so this runs
# anywhere WebKit does, and it SKIPS (77) when there is none.
add_executable(webengine_input webengine_input.c)
target_link_libraries(webengine_input PRIVATE WebView)

add_test(NAME webengine-input COMMAND webengine_input)
set_tests_properties(webengine-input PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")

# **THE CLASS, OFFSHORE: no compositor, no window, a bitmap rep for a surface.** A page must arrive upright (the rep's
# own orientation is measured first, so "upright" is a claim about the view rather than about the surface), the title
# must be the page's, and the pointer and the wheel must reach the page through the view's own conversion — which is
# where a wrong y flip or a forgotten buffer scale would hide.
add_executable(appkit_webview appkit_webview.m)
target_link_libraries(appkit_webview PRIVATE WebView)

add_test(NAME appkit-webview COMMAND appkit_webview)
set_tests_properties(appkit-webview PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")

# **THE OTHER EXPORT PATH: the same page as EGLImages.** The seam imports each one, blits it flipped, reads it back; the
# assertion is the same red-band-at-the-top one the shm test makes, because a flipped blit or a channel swap produces a
# frame that is not the page. SKIPS when the machine's EGL cannot import a dma-buf (the seam falls back, and says so).
add_executable(webengine_export webengine_export.c)
target_link_libraries(webengine_export PRIVATE WebView)

add_test(NAME webengine-egl COMMAND webengine_export)
set_tests_properties(webengine-egl PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")

# **THE PRESENT MODE: the frame goes into somebody else's GPU buffer, and the engine keeps no pixels.** The destination is
# manufactured by the test (a texture exported to a descriptor), the engine is told to draw into it, and the TEST reads it
# back — so a pass cannot come from the engine quietly reading its own frame, which the last check refuses.
add_executable(webengine_gpu webengine_gpu.c)
target_link_libraries(webengine_gpu PRIVATE WebView)

add_test(NAME webengine-gpu COMMAND webengine_gpu)
set_tests_properties(webengine-gpu PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")

# **AND THE WHOLE WINDOW, GPU TO GPU.** The window's own dma-buf is the destination: BeginFrameDMABUF, the engine draws into
# it on the GPU, EndFrame (or CancelFrame when nothing arrived). Nested and opt-in, because a window only gets a dma-buf
# from a compositor that imports them.
add_executable(webengine_window webengine_window.c)
target_link_libraries(webengine_window PRIVATE WebView)

add_test(NAME webengine-window
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-webengine-window.sh"
          "$<TARGET_FILE:webengine_window>" "${AGORA_WINDOW_SERVER_BINARY}")
set_tests_properties(webengine-window PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")

# **AND THE APPLICATION, NESTED: AppKit → a window's buffer → the compositor → the glass.** Every other test stops short of
# this hop; it is what `demo/agora-webview.sh` starts, and the runner asserts the page's colours in the composed frame.
add_test(NAME webview-app
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-webview-app.sh"
          "$<TARGET_FILE:agora-webview>" "${AGORA_WINDOW_SERVER_BINARY}")
set_tests_properties(webview-app PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")

# **THE TWO NUMBERS A PAGE IS LAID OUT WITH**: the device scale factor (points -> pixels), and a resize that happens while
# the page is still loading. Both are asserted by the page's own geometry: a 40-point band is 80 device rows at factor 2,
# and 40 rows — not 17 — after a resize to 320x200, because 17 is what stretching a 640x480 layout would give.
add_executable(webengine_scale webengine_scale.c)
target_link_libraries(webengine_scale PRIVATE WebView)

add_test(NAME webengine-scale COMMAND webengine_scale)
set_tests_properties(webengine-scale PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")

# **THE CLASS IN A WINDOW, ON A SCALED OUTPUT.** Two bugs a person found in the demo and no test here could see: a window
# resize arriving as `-setFrame:` (which the class did not override, so the page was stretched) and a device scale factor
# read once, too early (so the page rendered at 1x and every glyph was resampled). At --scale 2 both are visible numbers.
add_executable(webview_resize webview_resize.m)
target_link_libraries(webview_resize PRIVATE WebView)

add_test(NAME webview-resize
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-webview-resize.sh"
          "$<TARGET_FILE:webview_resize>" "${AGORA_WINDOW_SERVER_BINARY}")
set_tests_properties(webview-resize PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")

add_test(NAME webengine-page
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-webengine.sh"
          "$<TARGET_FILE:webengine_probe>" "${AGORA_WINDOW_SERVER_BINARY}")
set_tests_properties(webengine-page PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")
