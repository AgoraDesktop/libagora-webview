/*
 * AGWebEngine-WPE.c — the web engine seam over WPE WebKit, taking frames as `wl_shm` pixels.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * WHAT THIS IS. WPE WebKit splits a browser into a UI process — this one — and a web process it spawns. The UI process
 * is told what to draw by `libwpe`'s view backend, and `libwpebackend-fdo` is the backend that hands the host finished
 * frames as shared memory (`wl_shm` buffers) or as `EGLImage`s. This file is the host: it creates the exportable view
 * backend, hands WebKit a `WebKitWebViewBackend` made from it, and copies each exported buffer into a frame of its own
 * in the window seam's `0x00RRGGBB`.
 *
 * **THE SEQUENCE IS NOT OBVIOUS AND EVERY STEP OF IT WAS MEASURED**, so it is written down once, here, in the order it
 * has to happen:
 *
 *   1. `wpe_loader_init("libWPEBackend-fdo-1.0.so")`. Without it `libwpe` looks for `libWPEBackend-default.so` when
 *      WebKit asks for a renderer host client, and the process ABORTS inside `wpe_renderer_host_create_client` the
 *      first time a page is loaded — measured as a `SIGABRT` three frames into `webkit_web_view_load_html`.
 *   2. `wpe_fdo_initialize_shm()`, which is what makes the backend able to produce shm buffers at all.
 *   3. `wpe_view_backend_exportable_fdo_create(client, data, width, height)` and
 *      `…_get_view_backend()`, which is the `struct wpe_view_backend*` WebKit wants.
 *   4. `webkit_web_view_backend_new(backend, NULL, NULL)` then `webkit_web_view_new()`, then a load.
 *   5. In the export callback: copy the pixels, `…_dispatch_release_shm_exported_buffer`, **THEN
 *      `…_dispatch_frame_complete`, THEN `wpe_view_backend_dispatch_frame_displayed`**.
 *
 * **STEP 5'S `frame_complete` IS THE ONE THAT IS EASY TO MISS, AND MISSING IT LOOKS LIKE A BROKEN ENGINE.** Without it
 * the host receives exactly ONE frame for ever: measured, a page loaded and finished (all three load stages arrived)
 * and the only pixels ever exported were the blank white pre-load frame — 1 image in 20 seconds of pumping, on both the
 * shm and the EGL export paths. With the call added, the second frame is the page. The other two calls alone are not
 * enough, and the ordering is what the backend's own accounting expects: the buffer is returned, the frame is
 * acknowledged, then the view says it has been displayed.
 *
 * WHY SHM AND NOT EGL. The AppKit draws with the CPU, `AGWindowBeginFrame` measures the cost of blending into GPU memory
 * (2.4 µs a pixel against 2 ns), and a shared-memory frame needs no EGL context, no device and no driver: it is pixels
 * in a file, which is exactly the thing this desktop is built out of. The EGL export path works — `AGGL` imports an
 * `EGLImage` from this backend, measured — and belongs to a caller that composites on the GPU.
 *
 * **THE WEB PROCESS IS SANDBOXED AND THAT IS NOT WORKED AROUND.** WebKit 2.48 spawns its web process inside bubblewrap;
 * it starts here with no environment variable needed and no `/run` rewriting. The bare `WEBKIT_DISABLE_SANDBOX…`
 * escape hatch exists upstream and is deliberately not used by this file or by its test.
 */

#include "AGWebEngine.h"

#include <wpe/wpe.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>
#include <wpe/webkit.h>

/* **THE SHM ACCESSORS ARE `libwayland-server`'S.** The FDO backend hands the host a `struct wl_shm_buffer` — the
 * server-side object, because that is what it built — so reading a frame means those six functions and one link. They
 * are declared here rather than by including `<wayland-server-core.h>`, which cannot be mixed with the client headers
 * in one translation unit on every libwayland this tree supports; the signatures are stable ABI and this is the whole of
 * what a host uses. */
struct wl_shm_buffer;
int wl_shm_buffer_get_width(struct wl_shm_buffer *buffer);
int wl_shm_buffer_get_height(struct wl_shm_buffer *buffer);
int wl_shm_buffer_get_stride(struct wl_shm_buffer *buffer);
uint32_t wl_shm_buffer_get_format(struct wl_shm_buffer *buffer);
void *wl_shm_buffer_get_data(struct wl_shm_buffer *buffer);
void wl_shm_buffer_begin_access(struct wl_shm_buffer *buffer);
void wl_shm_buffer_end_access(struct wl_shm_buffer *buffer);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WL_SHM_FORMAT_ARGB8888, the only format this host accepts; anything else is refused rather than mis-coloured. */
#define AG_WL_SHM_FORMAT_ARGB8888 0u

struct AGWebEngine {
	AGWebEngineCallbacks callbacks;
	void *context;

	struct wpe_view_backend_exportable_fdo *exportable;
	struct wpe_view_backend *backend;
	WebKitWebView *view;

	int width;
	int height;
	uint32_t *frame;	/* the newest exported frame, 0x00RRGGBB, top-down */
	int frame_width;
	int frame_height;
	int have_frame;
};

/* **ONE INITIALISATION PER PROCESS, BECAUSE THE LOADER IS A LIBRARY-WIDE SINGLETON.** `wpe_loader_init` names the
 * backend implementation for the whole process and `wpe_fdo_initialize_shm` sets up the export machinery, so neither is
 * called twice — and a failure is remembered rather than retried, because the second attempt would print the same
 * diagnosis and fail the same way. */
static int ag_web_library_ready(void)
{
	static int state;	/* 0 = not tried, 1 = ready, -1 = failed */

	if (state == 0) {
		if (!wpe_loader_init("libWPEBackend-fdo-1.0.so")) {
			fprintf(stderr, "AGWebEngine: libwpe could not load libWPEBackend-fdo-1.0.so\n");
			state = -1;
		} else if (!wpe_fdo_initialize_shm()) {
			fprintf(stderr, "AGWebEngine: libwpebackend-fdo could not initialise its shm export\n");
			state = -1;
		} else {
			state = 1;
		}
	}
	return state == 1;
}

/* ----------------------------------------------------------------------------------------------------------------
 * frames in
 * ---------------------------------------------------------------------------------------------------------------- */

/* **ONE FRAME, COPIED AND CONVERTED IN ONE PLACE.** ARGB8888's bytes are B,G,R,A on a little-endian host and the window
 * seam's integer is `0x00RRGGBB`, so the copy is `& 0x00ffffff` per pixel — the alpha is dropped because an Agora window
 * is opaque and the engine's page background is opaque too. The frame buffer is reallocated only when the engine's size
 * changes, which is a resize and not a frame. */
static void ag_web_take_pixels(AGWebEngine *engine, struct wl_shm_buffer *buffer, int width, int height, int stride,
			       const unsigned char *data)
{
	uint32_t *grown;
	int x, y;

	if (width <= 0 || height <= 0) {
		return;
	}
	if (engine->frame == NULL || engine->frame_width != width || engine->frame_height != height) {
		grown = realloc(engine->frame, (size_t)width * (size_t)height * 4u);
		if (grown == NULL) {
			return;
		}
		engine->frame = grown;
		engine->frame_width = width;
		engine->frame_height = height;
	}
	for (y = 0; y < height; y++) {
		const uint32_t *row = (const uint32_t *)(data + (size_t)y * (size_t)stride);
		uint32_t *out = engine->frame + (size_t)y * (size_t)width;

		for (x = 0; x < width; x++) {
			out[x] = row[x] & 0x00ffffffu;
		}
	}
	engine->have_frame = 1;
	(void)buffer;
}

static void ag_web_export_shm_buffer(void *data, struct wpe_fdo_shm_exported_buffer *buffer)
{
	AGWebEngine *engine = data;
	struct wl_shm_buffer *shm = wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer);

	if (shm == NULL) {
		return;
	}
	if (wl_shm_buffer_get_format(shm) != AG_WL_SHM_FORMAT_ARGB8888) {
		fprintf(stderr, "AGWebEngine: the engine exported format 0x%08x, which this host cannot read\n",
			wl_shm_buffer_get_format(shm));
	} else {
		wl_shm_buffer_begin_access(shm);
		ag_web_take_pixels(engine, shm, wl_shm_buffer_get_width(shm), wl_shm_buffer_get_height(shm),
				   wl_shm_buffer_get_stride(shm), wl_shm_buffer_get_data(shm));
		wl_shm_buffer_end_access(shm);
	}
	/* **THE THREE CALLS OF STEP 5, IN ORDER.** The buffer goes back, the frame is acknowledged, and the view is told it
	 * has been displayed — see this file's header for what omitting the middle one costs. */
	wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(engine->exportable, buffer);
	wpe_view_backend_exportable_fdo_dispatch_frame_complete(engine->exportable);
	wpe_view_backend_dispatch_frame_displayed(engine->backend);
	if (engine->callbacks.damaged != NULL) {
		engine->callbacks.damaged(engine->context, engine);
	}
}

static const struct wpe_view_backend_exportable_fdo_client ag_web_client = {
	NULL,				/* export_buffer_resource — wl_buffer sharing, not used here */
	NULL,				/* export_dmabuf_resource */
	ag_web_export_shm_buffer,
	NULL,
	NULL,
};

/* ----------------------------------------------------------------------------------------------------------------
 * WebKit's signals, in the seam's vocabulary
 * ---------------------------------------------------------------------------------------------------------------- */

static void ag_web_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
	AGWebEngine *engine = data;

	(void)view;
	if (engine->callbacks.load_changed == NULL) {
		return;
	}
	switch (event) {
	case WEBKIT_LOAD_STARTED:
		engine->callbacks.load_changed(engine->context, engine, AGWebLoadStarted);
		break;
	case WEBKIT_LOAD_REDIRECTED:
		engine->callbacks.load_changed(engine->context, engine, AGWebLoadRedirected);
		break;
	case WEBKIT_LOAD_COMMITTED:
		engine->callbacks.load_changed(engine->context, engine, AGWebLoadCommitted);
		break;
	case WEBKIT_LOAD_FINISHED:
		engine->callbacks.load_changed(engine->context, engine, AGWebLoadFinished);
		break;
	default:
		break;
	}
}

static void ag_web_title_changed(GObject *object, GParamSpec *spec, gpointer data)
{
	AGWebEngine *engine = data;
	const char *title;

	(void)spec;
	if (engine->callbacks.title_changed == NULL) {
		return;
	}
	title = webkit_web_view_get_title(WEBKIT_WEB_VIEW(object));
	engine->callbacks.title_changed(engine->context, engine, title != NULL ? title : "");
}

/* ----------------------------------------------------------------------------------------------------------------
 * the seam
 * ---------------------------------------------------------------------------------------------------------------- */

AGWebEngine *AGWebEngineCreate(int width, int height, const AGWebEngineCallbacks *callbacks, void *context)
{
	AGWebEngine *engine;
	WebKitWebViewBackend *view_backend;

	if (!ag_web_library_ready()) {
		return NULL;
	}
	engine = calloc(1, sizeof *engine);
	if (engine == NULL) {
		return NULL;
	}
	if (callbacks != NULL) {
		engine->callbacks = *callbacks;
	}
	engine->context = context;
	engine->width = width > 0 ? width : 1;
	engine->height = height > 0 ? height : 1;

	engine->exportable = wpe_view_backend_exportable_fdo_create(&ag_web_client, engine, (uint32_t)engine->width,
								    (uint32_t)engine->height);
	if (engine->exportable == NULL) {
		fprintf(stderr, "AGWebEngine: the FDO backend refused to make a view backend\n");
		free(engine);
		return NULL;
	}
	engine->backend = wpe_view_backend_exportable_fdo_get_view_backend(engine->exportable);

	view_backend = webkit_web_view_backend_new(engine->backend, NULL, NULL);
	engine->view = webkit_web_view_new(view_backend);
	if (engine->view == NULL) {
		fprintf(stderr, "AGWebEngine: WebKit could not make a web view\n");
		wpe_view_backend_exportable_fdo_destroy(engine->exportable);
		free(engine);
		return NULL;
	}
	g_signal_connect(engine->view, "load-changed", G_CALLBACK(ag_web_load_changed), engine);
	g_signal_connect(engine->view, "notify::title", G_CALLBACK(ag_web_title_changed), engine);
	/* **THE BACKGROUND IS LEFT ALONE, AND THAT IS THE MEASURED CHOICE.** `webkit_web_view_set_background_color` refuses
	 * NULL with a `g_return_if_fail` CRITICAL, and its default is already opaque white — which is what a page with no
	 * background is, and what this seam's caller draws over. Setting it needs a `WebKitColor` built for no gain. */
	return engine;
}

void AGWebEngineDestroy(AGWebEngine *engine)
{
	if (engine == NULL) {
		return;
	}
	/* The view owns neither the exportable nor the view backend; the exportable owns the backend. Destroying the view
	 * first stops its loads, then the backend, then this file's own frame. */
	/* **THE VIEW IS THE ONLY THING THIS FILE OWNED.** The `WebKitWebViewBackend` is TRANSFER FULL into
	 * `webkit_web_view_new` (the header says so, and MEASURED as a SIGSEGV inside `g_object_unref` when this file also
	 * unreffed it), so the view holds it and dropping the view releases it. The view backend itself belongs to the
	 * exportable, which goes second. */
	if (engine->view != NULL) {
		g_object_unref(engine->view);
		engine->view = NULL;
		/* **AND THE MAIN CONTEXT IS DRAINED BEFORE THE BACKEND GOES.** Dropping the view queues teardown work on the
		 * GLib context (WebKit's IPC to its web process, and the renderer host's own shutdown); destroying the
		 * exportable while that is still pending is the race the next few lines exist to avoid. */
		while (g_main_context_pending(NULL)) {
			g_main_context_iteration(NULL, FALSE);
		}
	}
	/* **THE EXPORTABLE IS NEVER DESTROYED BY THIS FILE, AND THAT IS AN OWNERSHIP FACT RATHER THAN AN OVERSIGHT.**
	 * `webkit_web_view_backend_new` takes the `struct wpe_view_backend` with TRANSFER FULL, so WebKit destroys it when
	 * the view is finalised — and `wpe_view_backend_exportable_fdo_destroy` destroys the same object again. MEASURED,
	 * both orders: destroying the exportable after the view and before it each end in a SIGSEGV inside
	 * `wpe_view_backend_destroy`, while leaving it alone lets two engines be made and dropped in one process
	 * (`webengine_teardown` asserts exactly that). So the exportable's handle is CONSUMED by the view: this file keeps
	 * it only while the view is alive, and the small host-side allocation it was is the price of the API's ownership
	 * model. A caller that makes thousands of views will notice; a browser makes them one at a time. */
	free(engine->frame);
	free(engine);
}

int AGWebEngineIterate(AGWebEngine *engine)
{
	int worked = 0;

	if (engine == NULL) {
		return 0;
	}
	/* Bounded so that a page that schedules work continuously cannot starve the caller's own loop: the engine is one
	 * source among several, and a turn here must be a turn. */
	while (worked < 64 && g_main_context_pending(NULL)) {
		g_main_context_iteration(NULL, FALSE);
		worked++;
	}
	return worked > 0;
}

void AGWebEngineLoadURI(AGWebEngine *engine, const char *uri)
{
	if (engine == NULL || engine->view == NULL || uri == NULL) {
		return;
	}
	webkit_web_view_load_uri(engine->view, uri);
}

void AGWebEngineLoadHTML(AGWebEngine *engine, const char *html, const char *base_uri)
{
	if (engine == NULL || engine->view == NULL || html == NULL) {
		return;
	}
	webkit_web_view_load_html(engine->view, html, base_uri);
}

void AGWebEngineSetSize(AGWebEngine *engine, int width, int height)
{
	if (engine == NULL || width <= 0 || height <= 0) {
		return;
	}
	engine->width = width;
	engine->height = height;
	/* **THE VIEW BACKEND IS TOLD, AND IT IS THE ONLY WAY WEBKIT HEARS ABOUT A RESIZE.** The page re-lays out for the
	 * new viewport and paints when it is ready; a caller that resized only its own buffer would stretch the old
	 * layout. */
	wpe_view_backend_dispatch_set_size(engine->backend, (uint32_t)width, (uint32_t)height);
}

const char *AGWebEngineURI(AGWebEngine *engine)
{
	if (engine == NULL || engine->view == NULL) {
		return NULL;
	}
	return webkit_web_view_get_uri(engine->view);
}

const char *AGWebEngineTitle(AGWebEngine *engine)
{
	if (engine == NULL || engine->view == NULL) {
		return NULL;
	}
	return webkit_web_view_get_title(engine->view);
}

double AGWebEngineProgress(AGWebEngine *engine)
{
	if (engine == NULL || engine->view == NULL) {
		return 0.0;
	}
	return webkit_web_view_get_estimated_load_progress(engine->view);
}

void AGWebEngineFrameSize(AGWebEngine *engine, int *width, int *height)
{
	if (width != NULL) {
		*width = engine != NULL ? engine->frame_width : 0;
	}
	if (height != NULL) {
		*height = engine != NULL ? engine->frame_height : 0;
	}
}

int AGWebEngineDraw(AGWebEngine *engine, uint32_t *dst, int dst_stride_bytes, int width, int height)
{
	int rows;
	int columns;
	int y;

	if (engine == NULL || !engine->have_frame || dst == NULL || width <= 0 || height <= 0) {
		return 0;
	}
	rows = engine->frame_height < height ? engine->frame_height : height;
	columns = engine->frame_width < width ? engine->frame_width : width;
	for (y = 0; y < rows; y++) {
		memcpy((unsigned char *)dst + (size_t)y * (size_t)dst_stride_bytes,
		       engine->frame + (size_t)y * (size_t)engine->frame_width,
		       (size_t)columns * 4u);
	}
	return 1;
}
