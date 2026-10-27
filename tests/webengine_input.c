/*
 * webengine_input — does the page hear the pointer and the keyboard?
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHY EACH EVENT IS ASSERTED BY A COLOUR. "The event was dispatched" is not a fact anyone can check from outside the
 * engine; "the page painted a different colour because of it" is. So the page has a strip that turns green on `:hover`,
 * the same strip that turns yellow when it is clicked, a second strip that turns red when the page receives a key, and a
 * marker 680 points down that can only be seen after a scroll. Each door gets its own colour, and each colour is read
 * out of the ENGINE's frame — the pixels `AGWebEngineDraw` hands a view — so what is measured is what a person would see.
 *
 * **NO COMPOSITOR AND NO WINDOW**: the engine is independent of the window server (that is the point of it being a
 * separate seam), so this probe runs anywhere, and a machine without WebKit SKIPS rather than fails.
 *
 * The page is written to make each assertion unambiguous:
 *   - `:hover` is a CSS hit test, so it fails if the pointer arrives with the wrong coordinates or no motion at all;
 *   - the click is an inline style set by JavaScript, so it also proves the page's script ran;
 *   - the key is a `keydown` listener with no focused field, so it needs nothing but a keyboard event that reaches the
 *     document — which is exactly what a `WebView` with no form control on screen has to do.
 */

#include <AgoraWebView/AGWebEngine.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VIEW_WIDTH 320
#define VIEW_HEIGHT 200

#define COLOUR_HOVER_BASE 0x00002040u
#define COLOUR_HOVER_CSS 0x0000C000u
#define COLOUR_CLICKED 0x00FFFF00u
#define COLOUR_KEY_BASE 0x00400020u
#define COLOUR_KEYED 0x00FF0000u
#define COLOUR_MARKER 0x0000FFFFu

#define XKB_KEY_a 0x0061u
#define EVDEV_KEY_A 38u

#define HOVER_X 160
#define HOVER_Y 20
#define KEY_X 160
#define KEY_Y 60

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

static void on_damaged(void *context, AGWebEngine *engine)
{
	struct App *app = context;

	(void)engine;
	app->damaged++;
}

static int now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void nap(int milliseconds)
{
	struct timespec ts;

	ts.tv_sec = milliseconds / 1000;
	ts.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

/* The newest frame, drawn into this probe's own buffer and sampled — the same call a view makes in `-drawRect:`. */
static uint32_t sample(struct App *app, int x, int y)
{
	int width = 0;
	int height = 0;

	AGWebEngineFrameSize(app->engine, &width, &height);
	if (width != app->frame_width || height != app->frame_height) {
		uint32_t *grown = realloc(app->frame, (size_t)width * (size_t)height * 4u);

		if (grown == NULL) {
			return 0xFFFFFFFFu;
		}
		app->frame = grown;
		app->frame_width = width;
		app->frame_height = height;
	}
	if (x < 0 || y < 0 || x >= width || y >= height) {
		return 0xFFFFFFFFu;
	}
	if (!AGWebEngineDraw(app->engine, app->frame, width * 4, width, height)) {
		return 0xFFFFFFFFu;
	}
	return app->frame[(size_t)y * (size_t)width + (size_t)x];
}

/* **WAIT FOR A COLOUR RATHER THAN FOR A FRAME.** WebKit coalesces repaints: the frame a click produces may also carry the
 * hover state from a moment earlier, and the frame that arrives first after an event can be one the load started. So an
 * assertion polls the sample until it matches, with a budget, and reports the last value it saw when it never does —
 * which turns "the door is broken" into a colour and a number instead of a timeout. */
static int expect_colour(struct App *app, int x, int y, uint32_t want, const char *what, int budget_ms)
{
	int started = now_ms();
	uint32_t got = 0xFFFFFFFFu;

	while (now_ms() - started < budget_ms) {
		AGWebEngineIterate(app->engine);
		got = sample(app, x, y);
		if (got == want) {
			return 1;
		}
		nap(5);
	}
	fprintf(stderr, "input: %s gave 0x%06x at (%d,%d), want 0x%06x\n", what, got, x, y, want);
	return 0;
}

static int has_colour(struct App *app, uint32_t colour)
{
	int width = 0;
	int height = 0;
	int x, y;

	AGWebEngineFrameSize(app->engine, &width, &height);
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			if (sample(app, x, y) == colour) {
				return 1;
			}
		}
	}
	return 0;
}

int main(void)
{
	static const char *page =
		"<html><head><title>input probe</title><style>"
		"  body { margin:0; background:#202020; }"
		"  #hover { height:40px; background:#002040; }"
		"  #hover:hover { background:#00c000; }"
		"  #key { height:40px; background:#400020; }"
		"  #spacer { height:200px; background:#101010; }"
		"  #marker { height:200px; background:#00ffff; }"
			"</style></head><body>"
		"<div id=hover></div><div id=key></div><div id=spacer></div><div id=marker></div>"
		"<div style='height:400px;background:#101010'></div>"
		"<script>"
		"document.getElementById('hover').addEventListener('click', function (e) {"
		"  e.target.style.background = '#ffff00'; });"
		"window.addEventListener('keydown', function () {"
		"  document.getElementById('key').style.background = '#ff0000'; });"
		"</script></body></html>";
	static const AGWebEngineCallbacks callbacks = { on_load_changed, NULL, on_damaged };
	struct App app;
	int baseline;
	int failures = 0;
	int waited;

	setvbuf(stdout, NULL, _IOLBF, 0);
	memset(&app, 0, sizeof app);
	app.engine = AGWebEngineCreate(VIEW_WIDTH, VIEW_HEIGHT, &callbacks, &app);
	if (app.engine == NULL) {
		printf("input: the engine would not start\n");
		return 77;		/* SKIP: a machine with no WPE WebKit is not a failure of this code */
	}
	AGWebEngineLoadHTML(app.engine, page, NULL);

	/* **THE LOAD FINISHES AND THE REAL PAGE IS ON SCREEN BEFORE ANY EVENT IS SENT.** An input event delivered to a blank
	 * document is thrown away, and the frame that arrives first is the blank white one — MEASURED: sampling straight
	 * after the first damage reports 0xffffff, which is neither of the page's colours and would have made every
	 * assertion below fail for a reason that has nothing to do with input. */
	{
		int started = now_ms();

		while (now_ms() - started < 5000) {
			AGWebEngineIterate(app.engine);
			if (app.finished && sample(&app, HOVER_X, HOVER_Y) == COLOUR_HOVER_BASE &&
			    sample(&app, KEY_X, KEY_Y) == COLOUR_KEY_BASE) {
				break;
			}
			nap(2);
		}
		if (!app.finished) {
			fprintf(stderr, "input: the page never loaded\n");
			return 1;
		}
	}
	printf("input: the page loaded and painted\n");

	failures += !expect_colour(&app, HOVER_X, HOVER_Y, COLOUR_HOVER_BASE, "the hover strip before input", 1000);
	failures += !expect_colour(&app, KEY_X, KEY_Y, COLOUR_KEY_BASE, "the key strip before input", 1000);
	if (has_colour(&app, COLOUR_MARKER)) {
		fprintf(stderr, "input: the scroll marker is visible before any scroll\n");
		failures++;
	}

	/* 1. THE POINTER, WHICH IS A CSS HIT TEST. */
	AGWebEnginePointerMotion(app.engine, HOVER_X, HOVER_Y, 0, (uint32_t)now_ms());
	if (expect_colour(&app, HOVER_X, HOVER_Y, COLOUR_HOVER_CSS, "hovering", 3000)) {
		printf("input: hover ok\n");
	} else {
		failures++;
	}

	/* 2. THE BUTTON, WHICH IS ALSO JAVASCRIPT. */
	/* **ONE CLICK TO TAKE THE FOCUS AND ONE TO MEAN IT.** MEASURED: the first press/release pair a page ever receives is
	 * spent taking the focus — the `click` arrives on the second — which is also what a person does to a window they
	 * have not touched yet. Both are sent here so the probe asks what a view in an untouched window has to ask. */
	{
		int round;

		for (round = 0; round < 2; round++) {
			AGWebEnginePointerButton(app.engine, HOVER_X, HOVER_Y, 0x110u, 1, 0, (uint32_t)now_ms());
			AGWebEngineIterate(app.engine);
			nap(60);
			AGWebEnginePointerButton(app.engine, HOVER_X, HOVER_Y, 0x110u, 0, 0, (uint32_t)now_ms());
			AGWebEngineIterate(app.engine);
			nap(60);
		}
	}
	if (expect_colour(&app, HOVER_X, HOVER_Y, COLOUR_CLICKED, "the click", 3000)) {
		printf("input: click ok\n");
	} else {
		failures++;
	}

	/* 3. THE KEYBOARD. */
	AGWebEngineKey(app.engine, XKB_KEY_a, EVDEV_KEY_A, 0, 1, (uint32_t)now_ms());
	if (expect_colour(&app, KEY_X, KEY_Y, COLOUR_KEYED, "the key", 3000)) {
		printf("input: key ok\n");
	} else {
		failures++;
	}
	AGWebEngineKey(app.engine, XKB_KEY_a, EVDEV_KEY_A, 0, 0, (uint32_t)now_ms());

	/* 4. THE WHEEL. **THE MARKER IS THE ASSERTION, NOT A BAND OF COLOUR**: it starts 200 points down — immediately
	 * below the fold — so three gestures of 60 bring it into view, and a scroll that is delivered but not applied leaves
	 * it out of sight. MEASURED the hard way: the first version of this check sampled the middle of a 100-point band and
	 * "proved" that scrolling did nothing for an hour, while the document had been scrolling all along. */
	{
		int notch;
		int seen = 0;
		int started;

		for (notch = 0; notch < 4; notch++) {
			AGWebEngineScroll(app.engine, HOVER_X, HOVER_Y, 0 /* AGAxisVertical */, 60.0, 0, 0,
					  (uint32_t)now_ms());
			AGWebEngineIterate(app.engine);
			nap(200);
		}
		/* **WHERE THE MARKER IS DOES NOT MATTER; THAT IT IS VISIBLE DOES.** WebKit's scroll is smooth, so the exact
		 * distance four gestures produce depends on how they overlap — MEASURED, and the reason this assertion is
		 * "the band is on screen at all" rather than "the band is at y = 30": a first attempt with a 24-point mark gave
		 * two false negatives, one because the document could not scroll far enough to show it and one because the
		 * scroll went past it. A band that is on screen for a 200-point range asks the question that matters. */
		started = now_ms();
		while (now_ms() - started < 4000) {
			AGWebEngineIterate(app.engine);
			if (has_colour(&app, COLOUR_MARKER)) {
				seen = 1;
				break;
			}
			nap(5);
		}
		if (seen) {
			printf("input: scroll ok\n");
		} else {
			fprintf(stderr, "input: the scroll marker never came into view\n");
			failures++;
		}
	}

	free(app.frame);
	AGWebEngineDestroy(app.engine);
	if (failures != 0) {
		fprintf(stderr, "input: %d check(s) failed\n", failures);
		return 1;
	}
	printf("input: ok — the page heard the pointer, the button, the keys and the wheel\n");
	return 0;
}
