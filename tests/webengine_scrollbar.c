/*
 * webengine_scrollbar — the page's scrollbars, drawn the way the AppKit's own scroller is.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **THE REPORT THIS FILE ANSWERS IS "the page's bars are invisible until you touch the wheel".** The port draws them with
 * WebKit's Adwaita theme, and on WPE they are OVERLAY bars (`ScrollbarThemeAdwaita::usesOverlayScrollbars()` is
 * `SystemSettings::overlayScrolling().value_or(true)`, with no environment override on this port), so a page at rest has
 * no bar at all and a reader has no idea how far through it they are. The engine installs a user style sheet — the
 * public `webkit_web_view_get_user_content_manager` and `webkit_user_style_sheet_new` — that redraws them as the AppKit's
 * `NSScroller`: fifteen points wide, a #d9d9d9 groove, a white knob with a #bcbcbc edge and arrow buttons carrying black
 * triangles.
 *
 * **TWO THINGS ARE ASSERTED, AND THE SECOND ONE IS THE SURPRISE.** First the PIXELS: the bar is there without scrolling,
 * its groove is the AppKit's grey and its knob's edge is its border grey. Second the LAYOUT: styling `::-webkit-scrollbar`
 * makes WebKit treat the bar as `usesLegacyScrollbarStyle` (`LocalFrameView.cpp:1677`), so it takes its fifteen points out
 * of the viewport instead of floating over the page — which is what an `NSScroller` does in a scroll view, and the reason
 * the page's own colour now stops short of the frame's edge.
 *
 * **AND THE ESCAPE HATCH IS MEASURED TOO.** `AGWEBVIEW_NO_SCROLLBAR_STYLE` is read when the engine is made, so this probe
 * builds one engine without it (the bars are ours) and one with it (the bars are WebKit's overlays, i.e. absent), and
 * asserts the difference — otherwise "we restyled the bars" and "we drew a bar ourselves somewhere" would look alike.
 *
 *   webengine_scrollbar        (no arguments; SKIPS when the engine cannot be made)
 */

#include <AgoraWebView/AGWebEngine.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VIEW_WIDTH 320
#define VIEW_HEIGHT 200

/* THE PAGE: one colour to fill the viewport and enough height to need a bar. The colour is the assertion's subject — the
 * place it STOPS is where the scrollbar starts. */
#define PAGE_COLOUR 0x0000C0C0u
static const char *page =
	"<html><head><title>scrollbar probe</title><style>"
	"  html, body { margin:0; padding:0; background:#00c0c0; }"
	"  #tail { height:1800px; }"
	"</style></head><body><div id=tail></div></body></html>";

/* THE APPKIT'S OWN NUMBERS, spelled here on purpose: this file is the one place that can catch the engine's copy of them
 * drifting from `libagora-app`'s art. #d9d9d9 is `+cg_scrollerTrackColor` (0.85), #bcbcbc is `+controlDarkShadowColor`
 * (0.737), and the bar is `+[NSScroller scrollerWidth]` points. */
#define BAR_WIDTH 15
#define TRACK_GREY 217u
#define EDGE_GREY 188u

struct App {
	AGWebEngine *engine;
	uint32_t *frame;
	int frame_width;
	int frame_height;
	int finished;
};

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

static void on_load_changed(void *context, AGWebEngine *engine, AGWebLoadStage stage)
{
	struct App *app = context;

	(void)engine;
	if (stage == AGWebLoadFinished) {
		app->finished = 1;
	}
}

/* A FRAME OF THE ENGINE'S OWN PIXELS, in this probe's buffer — the same call a view's `-drawRect:` makes. */
static int draw(struct App *app)
{
	int width = 0;
	int height = 0;

	AGWebEngineFrameSize(app->engine, &width, &height);
	if (width < 1 || height < 1) {
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

static uint32_t at(struct App *app, int x, int y)
{
	if (x < 0 || y < 0 || x >= app->frame_width || y >= app->frame_height) {
		return 0xFFFFFFFFu;
	}
	return app->frame[(size_t)y * (size_t)app->frame_width + (size_t)x];
}

static int grey_is(uint32_t pixel, unsigned value)
{
	return ((pixel >> 16) & 0xFFu) == value && ((pixel >> 8) & 0xFFu) == value && (pixel & 0xFFu) == value;
}

/* RUN THE ENGINE UNTIL THE PAGE HAS PAINTED, then draw once. */
static int settle(struct App *app, int budget_ms)
{
	int started = now_ms();

	while (now_ms() - started < budget_ms) {
		AGWebEngineIterate(app->engine);
		if (draw(app) && at(app, app->frame_width / 2, 20) == PAGE_COLOUR) {
			return 1;
		}
		nap(5);
	}
	return 0;
}

/* **PUMP WITHOUT TOUCHING ANYTHING, WHICH IS HOW A FADE-OUT HAPPENS.** WebKit's overlay bars are drawn when a scroll
 * happens and then fade; nothing but time and an iterating loop takes them away, and the fade is the comparison this probe
 * needs against a bar that is part of the layout. */
static void pump_for(struct App *app, int milliseconds)
{
	int started = now_ms();

	while (now_ms() - started < milliseconds) {
		AGWebEngineIterate(app->engine);
		nap(10);
	}
	draw(app);
}

static int load_page(struct App *app, const char *html)
{
	AGWebEngineCallbacks callbacks;

	memset(&callbacks, 0, sizeof callbacks);
	callbacks.load_changed = on_load_changed;
	app->engine = AGWebEngineCreate(VIEW_WIDTH, VIEW_HEIGHT, &callbacks, app);
	if (app->engine == NULL) {
		return 0;
	}
	AGWebEngineLoadHTML(app->engine, html, NULL);
	return settle(app, 8000);
}

/* THE COLUMN WHERE A BAR WOULD BE: the middle of the rightmost BAR_WIDTH columns, sampled down the whole frame. The
 * report is a histogram because a bar is made of FOUR colours (track, knob, edge, arrow ink) and a page that fills the
 * frame is made of one; "how many" is the question, and a named colour is the evidence when it is wrong. */
#define HISTOGRAM_MAX 8
struct Histogram {
	uint32_t colour[HISTOGRAM_MAX];
	int count[HISTOGRAM_MAX];
	int used;
	int total;
};

static void histogram_row(struct App *app, struct Histogram *h)
{
	int y = app->frame_height - BAR_WIDTH / 2;
	int x;
	int i;

	memset(h, 0, sizeof *h);
	for (x = 0; x < app->frame_width; x++) {
		uint32_t p = at(app, x, y);

		h->total++;
		for (i = 0; i < h->used; i++) {
			if (h->colour[i] == p) {
				h->count[i]++;
				break;
			}
		}
		if (i == h->used && h->used < HISTOGRAM_MAX) {
			h->colour[h->used] = p;
			h->count[h->used] = 1;
			h->used++;
		}
	}
}

static void histogram_column(struct App *app, struct Histogram *h)
{
	int x = app->frame_width - BAR_WIDTH / 2;
	int y;
	int i;

	memset(h, 0, sizeof *h);
	for (y = 0; y < app->frame_height; y++) {
		uint32_t p = at(app, x, y);

		h->total++;
		for (i = 0; i < h->used; i++) {
			if (h->colour[i] == p) {
				h->count[i]++;
				break;
			}
		}
		if (i == h->used && h->used < HISTOGRAM_MAX) {
			h->colour[h->used] = p;
			h->count[h->used] = 1;
			h->used++;
		}
	}
}

/* EVERY COLOUR OUR BAR IS MADE OF IS AN EQUAL-CHANNEL GREY: the groove, the knob's border and its edge are greys, and the
 * white knob and black arrows are too. WebKit's overlay bar is a TINT OF THE PAGE — its thumb is the page's own colour
 * darkened — so a column holding no grey at all is a column with no bar of ours in it. */
static int histogram_has_grey(const struct Histogram *h)
{
	int i;

	for (i = 0; i < h->used; i++) {
		uint32_t p = h->colour[i];
		unsigned r = (p >> 16) & 0xFFu;
		unsigned g = (p >> 8) & 0xFFu;
		unsigned b = p & 0xFFu;

		if (r == g && g == b) {
			return 1;
		}
	}
	return 0;
}

static void note_histogram(const char *what, const struct Histogram *h)
{
	int i;

	printf("  (note: %s —", what);
	for (i = 0; i < h->used; i++) {
		printf(" 0x%06x x%d", h->colour[i], h->count[i]);
	}
	printf(")\n");
}

/* HOW FAR RIGHT THE PAGE'S OWN COLOUR REACHES, in the row at mid height: the gutter a styled bar takes is what makes
 * this less than the frame's width, where an OVERLAY bar leaves it at the frame's width. */
static int page_right_edge(struct App *app, int row)
{
	int x;

	for (x = app->frame_width - 1; x >= 0; x--) {
		if (at(app, x, row) == PAGE_COLOUR) {
			return x;
		}
	}
	return -1;
}

/* AND THE FRAME ITSELF CAN BE KEPT, because a bar is a picture and the numbers above are only its silhouette:
 * `AGWEBVIEW_SCROLLBAR_PPM=/tmp/bar.ppm` writes the last frame this probe drew. */
static void maybe_write_ppm(const struct App *app, const char *path)
{
	FILE *f;
	int x, y;

	if (path == NULL || app->frame == NULL) {
		return;
	}
	f = fopen(path, "wb");
	if (f == NULL) {
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", app->frame_width, app->frame_height);
	for (y = 0; y < app->frame_height; y++) {
		for (x = 0; x < app->frame_width; x++) {
			uint32_t p = at((struct App *)app, x, y);

			fputc((int)((p >> 16) & 0xFFu), f);
			fputc((int)((p >> 8) & 0xFFu), f);
			fputc((int)(p & 0xFFu), f);
		}
	}
	fclose(f);
}

static int s_failed;

static void check(int ok, const char *what)
{
	printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		s_failed++;
	}
}

int main(void)
{
	struct App styled;
	struct App plain;
	struct Histogram hist;
	struct Histogram hist_plain;
	int edge;
	int x;
	int found_track = 0;
	int found_edge = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	memset(&styled, 0, sizeof styled);
	memset(&plain, 0, sizeof plain);

	printf("webengine_scrollbar: the page's bars, drawn as the AppKit's scroller\n");

	/* 1. **THE BAR IS THERE AT REST, WHICH IS THE WHOLE COMPLAINT.** Nothing is scrolled and no pointer has moved. */
	unsetenv("AGWEBVIEW_NO_SCROLLBAR_STYLE");
	if (!load_page(&styled, page)) {
		printf("SKIP: the engine did not load a page (no WebKit?)\n");
		return 77;
	}
	check(styled.frame_width == VIEW_WIDTH && styled.frame_height == VIEW_HEIGHT,
	      "the engine's frame is the view's size");

	histogram_column(&styled, &hist);
	note_histogram("with the style, the bar's column", &hist);
	check(hist.used > 1, "a bar is drawn in the rightmost fifteen points WITHOUT scrolling anything");

	edge = page_right_edge(&styled, styled.frame_height / 2);
	printf("  (note: the page's colour reaches x=%d of %d)\n", edge, styled.frame_width);
	check(edge >= 0 && edge < styled.frame_width - BAR_WIDTH + 1,
	      "the page stops short of the frame's edge, so the bar takes its points out of the LAYOUT");

	/* THE KNOB'S EDGE IS A ONE-POINT BORDER, so it is found by scanning the bar's columns for the colour rather than by
	 * guessing which row the knob's top is on. */
	for (x = styled.frame_width - BAR_WIDTH; x < styled.frame_width && !found_edge; x++) {
		int y;

		for (y = 0; y < styled.frame_height; y++) {
			uint32_t p = at(&styled, x, y);

			if (grey_is(p, TRACK_GREY)) {
				found_track = 1;
			}
			if (grey_is(p, EDGE_GREY)) {
				found_edge = 1;
			}
		}
	}
	check(found_track, "and its groove is the AppKit's scroller grey (#d9d9d9)");
	maybe_write_ppm(&styled, getenv("AGWEBVIEW_SCROLLBAR_PPM"));

	/* **AND IT STAYS, WHICH IS THE DIFFERENCE BETWEEN A SCROLLER AND A FADE.** Nothing is touched for a second and a half:
	 * a bar that is part of the layout is still there, and an overlay bar would not be. */
	pump_for(&styled, 1500);
	histogram_column(&styled, &hist);
	note_histogram("with the style, a second and a half later", &hist);
	check(hist.used > 1 && hist.total > 0,
	      "and it is STILL drawn a second and a half later, having been touched by nothing");
	check(found_edge, "and the knob's border grey (#bcbcbc) is too, so the knob is the AppKit's and not WebKit's");

	free(styled.frame);
	AGWebEngineDestroy(styled.engine);

	/* 2. **AND THE ESCAPE HATCH PUTS WEBKIT'S OWN BARS BACK**, which on this port means overlay bars and therefore no
	 * visible bar at rest: the page's colour reaches the frame's edge. Without this half, "we restyled the bars" and "we
	 * drew something grey in the corner" would pass the same checks. */
	setenv("AGWEBVIEW_NO_SCROLLBAR_STYLE", "1", 1);
	if (!load_page(&plain, page)) {
		printf("SKIP: the engine did not load a page with the style disabled (no WebKit?)\n");
		free(styled.frame);
		return 77;
	}
	unsetenv("AGWEBVIEW_NO_SCROLLBAR_STYLE");
	/* **THE OVERLAY BAR IS DRAWN AT FIRST AND THEN FADES**, which is why this waits before reading: MEASURED, the column
	 * right after the load held the page's colour plus three darker blends of it — WebKit's translucent overlay thumb —
	 * and a moment later held the page's colour alone. That fade IS the complaint. */
	pump_for(&plain, 1500);
	edge = page_right_edge(&plain, plain.frame_height / 2);
	printf("  (note: with the style disabled the page's colour reaches x=%d of %d)\n", edge, plain.frame_width);
	check(edge == plain.frame_width - 1,
	      "with the style disabled the page fills the frame, because WebKit's own bars are OVERLAY bars");
	histogram_column(&plain, &hist_plain);
	note_histogram("without the style, the same column", &hist_plain);
	/* **WEBKIT'S OWN BAR IS STILL THERE, AND IT IS NOT A BAR ANYONE CAN READ.** It is an overlay: a translucent tint of the
	 * page's own colour with no groove, no knob edge and no arrows — and, after a scroll, no bar at all once it has faded
	 * (MEASURED in a nested session: a 3000-point document's own colour ran to its last column two seconds after a scroll).
	 * So the assertion is about its COMPOSITION rather than its absence: no grey in the column, where ours is all grey. */
	check(!histogram_has_grey(&hist_plain),
	      "with the style disabled the column holds no grey at all — WebKit's overlay bar is a tint of the page");

	free(plain.frame);
	AGWebEngineDestroy(plain.engine);

	/* 3. **AND THE OTHER AXIS, WHICH IS THE SAME STYLE ASKED A SECOND QUESTION.** A page wider than the viewport gets a
	 * bar along the BOTTOM, with a left and a right arrow: the same `:horizontal` rules, the same two hidden second
	 * buttons, and the same fifteen points out of the layout. */
	{
		struct App wide;

		memset(&wide, 0, sizeof wide);
		if (load_page(&wide, "<html><body style='margin:0;background:#00c0c0'>"
				    "<div style='width:3000px;height:100px'></div></body></html>")) {
			struct Histogram bottom;
			int bottom_edge = -1;
			int y;

			for (y = 0; y < wide.frame_height - 2; y++) {
				if (at(&wide, wide.frame_width / 2, y) != PAGE_COLOUR) {
					continue;
				}
				bottom_edge = y;
			}
			printf("  (note: the wide page's colour reaches y=%d of %d)\n", bottom_edge, wide.frame_height);
			check(bottom_edge >= 0 && bottom_edge < wide.frame_height - BAR_WIDTH + 1,
			      "a page wider than the viewport gets a bar along the bottom, taking its points out of the layout");
			histogram_row(&wide, &bottom);
			note_histogram("the bottom bar's row", &bottom);
			check(histogram_has_grey(&bottom), "and that bar wears the AppKit's greys too");
		} else {
			printf("  (note: the wide page did not load; skipping the horizontal half)\n");
		}
		free(wide.frame);
		AGWebEngineDestroy(wide.engine);
	}

	/* 4. **AND A PAGE THAT PAINTS ITS OWN BAR LOSES TO THE DESKTOP'S, WHICH IS WHAT `USER` LEVEL MEANS.** WebKit's own
	 * header says a user style sheet "always override[s] other style sheets", so a site's 30-point red bar is replaced by
	 * the fifteen-point scroller every Agora application wears. Measured as the ABSENCE of the page's colour and width:
	 * a site cannot half-win this. */
	{
		struct App site;

		memset(&site, 0, sizeof site);
		if (load_page(&site, "<html><head><style>"
				     "::-webkit-scrollbar { width: 30px; background: #ff0000; }"
				     "</style></head><body style='margin:0;background:#00c0c0'>"
				     "<div style='height:1800px'></div></body></html>")) {
			struct Histogram column;
			int edge;
			int red = 0;
			int x;

			pump_for(&site, 500);
			histogram_column(&site, &column);
			note_histogram("a page with its own 30-point red bar", &column);
			edge = page_right_edge(&site, site.frame_height / 2);
			check(edge >= 0 && edge < site.frame_width - BAR_WIDTH + 1,
			      "a page that styles its own scrollbar still gets the desktop's, fifteen points wide");
			for (x = 0; x < site.frame_width; x++) {
				int yy;

				for (yy = 0; yy < site.frame_height; yy++) {
					uint32_t p = at(&site, x, yy);

					if (((p >> 16) & 0xFFu) > 0xF0u && ((p >> 8) & 0xFFu) < 0x10u && (p & 0xFFu) < 0x10u) {
						red++;
					}
				}
			}
			check(red == 0, "and the page's own red bar is not drawn anywhere");
			maybe_write_ppm(&site, getenv("AGWEBVIEW_SCROLLBAR_PPM_SITE"));
		} else {
			printf("  (note: the self-styled page did not load; skipping that half)\n");
		}
		free(site.frame);
		AGWebEngineDestroy(site.engine);
	}

	if (s_failed != 0) {
		printf("webengine_scrollbar: %d check(s) failed\n", s_failed);
		return 1;
	}
	printf("webengine_scrollbar: ok\n");
	return 0;
}
