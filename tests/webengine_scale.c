/*
 * webengine_scale — the two numbers a page is laid out with: how many points there are, and how many pixels a point owns.
 *
 * Copyright (C) 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PART ONE: THE DEVICE SCALE FACTOR. A 400x300 engine at factor 2 must hand back an 800x600 frame whose content is laid out
 * for 400 CSS pixels — a 40-POINT band is 80 device rows. That is the difference between rendering at the output's
 * resolution and rendering at 1x and resampling it up, which is what this component did until the factor was passed
 * through, and the difference is visible as softness in every glyph.
 *
 * PART TWO: A RESIZE WHILE THE PAGE IS LOADING. The page's band has a FIXED height in points, so its height in device rows
 * at the end says which viewport the page was laid out for: 40 rows at 320x200 is a page laid out for 320x200, and ~17
 * rows would be a 640x480 page's 40 rows rescaled — i.e. a stale layout stretched, which is the bug this asserts against.
 */

#include <AgoraWebView/AGWebEngine.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BAND_POINTS 40

struct App {
	AGWebEngine *engine;
	uint32_t *frame;
	int frame_width;
	int frame_height;
	int finished;
	int damaged;
};

static void on_load_changed(void *context, AGWebEngine *engine, AGWebLoadStage stage)
{
	struct App *app = context;

	(void)engine;
	if (stage == AGWebLoadFinished) {
		app->finished = 1;
	}
}

static void on_damaged(void *context, AGWebEngine *engine, int x, int y, int width, int height)
{
	struct App *app = context;

	(void)engine;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	app->damaged++;
}

static void nap(int milliseconds)
{
	struct timespec ts;

	ts.tv_sec = milliseconds / 1000;
	ts.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

static int draw(struct App *app)
{
	int width = 0;
	int height = 0;

	AGWebEngineFrameSize(app->engine, &width, &height);
	if (width <= 0 || height <= 0) {
		return 0;
	}
	if (width != app->frame_width || height != app->frame_height) {
		uint32_t *grown = realloc(app->frame, (size_t)width * (size_t)height * 4u);

		if (grown == NULL) {
			return 0;
		}
		app->frame = grown;
		app->frame_width = width;
		app->frame_height = height;
	}
	return AGWebEngineDraw(app->engine, app->frame, width * 4, width, height);
}

/* **HOW MANY DEVICE ROWS OF THE BAND THERE ARE**, counted down one column: the page has a red band on top and blue under
 * it, so the answer is the band's height in the frame the engine produced. */
static int band_rows(struct App *app, int x)
{
	int y = 0;

	while (y < app->frame_height) {
		uint32_t pixel = app->frame[(size_t)y * (size_t)app->frame_width + (size_t)x];

		if (pixel != 0x00FF0000u) {
			break;
		}
		y++;
	}
	return y;
}

/* **WAIT FOR THE BAND, NOT FOR A FRAME COUNT.** WebKit repaints on its own schedule and the frame that arrives first after
 * a load finishes can still be the blank page; a test that sampled once would call a working engine broken — which is the
 * mistake this file made first, twice. So the wait is for the thing being asserted: a row of the page's own colour. */
static int wait_for_band(struct App *app, int x, int budget_ms)
{
	struct timespec ts;
	int started;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	started = (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	while ((int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000) - started < budget_ms) {
		AGWebEngineIterate(app->engine);
		if (draw(app) && band_rows(app, x) > 0) {
			return 1;
		}
		nap(2);
		clock_gettime(CLOCK_MONOTONIC, &ts);
	}
	return 0;
}

static int pump_until_loaded(struct App *app, int budget_ms)
{
	struct timespec ts;
	int started;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	started = (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	while (!app->finished) {
		AGWebEngineIterate(app->engine);
		nap(2);
		if ((int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000) - started > budget_ms) {
			return 0;
		}
		clock_gettime(CLOCK_MONOTONIC, &ts);
	}
	/* And a moment more, because the page's own frame arrives after the load finishes. */
	{
		int frames = app->damaged;

		while (app->damaged == frames) {
			AGWebEngineIterate(app->engine);
			nap(2);
			if ((int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000) - started > budget_ms) {
				return 0;
			}
			clock_gettime(CLOCK_MONOTONIC, &ts);
		}
	}
	return 1;
}

static const char *page =
	"<html><head><style>"
	"  body { margin:0; }"
	"  #band { height:40px; background:#ff0000; }"
	"  #body { height:2000px; background:#0000c0; }"
	"</style></head><body><div id=band></div><div id=body></div></body></html>";

int main(void)
{
	static const AGWebEngineCallbacks callbacks = { on_load_changed, NULL, on_damaged };
	struct App app;
	int failed = 0;
	const char *diag = getenv("AG_WEBENGINE_SCALE_DIAG");

	/* ============================ PART ONE: THE DEVICE SCALE FACTOR ============================ */
	memset(&app, 0, sizeof app);
	app.engine = AGWebEngineCreate(400, 300, &callbacks, &app);
	if (app.engine == NULL) {
		printf("scale: the engine would not start\n");
		return 77;		/* SKIP: no WPE WebKit on this machine */
	}
	AGWebEngineSetDeviceScaleFactor(app.engine, 2.0);
	AGWebEngineLoadHTML(app.engine, page, NULL);
	if (!pump_until_loaded(&app, 8000) || !wait_for_band(&app, 200, 8000)) {
		fprintf(stderr, "scale: the page never painted the band at factor 2 (frame %dx%d)\n", app.frame_width,
			app.frame_height);
		return 1;
	}
	printf("scale: factor 2 -> frame %dx%d, band %d row(s)\n", app.frame_width, app.frame_height,
	       band_rows(&app, 200));
	if (diag != NULL) {
		int row;

		for (row = 0; row < 6; row++) {
			printf("  DIAG row %d: 0x%06x 0x%06x\n", row * 20,
			       app.frame[(size_t)(row * 20) * app.frame_width + 200],
			       app.frame[(size_t)(row * 20) * app.frame_width + 10]);
		}
	}
	if (app.frame_width != 800 || app.frame_height != 600) {
		fprintf(stderr, "scale: the frame is %dx%d at factor 2, want 800x600 — points x factor\n",
			app.frame_width, app.frame_height);
		failed++;
	}
	if (band_rows(&app, 200) != BAND_POINTS * 2) {
		fprintf(stderr, "scale: a %d-point band is %d device rows at factor 2, want %d\n", BAND_POINTS,
			band_rows(&app, 200), BAND_POINTS * 2);
		failed++;
	}
	free(app.frame);
	AGWebEngineDestroy(app.engine);

	/* ============================ PART TWO: A RESIZE WHILE LOADING ============================ */
	memset(&app, 0, sizeof app);
	app.engine = AGWebEngineCreate(640, 480, &callbacks, &app);
	if (app.engine == NULL) {
		return 77;
	}
	AGWebEngineLoadHTML(app.engine, page, NULL);
	/* **THE RESIZE HAPPENS WHILE THE PAGE IS STILL LOADING, WHICH IS THE CASE THAT HAS NO GATE.** A few turns in, the
	 * viewport changes twice and settles at 320x200; the load finishes afterwards. */
	{
		int turn;

		for (turn = 0; turn < 6; turn++) {
			AGWebEngineIterate(app.engine);
			nap(2);
		}
		AGWebEngineSetSize(app.engine, 480, 360);
		for (turn = 0; turn < 4; turn++) {
			AGWebEngineIterate(app.engine);
			nap(2);
		}
		AGWebEngineSetSize(app.engine, 320, 200);
	}
	if (!pump_until_loaded(&app, 8000) || !wait_for_band(&app, 160, 8000)) {
		fprintf(stderr, "scale: the page never painted the band after being resized while it loaded "
				"(frame %dx%d)\n", app.frame_width, app.frame_height);
		return 1;
	}
	printf("scale: resized while loading -> frame %dx%d, band %d row(s)\n", app.frame_width, app.frame_height,
	       band_rows(&app, 160));
	if (app.frame_width != 320 || app.frame_height != 200) {
		fprintf(stderr, "scale: the frame is %dx%d after resizing to 320x200\n", app.frame_width,
			app.frame_height);
		failed++;
	}
	/* **THE BAND'S HEIGHT IS THE PROOF OF RELAYOUT.** It is 40 POINTS tall whatever the viewport; a page laid out for
	 * 640x480 and merely resampled into 320x200 would show its 40 rows as about 17. */
	if (band_rows(&app, 160) != BAND_POINTS) {
		fprintf(stderr, "scale: the band is %d rows after resizing to 320x200 — %d means the page was laid out for\n"
				"      the old viewport and stretched, which is what a resize that only resizes the buffer looks like\n",
			band_rows(&app, 160), band_rows(&app, 160));
		failed++;
	}
	free(app.frame);
	AGWebEngineDestroy(app.engine);

	if (failed != 0) {
		fprintf(stderr, "scale: %d check(s) failed\n", failed);
		return 1;
	}
	printf("scale: ok — the factor is points-to-pixels, and a resize relays the page out\n");
	return 0;
}
