/*
 * webengine_teardown — an engine can be made and dropped, which the page probe cannot ask.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHY THIS IS A TEST OF ITS OWN. The page probe ends when the compositor does — that is how its screenshot gets written
 * — and a WPE view backend destroyed after its session's connection has gone crashes inside `wpe_view_backend_destroy`
 * (measured). So the case that must be asserted is the one a real application has: views destroyed while the session is
 * healthy, which is what `-[NSApplication terminate:]` does. This program needs NO window server at all — the engine's
 * own processes are its business — so it runs anywhere, and it asserts what a web view's lifetime means: two engines may
 * exist one after the other, and the second starts from nothing.
 */

#include <AgoraWebView/AGWebEngine.h>

#include <stdio.h>

int main(void)
{
	int pass;

	for (pass = 0; pass < 2; pass++) {
		AGWebEngine *engine = AGWebEngineCreate(320, 200, NULL, NULL);

		if (engine == NULL) {
			/* A machine with no WPE WebKit is a SKIP, not a failure: this component is the engine's host. */
			printf("teardown: the engine would not start\n");
			return 77;
		}
		/* A turn or two of the loop, so the engine is past its own start-up and has something to tear down. */
		AGWebEngineIterate(engine);
		AGWebEngineIterate(engine);
		AGWebEngineDestroy(engine);
		printf("teardown: engine %d made and destroyed\n", pass + 1);
	}
	printf("teardown: ok — two engines in one process, each destroyed while the process was healthy\n");
	return 0;
}
