/*
 * webengine_probe — a real page, through the seam, into a real window.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHAT IS BEING ASKED. That a page loaded through `AGWebEngine` reaches a window as pixels: the probe loads HTML — a
 * red strip across the top, a blue body under it — draws every damaged frame into its window, and the runner asserts
 * the server's screenshot. Two colours in two places is the smallest picture that fails if the engine is flipped, if
 * the buffer is mis-strided, or if the seam is drawing the blank frame the engine produces before the page arrives.
 *
 * **THE ORDER OF THE LOAD IS PART OF WHAT IS MEASURED.** WebKit exports a blank white frame before the page is
 * committed, so a probe that stopped at the first frame would photograph white and pass a check that only asked for
 * "some pixels". This one records the load stages it saw, requires a FINISHED load, and then SAMPLES the engine's own
 * frame for the page's two colours — red band above, blue body below.
 *
 * The engine's loop is pumped here, which is the whole of what an application has to do: `AGWebEngineIterate` and
 * `AGWindowServerDispatch` are both non-blocking, and one turn of each is one turn of the desktop.
 */

#include <AgoraWebView/AGWebEngine.h>
#include <AppKit/platform/AGWindowServer.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_WIDTH 320
#define WINDOW_HEIGHT 200

#define PAGE_RED 0x00C00000u
#define PAGE_BLUE 0x000000C0u
#define WINDOW_TOP 32

struct App {
	AGWindowServer *server;
	AGWindow *window;
	AGWebEngine *engine;
	int finished;
	int drawn;
	int damaged;
	int stages;
	char title[128];
};

static void on_load_changed(void *context, AGWebEngine *engine, AGWebLoadStage stage)
{
	struct App *app = context;

	(void)engine;
	app->stages++;
	if (stage == AGWebLoadFinished) {
		app->finished = 1;
	}
	printf("webengine: load stage %d\n", (int)stage);
}

static void on_title_changed(void *context, AGWebEngine *engine, const char *title)
{
	struct App *app = context;

	(void)engine;
	snprintf(app->title, sizeof app->title, "%s", title != NULL ? title : "");
	printf("webengine: title '%s'\n", app->title);
}

/* **A DAMAGED FRAME IS DRAWN IMMEDIATELY, WHICH IS WHAT A VIEW WOULD DO IN `-display`.** The engine's pixels go into the
 * window's own buffer through the seam, so this probe and an NSView take the same path: `AGWindowBeginFrame…`,
 * `AGWebEngineDraw`, damage, end. */
static void on_damaged(void *context, AGWebEngine *engine, int x, int y, int damage_width, int damage_height)
{
	struct App *app = context;
	int stride = 0;
	int width = 0;
	int height = 0;
	uint32_t *pixels = AGWindowBeginFrameSized(app->window, &stride, &width, &height);

	(void)x;
	(void)y;
	(void)damage_width;
	(void)damage_height;

	app->damaged++;
	if (pixels == NULL) {
		return;			/* the compositor still holds the buffer; the engine keeps its newest frame */
	}
	if (AGWebEngineDraw(engine, pixels, stride, width, height)) {
		AGWindowDamage(app->window, 0, 0, width, height);
		app->drawn++;
	}
	AGWindowEndFrame(app->window);
}

static void on_event(void *context, const AGEvent *event)
{
	(void)context;
	(void)event;
}

/* **THE ENGINE'S OWN FRAME, ASKED FOR THE WAY A VIEW ASKS FOR IT.** `AGWebEngineDraw` is the same call `-drawRect:` makes,
 * so a colour found here is a colour the window would show; the runner's screenshot then proves the last hop separately. */
static int frame_has_colour(struct App *app, uint32_t rgb)
{
	uint32_t *pixels;
	int width = 0;
	int height = 0;
	int found = 0;
	size_t i;
	size_t count;

	AGWebEngineFrameSize(app->engine, &width, &height);
	if (width <= 0 || height <= 0) {
		return 0;
	}
	pixels = malloc((size_t)width * (size_t)height * 4u);
	if (pixels == NULL) {
		return 0;
	}
	AGWebEngineDraw(app->engine, pixels, width * 4, width, height);
	count = (size_t)width * (size_t)height;
	for (i = 0; i < count; i++) {
		if ((pixels[i] & 0x00FFFFFFu) == rgb) {
			found = 1;
			break;
		}
	}
	free(pixels);
	return found;
}

int main(void)
{
	static const AGWebEngineCallbacks callbacks = {
		on_load_changed,
		on_title_changed,
		on_damaged,
	};
	struct App app;
	int verdict;

	setvbuf(stdout, NULL, _IOLBF, 0);
	memset(&app, 0, sizeof app);

	app.server = AGWindowServerConnect();
	if (app.server == NULL) {
		fprintf(stderr, "webengine: no window server: %s\n", AGWindowServerLastError(NULL));
		return 1;
	}
	app.window = AGWindowCreate(app.server, WINDOW_WIDTH, WINDOW_HEIGHT, "Agora web engine");
	if (app.window == NULL) {
		fprintf(stderr, "webengine: could not make a window: %s\n", AGWindowServerLastError(app.server));
		AGWindowServerDisconnect(app.server);
		return 1;
	}
	app.engine = AGWebEngineCreate(WINDOW_WIDTH, WINDOW_HEIGHT, &callbacks, &app);
	if (app.engine == NULL) {
		fprintf(stderr, "webengine: the engine would not start\n");
		AGWindowServerDisconnect(app.server);
		return 77;		/* SKIP: a machine with no WPE WebKit is not a failure of this code */
	}
	AGWindowSetEventHandler(app.window, on_event, &app);
	AGWindowSetFrame(app.window, 0, WINDOW_TOP, WINDOW_WIDTH, WINDOW_HEIGHT);
	AGWindowShow(app.window);

	AGWebEngineLoadHTML(app.engine,
		"<html><head><title>Agora web engine</title></head>"
		"<body style='margin:0;background:#0000c0;color:#ffffff'>"
		"<div style='height:40px;background:#c00000'></div>"
		"<div style='padding:24px;font:24px sans-serif'>Hello from WPE WebKit</div>"
		"</body></html>", NULL);

	/* **THE RUN ENDS WHEN THE COMPOSITOR GOES AWAY, AND THE VERDICT IS PRINTED AS SOON AS IT IS EARNED.** The runner
	 * watches this output to know when the page has been drawn, and only then stops the server to make it write its
	 * screenshot — so a client that exited first would take its window with it and photograph the desktop. MEASURED as
	 * exactly that: a probe with a turn budget of its own finished before the runner acted, and the screenshot was an
	 * empty desktop while the probe's own checks passed. */
	verdict = 0;
	while (AGWindowServerDispatchTimeout(app.server, 5) >= 0) {
		AGWebEngineIterate(app.engine);
		/* **THE VERDICT IS THE PICTURE, NOT A FRAME COUNT.** This used to require TWO frames, on the reasoning that the first
	 * one is the blank page WebKit exports before the content commits. That was true of a SLOW start and false of a fast
	 * one: MEASURED after the vblank stall was fixed (the page now commits in ~270 ms instead of 6.5 s), the first frame
	 * the engine exports is already the finished page, nothing changes afterwards, so the damage callback fires once and a
	 * two-frame rule fails a probe whose screenshot is perfect. What matters is what the frame CONTAINS, so the colours
	 * are sampled instead — which is also a stronger claim than "a frame happened". */
	if (!verdict && app.finished && frame_has_colour(&app, PAGE_RED) && frame_has_colour(&app, PAGE_BLUE)) {
			verdict = 1;
			printf("webengine: ok — the page was drawn\n");
		}
	}
	/* **THE ENGINE IS DESTROYED AFTER THE COMPOSITOR HAS GONE, AND THAT IS WORTH ASKING OF THIS SEAM EVERY TIME.** An
	 * application's views are torn down on `-terminate:` with the session still up, so this order is the harder one; it
	 * is here so that a regression in the engine's teardown is caught where a real session would hit it. */
	AGWebEngineDestroy(app.engine);
	AGWindowServerDisconnect(app.server);

	if (!app.finished) {
		fprintf(stderr, "webengine: the page never finished loading\n");
		return 1;
	}
	if (!verdict) {
		fprintf(stderr, "webengine: the engine's frame never became the page (%d frame(s) drawn)\n", app.drawn);
		return 1;
	}
	return 0;
}
