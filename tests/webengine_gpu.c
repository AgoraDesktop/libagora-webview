/*
 * webengine_gpu — a page composited into somebody else's GPU buffer, with no readback on our side.
 *
 * Copyright (C) 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHAT IS ASKED, AND WHY IT IS ASKED THIS WAY. The present mode exists so that a frame can go from WebKit's texture to a
 * window's dma-buf without a megapixel readback in between. The engine therefore has NO pixels to hand back — which is
 * exactly what makes this testable without a compositor: a DESTINATION is manufactured here (a texture, exported to a
 * descriptor), the engine is told to draw into it, and then the test imports that same descriptor and reads it back
 * ITSELF. If the page's colours are in that buffer, the composition happened; and because the engine's own `AGWebEngineDraw`
 * is asserted to have nothing, a test that "passed" by the engine secretly reading back cannot.
 *
 * WHAT COULD GO WRONG THAT THIS CATCHES:
 *   - a blit without the flip — the red band would be at the bottom;
 *   - a destination imported but never rendered into — the buffer keeps the colour the test filled it with;
 *   - the engine falling back to the readback path — `AGWebEngineDraw` would answer 1, and the assertion says it must not.
 *
 * SKIPS (77) when this machine has no EGL that can import a dma-buf: the seam falls back to shared memory, and a test that
 * claimed the GPU path on a machine that took the CPU one would be a lie with a green tick.
 */

#include <AgoraWebView/AGWebEngine.h>

#include <AppKit/platform/AGGL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WIDTH 320
#define HEIGHT 200

/* The destination's own colour, so "nothing was drawn" is distinguishable from "the page is black at the top". */
#define DESTINATION_CLEAR 0x00204060u

struct App {
	AGWebEngine *engine;
	int finished;
	int frames;
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

static int now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* A buffer to be drawn into: a texture the GPU fills with one colour, exported as a descriptor. Returns the texture and
 * hands back the descriptor — exactly what a window's buffer gives the engine, minus the window. */
static unsigned int make_destination(AGGLContext *gl, int *fd_out, int *stride_out, unsigned int *fourcc_out,
				     unsigned long long *modifier_out)
{
	uint32_t *pixels = malloc((size_t)WIDTH * HEIGHT * 4u);
	unsigned int texture;
	AGGLInfo info;
	int i;

	if (pixels == NULL) {
		return 0;
	}
	for (i = 0; i < WIDTH * HEIGHT; i++) {
		pixels[i] = DESTINATION_CLEAR;
	}
	AGGLContextInfo(gl, &info);
	if (!info.export_dmabuf || !info.gl_texture_image) {
		free(pixels);
		return 0;
	}
	AGGLBeginFrame(gl, WIDTH, HEIGHT);
	texture = AGGLTextureFromPixels(gl, pixels, WIDTH, HEIGHT, WIDTH * 4);
	AGGLEndFrame(gl);
	free(pixels);
	if (texture == 0) {
		return 0;
	}
	if (!AGGLExportTextureDmabuf(gl, texture, WIDTH, HEIGHT, fd_out, stride_out, fourcc_out, modifier_out)) {
		AGGLDeleteTexture(gl, texture);
		return 0;
	}
	return texture;
}

/* Read the destination back: import it, draw it into this context's offscreen target, and hand out the pixels. */
static int read_destination(AGGLContext *gl, int fd, int stride, unsigned int fourcc, unsigned long long modifier,
			    uint32_t *out)
{
	unsigned int texture = AGGLTextureFromDmabuf(gl, fd, WIDTH, HEIGHT, stride, fourcc, modifier);
	int ok;

	if (texture == 0) {
		return 0;
	}
	ok = AGGLBeginFrame(gl, WIDTH, HEIGHT);
	if (ok) {
		AGGLFillRect(gl, 0, 0, WIDTH, HEIGHT, 0x00000000u);
		ok = AGGLBlitTextureFlipped(gl, texture, 0, 0, WIDTH, HEIGHT);
		AGGLEndFrame(gl);
	}
	if (ok) {
		ok = AGGLReadPixels(gl, 0, 0, WIDTH, HEIGHT, out, WIDTH * 4);
	}
	AGGLDeleteTexture(gl, texture);
	return ok;
}

int main(void)
{
	/* **THE COLOURS COME FROM A STYLESHEET, NOT FROM `style=` ATTRIBUTES, AND THAT IS THE WHOLE OF WHY THIS PAGE
	 * HOVERS.** An inline style beats any selector short of `!important`, so a `:hover` rule that sets the same property
	 * on an element whose colour was set inline changes nothing — MEASURED: the hover produced no repaint at all, in
	 * either export mode, until the base colours moved into the sheet. */
	static const char *page =
		"<html><head><style>"
		"  body { margin:0; }"
		"  #top { height:40px; background:#ff0000; }"
		"  #h { height:40px; background:#002040; }"
		"  #h:hover { background:#00c000; }"
		"  #k { height:40px; background:#400020; }"
		"  #body { height:80px; background:#0000c0; }"
		"</style></head><body>"
		"<div id=top></div><div id=h></div><div id=k></div><div id=body></div>"
		"<script>window.addEventListener('keydown',function(){"
		"document.getElementById('k').style.background='#ff0000';});</script>"
		"</body></html>";
	static const AGWebEngineCallbacks callbacks = { on_load_changed, NULL, on_damaged };
	struct App app;
	AGGLContext *gl;
	unsigned int destination;
	int fd = -1;
	int stride = 0;
	unsigned int fourcc = 0;
	unsigned long long modifier = 0;
	uint32_t *pixels;
	const char *path;
	int waited;
	uint32_t top;
	uint32_t middle;
#define HOVER_Y 60
#define KEY_Y 100

	memset(&app, 0, sizeof app);
	app.engine = AGWebEngineCreateWithExport(WIDTH, HEIGHT, AGWebExportEGLPresent, &callbacks, &app);
	if (app.engine == NULL) {
		printf("gpu: the engine would not start\n");
		return 77;
	}
	path = AGWebEngineExportPath(app.engine);
	if (strcmp(path, "egl-present") != 0) {
		printf("gpu: this machine's EGL cannot import a dma-buf, so the seam fell back to '%s' — SKIP\n", path);
		AGWebEngineDestroy(app.engine);
		return 77;
	}
	/* **A SECOND CONTEXT, FOR THE DESTINATION AND FOR READING IT BACK.** Both are the test's own: the engine never reads
	 * the buffer, which is the claim. */
	gl = AGGLContextCreate(NULL);
	if (gl == NULL) {
		printf("gpu: no AGGL context for the destination — SKIP\n");
		AGWebEngineDestroy(app.engine);
		return 77;
	}
	destination = make_destination(gl, &fd, &stride, &fourcc, &modifier);
	if (destination == 0) {
		printf("gpu: this driver cannot export a texture to a dma-buf — SKIP\n");
		AGGLContextDestroy(gl);
		AGWebEngineDestroy(app.engine);
		return 77;
	}
	printf("gpu: destination %dx%d stride %d fourcc 0x%08x modifier 0x%llx\n", WIDTH, HEIGHT, stride, fourcc,
	       modifier);
	if (!AGWebEngineSetPresentDMABUF(app.engine, fd, WIDTH, HEIGHT, stride, fourcc, modifier)) {
		fprintf(stderr, "gpu: the engine refused the destination\n");
		AGGLDeleteTexture(gl, destination);
		close(fd);
		AGGLContextDestroy(gl);
		AGWebEngineDestroy(app.engine);
		return 1;
	}

	AGWebEngineLoadHTML(app.engine, page, NULL);
	waited = 0;
	while (!app.finished) {
		AGWebEngineIterate(app.engine);
		nap(2);
		if (++waited > 2500) {
			fprintf(stderr, "gpu: the page never loaded\n");
			break;
		}
	}
	pixels = malloc((size_t)WIDTH * HEIGHT * 4u);
	if (pixels == NULL) {
		return 1;
	}
	/* **KEEP PUMPING UNTIL THE PAGE'S FRAME LANDS.** WebKit composites a blank white frame before the content is
	 * committed — the same first frame the readback tests have to step past — and a test that stopped at "a frame was
	 * composited" would read that one and call it the page. Every frame composites into the same destination, so what is
	 * checked here is the LAST composition. */
	{
		int started = now_ms();
		int composited_before = 0;

		top = 0;
		while (now_ms() - started < 8000) {
			int frames = (int)AGWebEngineFramesPresented(app.engine);

			AGWebEngineIterate(app.engine);
			if ((int)AGWebEngineFramesPresented(app.engine) != frames) {
				if (read_destination(gl, fd, stride, fourcc, modifier, pixels)) {
					top = pixels[20 * WIDTH + 160];
					middle = pixels[160 * WIDTH + 160];
					if (top == 0x00FF0000u && middle == 0x000000C0u) {
						break;
					}
				}
			}
			nap(2);
		}
		(void)composited_before;
	}
	app.frames = (int)AGWebEngineFramesPresented(app.engine);
	printf("gpu: %d frame(s) composited, %d damage callback(s)\n", app.frames, app.damaged);
	printf("gpu: destination top 0x%06x, middle 0x%06x (clear colour was 0x%06x)\n", top, middle, DESTINATION_CLEAR);
	free(pixels);

	/* **AND INPUT WORKS IN THIS MODE, BECAUSE NOTHING ABOUT IT IS MODE-SPECIFIC — WHICH IS WORTH ASSERTING RATHER THAN
	 * ASSUMING.** The page has a `:hover` box and a `keydown` listener, both of which paint; the pointer and the key go
	 * through the same doors the shm tests use, and the proof is the DESTINATION's pixels, read by this test. */
	{
		uint32_t *probe = malloc((size_t)WIDTH * HEIGHT * 4u);
		int started;
		int hovered = 0;
		int keyed = 0;

		if (probe != NULL) {
			int frames_before = (int)AGWebEngineFramesPresented(app.engine);

			AGWebEnginePointerMotion(app.engine, WIDTH / 2, HOVER_Y, 0, 1000);
			started = now_ms();
			while (now_ms() - started < 4000 && !hovered) {
				AGWebEngineIterate(app.engine);
				if (read_destination(gl, fd, stride, fourcc, modifier, probe) &&
				    probe[(size_t)HOVER_Y * WIDTH + WIDTH / 2] == 0x0000C000u) {
					hovered = 1;
				}
				nap(2);
			}
			printf("gpu: frames %d -> %d around the pointer, colour at (%d,%d) is 0x%06x\n", frames_before,
			       (int)AGWebEngineFramesPresented(app.engine), WIDTH / 2, HOVER_Y,
			       probe != NULL && read_destination(gl, fd, stride, fourcc, modifier, probe) ?
				       probe[(size_t)HOVER_Y * WIDTH + WIDTH / 2] : 0xFFFFFFFFu);
			AGWebEngineKey(app.engine, 0x0061u /* XKB_KEY_a */, 38 /* KEY_A */, 0, 1, 2000);
			started = now_ms();
			while (now_ms() - started < 4000 && !keyed) {
				AGWebEngineIterate(app.engine);
				if (read_destination(gl, fd, stride, fourcc, modifier, probe) &&
				    probe[(size_t)KEY_Y * WIDTH + WIDTH / 2] == 0x00FF0000u) {
					keyed = 1;
				}
				nap(2);
			}
			free(probe);
		}
		printf("gpu: pointer %s, key %s\n", hovered ? "reached the page" : "did NOT reach the page",
		       keyed ? "reached the page" : "did NOT reach the page");
		if (!hovered || !keyed) {
			fprintf(stderr, "gpu: input did not reach the page in the present mode\n");
			AGGLDeleteTexture(gl, destination);
			close(fd);
			AGGLContextDestroy(gl);
			AGWebEngineDestroy(app.engine);
			return 1;
		}
	}

	/* **THE ENGINE MUST HAVE NO PIXELS OF ITS OWN.** If it does, the readback ran, and the mode this test exists to prove
	 * is not the one that ran. */
	{
		int width = 0;
		int height = 0;
		uint32_t probe = 0;

		AGWebEngineFrameSize(app.engine, &width, &height);
		if (AGWebEngineDraw(app.engine, &probe, 4, 1, 1) != 0) {
			fprintf(stderr, "gpu: the engine handed back pixels — it read the frame back after all\n");
			AGGLDeleteTexture(gl, destination);
			close(fd);
			AGGLContextDestroy(gl);
			AGWebEngineDestroy(app.engine);
			return 1;
		}
	}

	AGGLDeleteTexture(gl, destination);
	close(fd);
	AGGLContextDestroy(gl);
	AGWebEngineDestroy(app.engine);

	if (app.frames == 0) {
		fprintf(stderr, "gpu: no frame was composited\n");
		return 1;
	}
	if (top != 0x00FF0000u) {
		fprintf(stderr, "gpu: the destination's top row is 0x%06x, want red — a flip, or nothing was drawn\n", top);
		return 1;
	}
	if (middle != 0x000000C0u) {
		fprintf(stderr, "gpu: the destination's body is 0x%06x at y=160, want 0x0000c0\n", middle);
		return 1;
	}
	printf("gpu: ok — the page was composited into a GPU buffer, upright, with no readback\n");
	return 0;
}
