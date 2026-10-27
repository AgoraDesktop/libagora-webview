/*
 * webengine_export — the two ways a frame can arrive, and the claim that they are the same picture.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHAT IS ASKED. `AGWebExportEGL` makes WPE hand the host `EGLImage`s; the seam imports each one on an `AGGL` context,
 * blits it into an offscreen target and reads it back. That is three chances to get the picture wrong — an import that
 * samples nothing, a blit without the flip (a producer's frames are top-down and GL's texture row 0 is the top), and a
 * readback with the channels swapped — and every one of them produces a frame that is not the page.
 *
 * **SO THE ASSERTION IS THE SAME ONE THE SHARED-MEMORY TEST MAKES**: a red band at the top of the frame and blue under
 * it. Red-at-the-top is what distinguishes a flipped blit from an upright one and a channel swap from neither; a test
 * that only asked "some pixels arrived" would pass with the page upside down and magenta.
 *
 * A machine whose EGL cannot import a dma-buf FALLS BACK to shared memory (the seam treats the request as a preference),
 * and this test SKIPS (77) rather than claiming a path it did not take — `AGWebEngineExportPath` is what tells it.
 */

#include <AgoraWebView/AGWebEngine.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VIEW_WIDTH 320
#define VIEW_HEIGHT 200

struct App {
	AGWebEngine *engine;
	uint32_t *frame;
	int frame_width;
	int frame_height;
	int finished;
	int damaged;
	int drawn;
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

static int now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
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
	if (!AGWebEngineDraw(app->engine, app->frame, width * 4, width, height)) {
		return 0;
	}
	app->drawn++;
	return 1;
}

static uint32_t pixel(struct App *app, int x, int y)
{
	if (app->frame == NULL || x < 0 || y < 0 || x >= app->frame_width || y >= app->frame_height) {
		return 0xFFFFFFFFu;
	}
	return app->frame[(size_t)y * (size_t)app->frame_width + (size_t)x];
}

int main(void)
{
	static const char *page =
		"<html><body style='margin:0'>"
		"<div style='height:40px;background:#ff0000'></div>"
		"<div style='height:160px;background:#0000c0'></div>"
		"</body></html>";
	static const AGWebEngineCallbacks callbacks = { on_load_changed, NULL, on_damaged };
	struct App app;
	const char *path;
	int waited;
	uint32_t top;
	uint32_t bottom;
	uint32_t middle;

	memset(&app, 0, sizeof app);
	app.engine = AGWebEngineCreateWithExport(VIEW_WIDTH, VIEW_HEIGHT, AGWebExportEGL, &callbacks, &app);
	if (app.engine == NULL) {
		printf("egl: the engine would not start\n");
		return 77;		/* SKIP: no WPE WebKit on this machine */
	}
	path = AGWebEngineExportPath(app.engine);
	printf("egl: the engine's export path is '%s'\n", path);
	if (strcmp(path, "egl") != 0) {
		printf("egl: this machine's EGL cannot import a dma-buf, so the seam fell back — SKIP\n");
		AGWebEngineDestroy(app.engine);
		return 77;
	}

	AGWebEngineLoadHTML(app.engine, page, NULL);
	waited = 0;
	while (!app.finished || app.damaged < 1) {
		AGWebEngineIterate(app.engine);
		nap(2);
		if (++waited > 2500) {
			fprintf(stderr, "egl: the page never loaded\n");
			AGWebEngineDestroy(app.engine);
			return 1;
		}
	}
	/* The page's own frame arrives after the load finishes, and the white pre-load frame is not the page. */
	{
		int started = now_ms();

		while (now_ms() - started < 3000) {
			AGWebEngineIterate(app.engine);
			if (draw(&app) && pixel(&app, 160, 20) == 0x00FF0000u) {
				break;
			}
			nap(2);
		}
	}
	top = pixel(&app, 160, 20);
	middle = pixel(&app, 160, 60);
	bottom = pixel(&app, 160, 150);
	printf("egl: top 0x%06x, middle 0x%06x, bottom 0x%06x (frame %dx%d, %d draw(s))\n", top, middle, bottom,
	       app.frame_width, app.frame_height, app.drawn);
	free(app.frame);
	AGWebEngineDestroy(app.engine);

	if (top != 0x00FF0000u) {
		fprintf(stderr, "egl: the red band is 0x%06x at the top — a flip, a channel swap, or an empty frame\n", top);
		return 1;
	}
	if (middle != 0x000000C0u || bottom != 0x000000C0u) {
		fprintf(stderr, "egl: the body is 0x%06x/0x%06x, want 0x0000c0 — the page is not the whole frame\n", middle,
			bottom);
		return 1;
	}
	printf("egl: ok — the page arrived through the EGL export, upright and in the same channels\n");
	return 0;
}
