/*
 * Copyright (C) 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE FIRST LOAD TOOK 6.5 SECONDS, AND IT WAS A VBLANK WAIT ON A DISPLAY THIS PROCESS DOES NOT OWN.** This probe exists
 * because that number was invisible: everything "worked", the page appeared, and only a stopwatch said something was
 * wrong. It loads a page and asserts the load's FIRST stage arrives quickly, so a return of the stall fails a test
 * instead of turning up as "the browser feels slow".
 *
 * THE MEASUREMENT, on this machine, nested on X11, with WPE WebKit 2.54.2 built locally:
 *
 *   before   load started 6418 ms, commit 6421 ms — for an EMPTY page and for a `file://` page alike
 *   after    load started 256 ms, commit 273 ms
 *
 * The cause was named by a `gdb` backtrace of the UI process while it waited: `WebPageProxy::loadRequest` →
 * `initializeWebPage` → `creationParameters` → `DisplayLink::platformInitialize` → `DisplayVBlankMonitorDRM::create`,
 * whose last act is `drmWaitVBlank()` on a CRTC the X server owns. The engine sets `WEBKIT_FORCE_VBLANK_TIMER=1`
 * (`ag_web_want_vblank_timer` in AGWebEngine-WPE.c) and the wait goes away.
 *
 * IT CAN BE MADE TO FAIL ON PURPOSE, which is how it was checked: `WEBKIT_FORCE_VBLANK_TIMER=0` in the environment is
 * respected by the engine (it only sets an unset variable), so running this probe with that variable restores the DRM path
 * and the assertion below fails with the ~6400 ms figure.
 */
#include <AgoraWebView/AGWebEngine.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BOUND_MS 1500

static double started_ms = -1.0;
static double t0;

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void on_load_changed(void *context, AGWebEngine *engine, AGWebLoadStage stage)
{
	(void)context;
	(void)engine;
	if (stage == AGWebLoadStarted && started_ms < 0.0) {
		started_ms = now_ms() - t0;
	}
}

static void nap(int milliseconds)
{
	struct timespec ts = { milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

int main(void)
{
	static const AGWebEngineCallbacks callbacks = { on_load_changed, NULL, NULL };
	static const char *page =
		"<html><head><title>startup</title></head><body><p>one paragraph, nothing else</p></body></html>";
	AGWebEngine *engine;
	int waited;

	setvbuf(stdout, NULL, _IOLBF, 0);
	engine = AGWebEngineCreate(800, 600, &callbacks, NULL);
	if (engine == NULL) {
		printf("startup: the engine would not start\n");
		return 77;		/* SKIP: a machine with no WPE WebKit is not a failure of this code */
	}
	AGWebEngineIterate(engine);
	t0 = now_ms();
	AGWebEngineLoadHTML(engine, page, NULL);
	for (waited = 0; waited < 30000 && started_ms < 0.0; waited++) {
		AGWebEngineIterate(engine);
		nap(1);
	}
	if (started_ms < 0.0) {
		fprintf(stderr, "startup: the load never started within 30 s\n");
		AGWebEngineDestroy(engine);
		return 1;
	}
	printf("startup: the first load started %.0f ms after it was asked for (bound %d ms)\n", started_ms, BOUND_MS);
	AGWebEngineDestroy(engine);
	if (started_ms > BOUND_MS) {
		fprintf(stderr, "startup: %.0f ms is the DRM vblank stall — is WEBKIT_FORCE_VBLANK_TIMER set to something "
			"other than \"\" and \"0\"?\n", started_ms);
		return 1;
	}
	printf("startup: ok — no vblank stall\n");
	return 0;
}
