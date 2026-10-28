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

# **A LOCALLY BUILT WPE WEBKIT GOES IN FRONT, WHEN ONE IS NAMED.** `AGORA_WPE_PREFIX` is the install of a WebKit fork (see
# `~/Development/wpewebkit`) that has `ENABLE_DRAG_SUPPORT=ON`. Naming it does two things: the library path puts it before
# everything else, and `AGWEBVIEW_REAL_DRAG=1` tells the view to send a real drag instead of the shift-click emulation that
# a stock build needs. MEASURED: the same drag selects 29 characters either way, and 0 if the variable is set while the
# system library is loaded — so the two have to travel together, which is exactly what this does.
set(_agora_webview_test_lib_path "$<TARGET_FILE_DIR:WebView>:$<TARGET_FILE_DIR:AgoraApp::App>")
if (AGORA_WPE_PREFIX)
    set(_agora_webview_test_lib_path "${_agora_webview_test_lib_path}:${AGORA_WPE_PREFIX}/lib")
endif()
set(_agora_webview_test_env "LD_LIBRARY_PATH=${_agora_webview_test_lib_path}")
if (AGORA_WPE_PREFIX)
    list(APPEND _agora_webview_test_env "AGWEBVIEW_REAL_DRAG=1")
endif()

# **A COMPONENT'S TESTS MUST NOT NEED A SIBLING'S BUILD TO BE PRESENT**, so the window server is a path that may not
# exist and the runner SKIPS (77) when it does not. Declared as a cache entry above its first use, which is the mistake
# this tree has already made once (see libagora-app's tests).
set(AGORA_WINDOW_SERVER_BINARY "${CMAKE_CURRENT_SOURCE_DIR}/../../window-server/build/window-server"
    CACHE FILEPATH "window-server's built binary, for the tests that need a compositor of our own")

# **LINKS, AND THE ONE CLICK A DRAG MUST NOT FOLLOW.** Two pages on disk, a click on the link between them, back, and the
# suppressed click that must stay put — plus the check that suppression is a switch rather than a latch, since a single drag
# that turned it off for good would leave the browser unable to follow anything.
# **AND HOW LONG THE FIRST LOAD TAKES, WHICH ONCE TOOK 6.5 SECONDS.** The stall was a `drmWaitVBlank()` on a display this
# process does not own; the engine forces the timer-based vblank monitor and this asserts the load starts inside 1.5 s. Set
# `WEBKIT_FORCE_VBLANK_TIMER=0` to watch it fail.
# **THE POINTER OVER A PAGE IS THE PAGE'S DECISION.** Five bands in one page — link, text, crosshair, not-allowed, none —
# and the probe walks the pointer down them. SKIPS on a WebKit without the cursor door (the distro's), because the
# component still works there; it just keeps its own pointer.
add_executable(webengine_cursor webengine_cursor.c)
target_link_libraries(webengine_cursor PRIVATE WebView)

add_test(NAME webengine-cursor COMMAND webengine_cursor)
set_tests_properties(webengine-cursor PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")

add_executable(webengine_startup webengine_startup.c)
target_link_libraries(webengine_startup PRIVATE WebView)

add_test(NAME webengine-startup COMMAND webengine_startup)
set_tests_properties(webengine-startup PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")

add_executable(webengine_links webengine_links.c)
target_link_libraries(webengine_links PRIVATE WebView)

add_test(NAME webengine-links COMMAND webengine_links)
set_tests_properties(webengine-links PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")

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

# **AND THE PAGE'S BARS, WHICH ARE THE ONE PIECE OF WEBKIT'S OWN CHROME A PAGE WEARS ON AGORA'S DESKTOP.** The port draws
# them with WebKit's Adwaita theme as OVERLAY bars, so a page at rest has no visible bar at all; the engine installs a user
# style sheet that redraws them as the AppKit's `NSScroller` (fifteen points, a #d9d9d9 groove, a white knob edged in
# #bcbcbc). This measures the PIXELS and the LAYOUT — styling `::-webkit-scrollbar` makes WebKit take the bar's points out
# of the viewport — and the escape hatch, because "we restyled the bars" and "we drew a grey line somewhere" would
# otherwise pass the same checks.
add_executable(webengine_scrollbar webengine_scrollbar.c)
target_link_libraries(webengine_scrollbar PRIVATE WebView)

add_test(NAME webengine-scrollbar COMMAND webengine_scrollbar)
set_tests_properties(webengine-scrollbar PROPERTIES SKIP_RETURN_CODE 77
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

# **AND THE TOOLBAR, DRIVEN THE WAY A PERSON DRIVES IT**: the field is clicked, characters are typed as real key events
# through the AppKit's field editor, Return sends the field's action, and the Back button's own action navigates. The
# assertion is the page's title twice — the typed address loaded, and Back returned — plus the first page's colour in the
# composed frame.
add_test(NAME webview-toolbar
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-webview-toolbar.sh"
          "$<TARGET_FILE:agora-webview>" "${AGORA_WINDOW_SERVER_BINARY}")
set_tests_properties(webview-toolbar PROPERTIES SKIP_RETURN_CODE 77
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
