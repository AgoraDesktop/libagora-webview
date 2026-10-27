/*
 * Copyright (C) 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **LINKS, WHICH A BROWSER EXISTS TO FOLLOW.** Without a `decide-policy` handler WebKit IGNORES a navigation it is asked
 * about, so a click on a link does nothing at all — MEASURED before the handler existed: a press/release on `<a href>`
 * left the URI unchanged and the click went nowhere. This probe is two pages on disk, a click on the link between them,
 * the back step a link makes worth having, and the one refusal that keeps the emulated drag honest: navigation is
 * suppressed while a selection drag is in flight, because that drag is a run of SHIFT-CLICKS and a shift-click that
 * lands on a link would navigate.
 *
 * WHY THE PAGES ARE FILES. A link has to point somewhere the engine can go, and two `file://` URLs in a temporary
 * directory give two distinct URIs with no network involved — so the assertion is on `AGWebEngineURI` itself, the address
 * the engine says it is at, rather than on a title a page could lie about.
 *
 * TWO CLICKS PER STEP, both times. MEASURED earlier in this component: the FIRST press/release pair a page ever receives
 * can be spent taking the focus, so a probe that clicks once and reads the URI reports "the link does not work" on a
 * link that works on the second click.
 *
 * NO COMPOSITOR IS NEEDED — this is the seam's own behaviour — so it runs anywhere and SKIPS (77) on a machine with no
 * WPE WebKit.
 */
#include <AgoraWebView/AGWebEngine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LINK_X 40
#define LINK_Y 40
#define BLANK_X 40
#define BLANK_Y 110

static int damaged;
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

static void on_damaged(void *context, AGWebEngine *engine, int x, int y, int width, int height)
{
	(void)context;
	(void)engine;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	damaged++;
}

static void nap(int milliseconds)
{
	struct timespec ts = { milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static void settle(AGWebEngine *engine, int milliseconds)
{
	int elapsed;

	for (elapsed = 0; elapsed < milliseconds; elapsed++) {
		AGWebEngineIterate(engine);
		nap(1);
	}
}

static const char *uri(AGWebEngine *engine)
{
	const char *u = AGWebEngineURI(engine);

	return u != NULL ? u : "(no URI)";
}

static void click(AGWebEngine *engine, int x, int y, uint32_t time_ms)
{
	AGWebEnginePointerButton(engine, x, y, 0x110u, 1, 0, time_ms);
	settle(engine, 20);
	AGWebEnginePointerButton(engine, x, y, 0x110u, 0, 0, time_ms + 5);
	settle(engine, 150);
}

static int wait_for_uri(AGWebEngine *engine, const char *want, int timeout_ms)
{
	int elapsed;

	for (elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
		if (strstr(uri(engine), want) != NULL) {
			return 1;
		}
		settle(engine, 10);
	}
	return 0;
}

static int write_page(const char *path, const char *body)
{
	FILE *file = fopen(path, "w");
	int ok;

	if (file == NULL) {
		return 0;
	}
	ok = fprintf(file, "%s", body) > 0;
	fclose(file);
	return ok;
}

int main(void)
{
	static const AGWebEngineCallbacks callbacks = { on_load_changed, NULL, on_damaged };
	char directory[128];
	char page_a[192];
	char page_b[192];
	char path[192];
	char html[512];
	char command[192];
	AGWebEngine *engine;
	int started;

	setvbuf(stdout, NULL, _IOLBF, 0);
	snprintf(directory, sizeof directory, "/tmp/agora-links-%d", (int)getpid());
	if (mkdir(directory, 0700) != 0) {
		fprintf(stderr, "links: could not make %s\n", directory);
		return 1;
	}
	snprintf(page_a, sizeof page_a, "file://%s/a.html", directory);
	snprintf(page_b, sizeof page_b, "file://%s/b.html", directory);

	snprintf(html, sizeof html,
		 "<html><head><title>links A</title><style>body{margin:0;font:16px sans-serif}</style></head><body>"
		 "<a href=\"file://%s/b.html\" style=\"display:block;padding:30px;background:#e0e0e0\">go to B</a>"
		 "<a href=\"file://%s/c.html\" target=\"_blank\" style=\"display:block;padding:30px\">open C in a new window</a>"
		 "<p>page A</p></body></html>",
		 directory, directory);
	snprintf(path, sizeof path, "%s/a.html", directory);
	if (!write_page(path, html)) {
		fprintf(stderr, "links: could not write %s\n", path);
		return 1;
	}
	snprintf(path, sizeof path, "%s/c.html", directory);
	if (!write_page(path, "<html><head><title>links C</title></head><body><p>page C</p></body></html>")) {
		fprintf(stderr, "links: could not write %s\n", path);
		return 1;
	}
	snprintf(path, sizeof path, "%s/b.html", directory);
	if (!write_page(path, "<html><head><title>links B</title></head><body><p>page B</p></body></html>")) {
		fprintf(stderr, "links: could not write %s\n", path);
		return 1;
	}

	engine = AGWebEngineCreate(640, 300, &callbacks, NULL);
	if (engine == NULL) {
		printf("links: the engine would not start\n");
		return 77;		/* SKIP: a machine with no WPE WebKit is not a failure of this code */
	}

	AGWebEngineLoadURI(engine, page_a);
	started = 0;
	while (started < 5000 && (!finished || damaged < 2)) {
		AGWebEngineIterate(engine);
		nap(1);
		started++;
	}
	settle(engine, 200);
	printf("links: loaded %s\n", uri(engine));

	/* 1. THE LINK. */
	click(engine, LINK_X, LINK_Y, 100);
	click(engine, LINK_X, LINK_Y, 400);
	if (wait_for_uri(engine, "b.html", 3000)) {
		printf("links: the link navigated to %s\n", uri(engine));
	} else {
		fprintf(stderr, "links: clicking the link left the engine at %s, want b.html\n", uri(engine));
		failed++;
	}

	/* 2. THE STEP A LINK MAKES WORTH HAVING. */
	if (AGWebEngineCanGoBack(engine)) {
		AGWebEngineGoBack(engine);
		if (wait_for_uri(engine, "a.html", 3000)) {
			printf("links: back returned to %s\n", uri(engine));
		} else {
			fprintf(stderr, "links: back left the engine at %s, want a.html\n", uri(engine));
			failed++;
		}
	} else {
		fprintf(stderr, "links: the engine says it cannot go back after following a link\n");
		failed++;
	}
	settle(engine, 200);

	/* 3. **AND THE ONE REFUSAL.** A selection drag is a run of shift-clicks, so a drag across a link would navigate
	 * unless the view says otherwise — this is the seam's side of that rule, asserted by clicking the link with
	 * navigation suppressed and checking the engine stayed put. */
	AGWebEngineSetSuppressNavigation(engine, 1);
	click(engine, LINK_X, LINK_Y, 900);
	click(engine, LINK_X, LINK_Y, 1200);
	settle(engine, 500);
	if (strstr(uri(engine), "a.html") != NULL) {
		printf("links: with navigation suppressed the click stayed on %s\n", uri(engine));
	} else {
		fprintf(stderr, "links: a suppressed click navigated to %s anyway\n", uri(engine));
		failed++;
	}

	/* 4. **AND SAYS NOTHING MORE THAN THAT.** Suppression is a switch, not a latch: with it off again the same click
	 * has to work, or a single drag would leave the browser unable to follow any link. */
	AGWebEngineSetSuppressNavigation(engine, 0);
	click(engine, LINK_X, LINK_Y, 1600);
	click(engine, LINK_X, LINK_Y, 1900);
	if (wait_for_uri(engine, "b.html", 3000)) {
		printf("links: with suppression lifted the link works again (%s)\n", uri(engine));
	} else {
		fprintf(stderr, "links: the link never worked again after suppression was lifted (%s)\n", uri(engine));
		failed++;
	}

	/* 5. **A LINK THAT ASKS FOR A NEW WINDOW, WHICH IS WHAT THE POLICY HANDLER IS REALLY FOR.** WebKit reports it as a
	 * NEW_WINDOW_ACTION and IGNORES it by default, because there is no window to open and this embedder creates none — a
	 * `target="_blank"` link would simply do nothing. The handler loads it in this view instead, which is the honest
	 * single-view answer until tabs exist. MEASURED both ways: with the handler disconnected this step stays on c.html. */
	AGWebEngineGoBack(engine);
	(void)wait_for_uri(engine, "a.html", 3000);
	settle(engine, 200);
	click(engine, BLANK_X, BLANK_Y, 2400);
	click(engine, BLANK_X, BLANK_Y, 2700);
	if (wait_for_uri(engine, "c.html", 3000)) {
		printf("links: a target=\"_blank\" link opened in this view (%s)\n", uri(engine));
	} else {
		fprintf(stderr, "links: a target=\"_blank\" link went nowhere (%s)\n", uri(engine));
		failed++;
	}

	AGWebEngineDestroy(engine);
	snprintf(command, sizeof command, "rm -rf %s", directory);
	(void)system(command);

	if (failed != 0) {
		fprintf(stderr, "links: %d check(s) failed\n", failed);
		return 1;
	}
	printf("links: ok — a link navigates, back returns, and a suppressed click does neither\n");
	return 0;
}
