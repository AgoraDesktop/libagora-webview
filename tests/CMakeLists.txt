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

add_test(NAME webengine-page
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-webengine.sh"
          "$<TARGET_FILE:webengine_probe>" "${AGORA_WINDOW_SERVER_BINARY}")
set_tests_properties(webengine-page PROPERTIES SKIP_RETURN_CODE 77
  ENVIRONMENT "${_agora_webview_test_env}")
