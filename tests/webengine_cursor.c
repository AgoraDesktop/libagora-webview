/*
 * Copyright (C) 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE POINTER OVER A PAGE IS THE PAGE'S DECISION, AND THIS ASKS WHAT IT DECIDED.** A hand over a link, an I-beam over
 * text, a crosshair on a `cursor: crosshair` div, a "no" sign on a control that refuses a drop, nothing at all where a page
 * draws its own pointer: all five are in this one page, one horizontal band each, and the probe walks the pointer down it
 * and reads `AGWebEngineCursor` at every stop.
 *
 * WHY IT MATTERS THAT THIS IS THE PAGE'S ANSWER AND NOT A HIT TEST: `WebKitHitTestResult` says "link", "text", "image" —
 * it cannot say that this `<div>` asked for a crosshair, that this form control asked for `not-allowed`, or that this
 * video asked for no cursor. Those are exactly the cursors a person notices missing.
 *
 * **IT SKIPS ON A WEBKIT WITHOUT THE DOOR.** The cursor is reported by an API this project's own WebKit fork adds
 * (`webkit_web_view_get_cursor_type`, see `~/Development/wpewebkit`); the distro's 2.48 has no such function, and then
 * `AGWebEngineCursorSupported` is false and this probe SKIPS (77) instead of failing — the component still builds and runs
 * there, it just keeps its own pointer.
 */
#include <AgoraWebView/AGWebEngine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VIEW_WIDTH 640
#define VIEW_HEIGHT 240
#define BAND_HEIGHT 40

#define LINK_Y 20
#define TEXT_Y 60
#define CROSS_Y 100
#define NOT_ALLOWED_Y 140
#define NONE_Y 180

static int finished;
static int failed;

static void on_load_changed(void *context, AGWebEngine *engine, AGWebLoadStage stage)
{
	(void)context;
	(void)engine;
	if (stage == AGWebLoadFinished) {
		finished = 1;
	}
}

static void nap(int milliseconds)
{
	struct timespec ts = { milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* **A CURSOR IS A CONSEQUENCE OF A MOVE, SO IT IS WAITED FOR, NOT READ.** WebKit computes it from the hit test the move
 * produced, and that crosses to the web process; MEASURED, reading it in the same turn as the move reports the PREVIOUS
 * band every time — which would look exactly like a broken mapping. */
static AGWebCursor cursor_at(AGWebEngine *engine, int x, int y)
{
	int elapsed;

	AGWebEnginePointerMotion(engine, x, y, 0, 0);
	/* **A FIXED SETTLE, NOT A WAIT FOR "SOMETHING".** The first version waited only until the value stopped being
	 * `AGWebCursorInvalid`, which is true the moment ANY band has been hovered — so every band after the first read the
	 * FIRST band's cursor and four checks failed with "hand" over text. The move has to cross to the web process and come
	 * back, so the probe pumps for a fixed 150 ms and then reads. */
	for (elapsed = 0; elapsed < 150; elapsed += 5) {
		int i;

		for (i = 0; i < 5; i++) {
			AGWebEngineIterate(engine);
			nap(1);
		}
	}
	return AGWebEngineCursor(engine);
}

static void expect(AGWebEngine *engine, int y, AGWebCursor want, const char *what)
{
	AGWebCursor got = cursor_at(engine, 40, y);

	if (got == want) {
		printf("cursor: %-28s -> %d, as asked\n", what, (int)got);
	} else {
		fprintf(stderr, "cursor: %-28s -> %d, want %d\n", what, (int)got, (int)want);
		failed++;
	}
}

int main(void)
{
	static const AGWebEngineCallbacks callbacks = { on_load_changed, NULL, NULL };
	static const char *page =
		"<html><head><title>cursors</title><style>"
		"  body { margin:0; font:16px sans-serif; }"
		"  .band { height:40px; padding:8px 12px; box-sizing:border-box; }"
		"  #cross { cursor:crosshair; background:#e0e0e0; }"
		"  #nope { cursor:not-allowed; background:#d0d0d0; }"
		"  #gone { cursor:none; background:#c0c0c0; }"
		"</style></head><body>"
		"<div class=band><a href=\"#x\">a link, which asks for a hand</a></div>"
		"<div class=band>plain selectable text, which asks for an I-beam</div>"
		"<div class=band id=cross>cursor: crosshair</div>"
		"<div class=band id=nope>cursor: not-allowed</div>"
		"<div class=band id=gone>cursor: none</div>"
		"</body></html>";
	AGWebEngine *engine;
	int waited;

	setvbuf(stdout, NULL, _IOLBF, 0);
	engine = AGWebEngineCreate(VIEW_WIDTH, VIEW_HEIGHT, &callbacks, NULL);
	if (engine == NULL) {
		printf("cursor: the engine would not start\n");
		return 77;		/* SKIP: no WPE WebKit on this machine */
	}
	if (!AGWebEngineCursorSupported(engine)) {
		printf("cursor: this WebKit does not report the page's cursor (built without the Agora fork's door)\n");
		AGWebEngineDestroy(engine);
		return 77;		/* SKIP: the distro library has no such API */
	}
	AGWebEngineLoadHTML(engine, page, NULL);
	for (waited = 0; waited < 5000 && !finished; waited++) {
		AGWebEngineIterate(engine);
		nap(1);
	}
	if (!finished) {
		fprintf(stderr, "cursor: the page never loaded\n");
		AGWebEngineDestroy(engine);
		return 1;
	}

	/* ONE BAND EACH, IN THE ORDER THE PAGE LAYS THEM OUT. The y values are the middle of each 40-point band. */
	expect(engine, LINK_Y, AGWebCursorHand, "a link");
	expect(engine, TEXT_Y, AGWebCursorIBeam, "selectable text");
	expect(engine, CROSS_Y, AGWebCursorCrosshair, "cursor: crosshair");
	expect(engine, NOT_ALLOWED_Y, AGWebCursorNotAllowed, "cursor: not-allowed");
	expect(engine, NONE_Y, AGWebCursorNone, "cursor: none");

	AGWebEngineDestroy(engine);
	if (failed != 0) {
		fprintf(stderr, "cursor: %d check(s) failed\n", failed);
		return 1;
	}
	printf("cursor: ok — the page's cursor reaches the embedder\n");
	return 0;
}
