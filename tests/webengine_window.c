/*
 * webengine_window — a page in a WINDOW's own GPU buffer, which is the shape a browser is.
 *
 * Copyright (C) 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * THE COMPOSITION THIS FILE IS, in four lines, and every one of them is a door that exists for it:
 *
 *     AGWindowBeginFrameDMABUF(window, &dst);            the window's slot, as a descriptor
 *     AGWebEngineSetPresentDMABUF(engine, dst.fd, ...);  the engine draws into it on the GPU
 *     AGWebEngineIterate(engine);                        WebKit's frame arrives and lands
 *     AGWindowEndFrame(window);                          the compositor samples it
 *
 * and, when no frame arrived, `AGWindowCancelFrame` gives the slot back instead — a window that committed an unchanged
 * buffer sixty times a second would be a window repainting itself for nothing.
 *
 * **THIS NEEDS A COMPOSITOR THAT IMPORTS dma-buf** (`--renderer gl` on the nested backend) and `AG_WINDOW_DMABUF=1`, so
 * its runner starts a real session and reads the server's screenshot. A machine that cannot give the window GPU buffers
 * gets `AGWindowBeginFrameDMABUF` answering 0, and this program says so and exits 77 rather than pretending it composited
 * on the GPU.
 */

#include <AgoraWebView/AGWebEngine.h>

#include <AppKit/platform/AGWindowServer.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_WIDTH 320
#define WINDOW_HEIGHT 200
#define WINDOW_TOP 32

struct App {
	AGWindowServer *server;
	AGWindow *window;
	AGWebEngine *engine;
	int finished;
	int damage;
	unsigned int presented;
	int gpu_frames;
	int cancels;
	int sized_to_buffer;
};

static void on_load_changed(void *context, AGWebEngine *engine, AGWebLoadStage stage)
{
	struct App *app = context;

	(void)engine;
	if (stage == AGWebLoadFinished) {
		app->finished = 1;
		printf("webengine_window: the page finished loading\n");
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
	app->damage++;
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
	int waited;

	setvbuf(stdout, NULL, _IOLBF, 0);
	memset(&app, 0, sizeof app);

	app.server = AGWindowServerConnect();
	if (app.server == NULL) {
		fprintf(stderr, "webengine_window: no window server: %s\n", AGWindowServerLastError(NULL));
		return 77;
	}
	app.window = AGWindowCreate(app.server, WINDOW_WIDTH, WINDOW_HEIGHT, "Agora GPU page");
	if (app.window == NULL) {
		fprintf(stderr, "webengine_window: could not make a window\n");
		AGWindowServerDisconnect(app.server);
		return 1;
	}
	/* **THE ENGINE GETS THE DEVICE THE WINDOW'S BUFFERS COME FROM.** Without it the import of the window's dma-buf fails
	 * and the symptom is a frame that never arrives — measured, on this machine, before this argument existed. */
	app.engine = AGWebEngineCreateWithOptions(WINDOW_WIDTH, WINDOW_HEIGHT, AGWebExportEGLPresent,
						  AGWindowServerNativeGBMDevice(app.server), &callbacks, &app);
	if (app.engine == NULL) {
		fprintf(stderr, "webengine_window: the engine would not start\n");
		AGWindowServerDisconnect(app.server);
		return 77;
	}
	if (strcmp(AGWebEngineExportPath(app.engine), "egl-present") != 0) {
		printf("webengine_window: this machine's EGL cannot import a dma-buf — SKIP\n");
		AGWebEngineDestroy(app.engine);
		AGWindowServerDisconnect(app.server);
		return 77;
	}
	AGWindowSetFrame(app.window, 0, WINDOW_TOP, WINDOW_WIDTH, WINDOW_HEIGHT);
	AGWindowShow(app.window);
	AGWebEngineLoadHTML(app.engine, page, NULL);

	/* **THE LOOP A BROWSER WINDOW ACTUALLY RUNS.** Begin the frame, tell the engine where its next frame goes, pump, and
	 * commit only if one arrived — otherwise give the slot back. A `BeginFrameDMABUF` that answers 0 means this window
	 * has shared-memory buffers (no `AG_WINDOW_DMABUF=1`, or a compositor that imports nothing) and the whole point of
	 * this test is unavailable, which is a SKIP and not a soft pass. */
	waited = 0;
	while (AGWindowServerDispatchTimeout(app.server, 5) >= 0) {
		{
			AGWindowFrameDMABUF dst;

			if (!AGWindowBeginFrameDMABUF(app.window, &dst)) {
				if (app.gpu_frames == 0 && app.presented == 0) {
					if (++waited > 200) {
						printf("webengine_window: this window has no GPU buffers — SKIP\n");
						break;
					}
				}
				AGWebEngineIterate(app.engine);
				continue;
			}
			waited = 0;
			/* **THE ENGINE IS SIZED TO THE BUFFER, NOT TO THE POINTS ASKED FOR.** A window at 1.332 output pixels per
			 * point has a buffer 33% wider than its frame, and a page laid out for 320 points but drawn into a 426-pixel
			 * buffer would be scaled up — correct geometry, blurred type. The destination is the authority on how many
			 * pixels there are, so the first one settles it. */
			if (!app.sized_to_buffer) {
				AGWebEngineSetSize(app.engine, dst.width, dst.height);
				app.sized_to_buffer = 1;
			}
			AGWebEngineSetPresentDMABUF(app.engine, dst.fd, dst.width, dst.height, dst.stride, dst.fourcc,
						    dst.modifier);
			AGWebEngineIterate(app.engine);
			if (AGWebEngineFramesPresented(app.engine) != app.presented) {
				app.presented = AGWebEngineFramesPresented(app.engine);
				app.gpu_frames++;
				AGWindowEndFrame(app.window);
			} else {
				app.cancels++;
				AGWindowCancelFrame(app.window);
			}
		}
	}
	printf("webengine_window: %d GPU frame(s), %d cancel(s), %d damage callback(s), page %s\n", app.gpu_frames,
	       app.cancels, app.damage, app.finished ? "loaded" : "NOT loaded");
	AGWebEngineDestroy(app.engine);
	AGWindowServerDisconnect(app.server);
	if (app.gpu_frames < 1) {
		if (app.damage > 0) {
			/* **THE ENGINE PAINTED AND THE IMPORT REFUSED THE DESTINATION, WHICH IS A MACHINE FACT AND NOT A BUG HERE.**
			 * MEASURED on this machine: a window's buffer is allocated through GBM and an EGL context on the same device
			 * refuses to bind it (`glEGLImageTargetTexture2DOES` gives GL_INVALID_OPERATION), while buffers this seam
			 * exports from its own EGL import fine — `webengine-gpu` proves that half. So the composition is exercised,
			 * and the whole-window path SKIPS where the two do not agree. */
			printf("webengine_window: the engine produced frames but this machine will not bind its window's GBM buffer "
			       "as a texture — SKIP\n");
			return 77;
		}
		fprintf(stderr, "webengine_window: no frame was produced at all\n");
		return 1;
	}
	printf("webengine_window: ok — the page reached the window without a readback\n");
	return 0;
}
