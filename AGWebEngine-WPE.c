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

#include <dlfcn.h>

/* **THE APPKIT'S MODIFIER BITS, NOT A SECOND SET OF NAMES.** `AGWebEngine.h` documents its mask as the AppKit's, so the
 * mapping from those to WPE's needs both vocabularies in one place — and this file is where they meet. The two number
 * their bits DIFFERENTLY (the AppKit shift is 1, WPE's is 2), which is exactly the sort of thing a seam must absorb
 * rather than pass on. */
#include <AppKit/platform/AGWindowServer.h>

#include <AppKit/platform/AGGL.h>

#include <wpe/wpe.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
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

static void ag_web_push_geometry(AGWebEngine *engine, int size_changed, int scale_changed);

struct AGWebEngine {
	AGWebEngineCallbacks callbacks;
	void *context;

	struct wpe_view_backend_exportable_fdo *exportable;
	struct wpe_view_backend *backend;
	WebKitWebView *view;
	/* **THE GPU HALF, WHEN THE CALLER ASKED FOR IT.** An `AGGL` context is the same one `libagora-app` builds for its own
	 * probe; the engine's exported images are imported into it, blitted, and read back. Null on the shared-memory path,
	 * which is the default and needs none of this. */
	AGGLContext *gl;
	int export_path;		/* AGWebExport, as it actually turned out */
	uint32_t *scratch;		/* the readback, before the diff folds it into `frame` */
	size_t scratch_size;
	/* **THE PRESENT MODE'S DESTINATION.** Borrowed, set by the host before each frame, and used from inside the export
	 * callback — which is the same moment WPE's buffer is alive and the only moment it is legal to draw the frame. */
	int present_fd;
	int present_width;
	int present_height;
	int present_stride;
	unsigned int present_fourcc;
	unsigned long long present_modifier;
	unsigned int frames_presented;
	/* **A FRAME THAT ARRIVED WHILE NO DESTINATION WAS REGISTERED.** The host's loop registers a destination, pumps, and
	 * commits — but WebKit's frame can land in any turn, including one where the host had no free buffer. The AGGL target
	 * keeps the last composited frame, so this flag says "there is something to put into the next destination". */
	int frame_pending;

	/* **WHICH BUTTONS ARE DOWN, IN WPE'S OWN MODIFIER BITS.** This is not how a pointer API usually works — it is how THIS
	 * one works, and it is worth the paragraph because getting it wrong is invisible: MEASURED against WebKit's own source
	 * (2.48, `Shared/libwpe/WebEventFactory.cpp`), a page's `MouseEvent.buttons` is built from
	 * `event->modifiers & wpe_input_pointer_modifier_button1/2/3` — NOT from `event->state` and NOT from `event->button`. So
	 * a host that sets `state` faithfully (as this one did) and leaves the modifier bits at zero produces `buttons === 0` on
	 * every event, which is what a page reads as "no button is being held": no drag-scrolling, no drag handlers, and
	 * `e.buttons === 1` checks in page scripts failing silently. The mask is kept here rather than asked of the caller
	 * because a caller cannot be expected to remember it on every motion, and forgetting it on ONE event is a release as
	 * far as the page is concerned.
	 *
	 * WPE 1.16's header spells these `wpe_input_pointer_modifier_button1`..`button5`; the pointer event struct reserves
	 * the same field for keyboard modifiers, so the two are ORed together.
	 */
	uint32_t pressed_buttons;

	/* **SET WHILE A SELECTION DRAG IS RUNNING**, so the policy handler can refuse a link the drag happens to cross — see
	 * `AGWebEngineSetSuppressNavigation`. Zero means ordinary navigation. */
	int suppress_navigation;

	/* The cursor the page last asked for, kept so `AGWebEngineCursor` answers instantly and agrees with the signal. */
	AGWebCursor cursor;
	int cursor_supported;

	/* **THE SIZE IS IN POINTS AND THE FACTOR IS WHAT MAKES PIXELS.** See `AGWebEngineSetDeviceScaleFactor`: WebKit lays
	 * the page out for these, and paints `points x device_scale` device pixels. */
	int width;
	int height;
	double device_scale;
	uint32_t *frame;	/* the newest exported frame, 0x00RRGGBB, top-down */
	int frame_width;
	int frame_height;
	int have_frame;
	/* **WHAT THE LAST FRAME CHANGED**, in device pixels, top-down: the bounding box of the pixels that differ from what
	 * was there before. Computed during the copy rather than by a second pass, and reset to the whole frame whenever the
	 * buffer is new. */
	int damage_x;
	int damage_y;
	int damage_width;
	int damage_height;
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
		} else {
			state = 1;
		}
	}
	return state == 1;
}

/* **THE SHM EXPORT IS PROCESS-WIDE AND THE EGL ONE IS PER DISPLAY**, which is why they are initialised separately: an
 * engine that never asked for shared memory should not set up its machinery, and the EGL initialisation names the very
 * display the images will be created on. */
static int ag_web_shm_ready(void)
{
	static int state;	/* 0 = not tried, 1 = ready, -1 = failed */

	if (state == 0) {
		if (!wpe_fdo_initialize_shm()) {
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
static void ag_web_merge_frame(AGWebEngine *engine, const uint32_t *source, int width, int height, int source_stride)
{
	uint32_t *grown;
	int x, y;
	int damaged = 0;
	int min_x = width;
	int min_y = height;
	int max_x = -1;
	int max_y = -1;

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
		/* **A NEW BUFFER IS ALL DAMAGE.** Every pixel in it is a pixel that was not there before, and a caller told
		 * otherwise would keep the stale half of its last paint. */
		engine->damage_x = 0;
		engine->damage_y = 0;
		engine->damage_width = width;
		engine->damage_height = height;
		engine->have_frame = 1;
		for (y = 0; y < height; y++) {
			const uint32_t *row = (const uint32_t *)((const unsigned char *)source +
								(size_t)y * (size_t)source_stride);
			uint32_t *out = engine->frame + (size_t)y * (size_t)width;

			for (x = 0; x < width; x++) {
				out[x] = row[x] & 0x00ffffffu;
			}
		}
		return;
	}

	/* **THE DIFF IS FUSED INTO THE COPY, AND IT IS THE ONLY REASON THIS LOOP DOES ANYTHING BUT MEMCPY.** A second pass
	 * would read the frame again for the same answer; a compare on values already in registers costs nothing next to the
	 * memory traffic the copy already pays — measured in the test, which asserts the rectangle rather than the clock. */
	for (y = 0; y < height; y++) {
		const uint32_t *row = (const uint32_t *)((const unsigned char *)source +
							(size_t)y * (size_t)source_stride);
		uint32_t *out = engine->frame + (size_t)y * (size_t)width;

		for (x = 0; x < width; x++) {
			uint32_t value = row[x] & 0x00ffffffu;

			if (value != out[x]) {
				out[x] = value;
				if (!damaged) {
					damaged = 1;
					min_x = max_x = x;
					min_y = max_y = y;
				} else {
					if (x < min_x) {
						min_x = x;
					}
					if (x > max_x) {
						max_x = x;
					}
					if (y < min_y) {
						min_y = y;
					}
					if (y > max_y) {
						max_y = y;
					}
				}
			}
		}
	}
	if (damaged) {
		engine->damage_x = min_x;
		engine->damage_y = min_y;
		engine->damage_width = max_x - min_x + 1;
		engine->damage_height = max_y - min_y + 1;
	} else {
		engine->damage_width = 0;
		engine->damage_height = 0;
	}
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
		ag_web_merge_frame(engine, (const uint32_t *)wl_shm_buffer_get_data(shm),
				   wl_shm_buffer_get_width(shm), wl_shm_buffer_get_height(shm),
				   wl_shm_buffer_get_stride(shm));
		wl_shm_buffer_end_access(shm);
	}
	/* **THE THREE CALLS OF STEP 5, IN ORDER.** The buffer goes back, the frame is acknowledged, and the view is told it
	 * has been displayed — see this file's header for what omitting the middle one costs. */
	wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(engine->exportable, buffer);
	wpe_view_backend_exportable_fdo_dispatch_frame_complete(engine->exportable);
	wpe_view_backend_dispatch_frame_displayed(engine->backend);
	/* **A FRAME THAT CHANGED NOTHING IS NOT ANNOUNCED.** WebKit repaints on its own schedule and some of those renders
	 * are identical to the last; telling a view to repaint for them is the work this whole diff exists to avoid. */
	if (engine->callbacks.damaged != NULL && engine->damage_width > 0 && engine->damage_height > 0) {
		engine->callbacks.damaged(engine->context, engine, engine->damage_x, engine->damage_y,
					  engine->damage_width, engine->damage_height);
	}
}

/* **THE LAST FRAME, FROM THIS ENGINE'S TARGET INTO THE HOST'S BUFFER.** Written as a function of its own because two
 * callers need it: the export callback (a frame just arrived) and `AGWebEngineSetPresentDMABUF` (a destination just
 * arrived). Both orders happen — WebKit does not wait for the host's buffer — and a frame that is composited only on the
 * first of them is a frame lost half the time, which is exactly what MEASURED as a window that stayed white. */
static int ag_web_present_pending(AGWebEngine *engine)
{
	AGGLRenderTarget *destination;
	unsigned int frame_texture;

	if (engine == NULL || engine->gl == NULL || !engine->frame_pending || engine->present_fd < 0) {
		return 0;
	}
	destination = AGGLRenderTargetFromDmabuf(engine->gl, engine->present_fd, engine->present_width,
						 engine->present_height, engine->present_stride, engine->present_fourcc,
						 engine->present_modifier);
	if (destination == NULL) {
		/* **NAMED RATHER THAN SILENT.** A present that cannot import the destination is a frame that never arrives, and
		 * from the host's side that is indistinguishable from a page that stopped painting. The numbers are what the
		 * import needs, so they are what the message carries. */
		fprintf(stderr, "AGWebEngine: the present destination %dx%d stride %d fourcc 0x%08x modifier 0x%llx could "
				"not be imported\n", engine->present_width, engine->present_height, engine->present_stride,
			engine->present_fourcc, engine->present_modifier);
		return 0;
	}
	frame_texture = AGGLTargetTexture(engine->gl);
	if (!AGGLBeginRenderTarget(engine->gl, destination)) {
		AGGLRenderTargetDestroy(engine->gl, destination);
		return 0;
	}
	AGGLBlitTextureFlipped(engine->gl, frame_texture, 0, 0, engine->present_width, engine->present_height);
	AGGLEndRenderTarget(engine->gl);
	AGGLRenderTargetDestroy(engine->gl, destination);
	engine->frame_pending = 0;
	engine->frames_presented++;
	return 1;
}

/* **THE SAME FRAME, AS AN IMAGE, COMPOSITED AND READ BACK.** The exported `EGLImage` is imported on this engine's `AGGL`
 * context, drawn into an offscreen target with the V axis flipped (a producer's frame is top-down and GL's texture row 0
 * is its top), read back into the seam's own format, and handed to the same diff the shared-memory path uses. */
static void ag_web_export_egl_image(void *data, struct wpe_fdo_egl_exported_image *exported)
{
	AGWebEngine *engine = data;
	EGLImageKHR image;
	int width;
	int height;
	unsigned int texture;
	size_t needed;

	if (engine->gl == NULL || exported == NULL) {
		return;
	}
	image = wpe_fdo_egl_exported_image_get_egl_image(exported);
	width = (int)wpe_fdo_egl_exported_image_get_width(exported);
	height = (int)wpe_fdo_egl_exported_image_get_height(exported);
	if (width <= 0 || height <= 0) {
		wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(engine->exportable, exported);
		return;
	}
	texture = AGGLTextureFromImage(engine->gl, image, width, height);
	if (texture == 0) {
		fprintf(stderr, "AGWebEngine: the exported image did not become a texture\n");
		wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(engine->exportable, exported);
		return;
	}
	needed = (size_t)width * (size_t)height * 4u;
	if (needed > engine->scratch_size) {
		uint32_t *grown = realloc(engine->scratch, needed);

		if (grown == NULL) {
			AGGLDeleteTexture(engine->gl, texture);
			wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(engine->exportable, exported);
			return;
		}
		engine->scratch = grown;
		engine->scratch_size = needed;
	}
	if (AGGLBeginFrame(engine->gl, width, height)) {
		AGGLBlitTextureFlipped(engine->gl, texture, 0, 0, width, height);
		AGGLEndFrame(engine->gl);
		AGGLDeleteTexture(engine->gl, texture);
		texture = 0;
		if (engine->export_path == AGWebExportEGLPresent) {
			/* **NO READBACK: THE FRAME GOES FROM THIS TARGET TO THE HOST'S BUFFER ON THE GPU.** It is put there now if a
			 * destination is registered, and otherwise remembered for the next one — see `frame_pending`. */
			engine->frame_pending = 1;
			ag_web_present_pending(engine);
			/* **AND THE DIFF IS NOT COMPUTED, BECAUSE THERE ARE NO PIXELS TO DIFF.** A present-mode engine never reads its
			 * frames back, so it reports the whole frame as damaged: the host knows a frame arrived, and a window that is
			 * showing a page repaints all of it anyway. */
			engine->damage_x = 0;
			engine->damage_y = 0;
			engine->damage_width = width;
			engine->damage_height = height;
			/* **AND `have_frame` STAYS FALSE, BECAUSE THERE ARE NO PIXELS TO DRAW.** The frame lives on the GPU; a caller
			 * asking `AGWebEngineDraw` for it is asking the wrong question in this mode, and answering with the last
			 * thing in `frame` would be a stale picture that looks alive. */
		} else {
			if (needed > 0 && AGGLReadPixels(engine->gl, 0, 0, width, height, engine->scratch, width * 4)) {
				ag_web_merge_frame(engine, engine->scratch, width, height, width * 4);
			}
		}
	}
	if (texture != 0) {
		AGGLDeleteTexture(engine->gl, texture);
	}
	/* The producer's image goes back before the frame is acknowledged, and the view is told last — the same order the
	 * shared-memory path uses, and for the same reason (see this file's header). */
	wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(engine->exportable, exported);
	wpe_view_backend_exportable_fdo_dispatch_frame_complete(engine->exportable);
	wpe_view_backend_dispatch_frame_displayed(engine->backend);
	if (engine->callbacks.damaged != NULL && engine->damage_width > 0 && engine->damage_height > 0) {
		engine->callbacks.damaged(engine->context, engine, engine->damage_x, engine->damage_y,
					  engine->damage_width, engine->damage_height);
	}
}

static const struct wpe_view_backend_exportable_fdo_egl_client ag_web_egl_client = {
	NULL,				/* export_egl_image — the struct form carries the size, so that is the one used */
	ag_web_export_egl_image,
	NULL,				/* export_shm_buffer — not on this path */
	NULL,
	NULL,
};

static const struct wpe_view_backend_exportable_fdo_client ag_web_client = {
	NULL,				/* export_buffer_resource — wl_buffer sharing, not used here */
	NULL,				/* export_dmabuf_resource */
	ag_web_export_shm_buffer,
	NULL,
	NULL,
};

/* ----------------------------------------------------------------------------------------------------------------
 * input
 * ---------------------------------------------------------------------------------------------------------------- */

static uint32_t ag_web_modifiers(uint32_t modifiers)
{
	uint32_t wpe = 0;

	if (modifiers & AGModifierShift) {
		wpe |= wpe_input_keyboard_modifier_shift;
	}
	if (modifiers & AGModifierControl) {
		wpe |= wpe_input_keyboard_modifier_control;
	}
	if (modifiers & AGModifierAlt) {
		wpe |= wpe_input_keyboard_modifier_alt;
	}
	if (modifiers & AGModifierSuper) {
		wpe |= wpe_input_keyboard_modifier_meta;
	}
	return wpe;
}

void AGWebEnginePointerMotion(AGWebEngine *engine, int x, int y, uint32_t modifiers, uint32_t time_ms)
{
	struct wpe_input_pointer_event event;

	if (engine == NULL || engine->backend == NULL) {
		return;
	}
	memset(&event, 0, sizeof event);
	event.type = wpe_input_pointer_event_type_motion;
	event.time = time_ms;
	event.x = x;
	event.y = y;
	/* THE HELD BUTTONS RIDE ALONG, which is what makes a drag a drag to the page — see `pressed_buttons`. */
	event.modifiers = ag_web_modifiers(modifiers) | engine->pressed_buttons;
	wpe_view_backend_dispatch_pointer_event(engine->backend, &event);
}

void AGWebEnginePointerButton(AGWebEngine *engine, int x, int y, uint32_t button, int pressed, uint32_t modifiers,
			      uint32_t time_ms)
{
	struct wpe_input_pointer_event event;

	if (engine == NULL || engine->backend == NULL) {
		return;
	}
	memset(&event, 0, sizeof event);
	/* **A BUTTON EVENT CARRIES THE POSITION TOO, AND IT IS NOT DECORATION.** WebKit's hit test for a click uses this
	 * pair; a button event with stale or zero coordinates clicks whatever is at the top left, which is exactly how a
	 * "clicking does nothing" bug looks from the outside. */
	event.type = wpe_input_pointer_event_type_button;
	event.time = time_ms;
	event.x = x;
	event.y = y;
	/* **WPE'S BUTTON NUMBERS ARE A SMALL ENUM, AND IT IS NOT THE ONE AN EVDEV CODE SUGGESTS.** MEASURED by sending each
	 * number to a page that prints `MouseEvent.button`: WPE 1 is LEFT, **2 is RIGHT and 3 is MIDDLE** — a middle-then-right
	 * order that is neither the evdev codes nor X11's 1/2/3, and every value outside 1..3 arrives at the page as LEFT. So
	 * the AppKit's evdev codes (what every other door in this seam takes) are mapped here, and the first version of this
	 * switch had middle and right SWAPPED, which is exactly the sort of thing only a measurement finds.
	 *
	 * The side and extra buttons have no number in this enum; they are mapped to LEFT because that is what this WebKit does
	 * with a number it does not know, and a named comment is better than a silent surprise. */
	switch (button) {
	case 0x110u:			/* BTN_LEFT */
		event.button = 1;
		break;
	case 0x111u:			/* BTN_MIDDLE */
		event.button = 3;
		break;
	case 0x112u:			/* BTN_RIGHT */
		event.button = 2;
		break;
	case 0x113u:			/* BTN_SIDE — no number in this enum */
	case 0x114u:			/* BTN_EXTRA — likewise */
	default:
		event.button = 1;
		break;
	}

	event.state = pressed ? 1 : 0;
	/* **THE MASK MOVES WITH THE PRESS AND THE RELEASE**, and this event itself carries the new value: a press reports its
	 * own button as down, a release reports it gone, which is the shape a page expects from a real mouse. */
	{
		uint32_t bit = 0;

		switch (event.button) {
		case 1: bit = wpe_input_pointer_modifier_button1; break;
		case 2: bit = wpe_input_pointer_modifier_button2; break;
		case 3: bit = wpe_input_pointer_modifier_button3; break;
		default: break;
		}
		if (pressed) {
			engine->pressed_buttons |= bit;
		} else {
			engine->pressed_buttons &= ~bit;
		}
	}
	event.modifiers = ag_web_modifiers(modifiers) | engine->pressed_buttons;
	wpe_view_backend_dispatch_pointer_event(engine->backend, &event);
}

void AGWebEngineScroll(AGWebEngine *engine, int x, int y, uint32_t axis, double delta, int32_t value120,
		       uint32_t modifiers, uint32_t time_ms)
{
	struct wpe_input_axis_2d_event event;
	/* **A CALLER WITH STEPS AND NO DISTANCE STILL GETS ONE LINE'S WORTH PER NOTCH.** `value120` is
	 * hundred-and-twentieths of a notch, and a notch is `AGWebEnginePointsPerWheelLine` points — the same constant a
	 * caller passing a distance already used, which is why it is a constant and not a second 40 written here. */
	double distance = delta != 0.0 ? delta
		: (double)value120 * AGWebEnginePointsPerWheelLine / 120.0;
	/* **AND THE WIRE IS IN DEVICE PIXELS, WHICH IS THE ONE UNIT THIS SEAM DOES NOT SPEAK.** WebKit divides both the
	 * position and the 2-D smooth axis by the device scale factor on its way to CSS pixels, so a distance in POINTS has
	 * to be multiplied by it here — exactly as the caller multiplied a position, and for the same reason. Sending points
	 * through unscaled made every wheel notch a fraction of the distance it should be at a fractional display: the
	 * pointer landed in the wrong place for the SAME defect in the position, measured as a drag selecting the text above
	 * where the person pressed (`AGWebEnginePointerMotion`). */
	double pixels = distance * ((engine != NULL && engine->device_scale > 0.0) ? engine->device_scale : 1.0);

	if (engine == NULL || engine->backend == NULL) {
		return;
	}
	memset(&event, 0, sizeof event);
	/* **THE SIGN FLIPS, AND THAT IS A MEASUREMENT WITH A PAGE'S OWN EARS ON IT.** The AppKit's `delta` is Wayland's
	 * `wl_pointer.axis` value, where positive means "scroll down"; WPE's is the opposite, and a page listening for
	 * `wheel` says so directly — MEASURED, with a listener writing `e.deltaY` into the document title: sending +15
	 * reached the page as `deltaY = -15`. A host that passed the sign through would scroll every document the wrong
	 * way, which is the sort of bug a person reports as "it feels inverted" and nobody can reproduce.
	 *
	 * **AND THE 2-D FORM IS THE ONE THIS HOST USES.** `wpe_input_axis_event` delivers too, but the smooth 2-D pair is
	 * what a scrolling gesture is in WPE's own vocabulary, and a two-axis trackpad gesture has nowhere to go in the
	 * one-axis struct. It is ALSO the only form that keeps the distance: the one-axis struct's `value` is read as a
	 * TICK (±1), which WebKit multiplies by its line step — so a batch of three notches would arrive as one. */
	event.base.type = (enum wpe_input_axis_event_type)(wpe_input_axis_event_type_motion_smooth |
							  wpe_input_axis_event_type_mask_2d);
	event.base.time = time_ms;
	event.base.x = x;
	event.base.y = y;
	event.base.modifiers = ag_web_modifiers(modifiers);
	if (axis == AGAxisHorizontal) {
		event.x_axis = -pixels;
	} else {
		event.y_axis = -pixels;
	}
	wpe_view_backend_dispatch_axis_event(engine->backend, &event.base);
}

void AGWebEngineKey(AGWebEngine *engine, uint32_t keysym, uint32_t hardware_keycode, uint32_t modifiers, int pressed,
		    uint32_t time_ms)
{
	struct wpe_input_keyboard_event event;

	if (engine == NULL || engine->backend == NULL) {
		return;
	}
	memset(&event, 0, sizeof event);
	event.time = time_ms;
	event.key_code = keysym;
	event.hardware_key_code = hardware_keycode;
	event.pressed = pressed != 0;
	event.modifiers = ag_web_modifiers(modifiers);
	wpe_view_backend_dispatch_keyboard_event(engine->backend, &event);
}

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

/* **EVERYTHING THE PAGE ASKS FOR IS ALLOWED, UNTIL SOMETHING SAYS OTHERWISE.** This was written believing a missing handler
 * was why clicks on links did nothing; MEASURED with the handler removed, an ordinary `<a href>` STILL navigates, because
 * WebKit's default for a navigation action is to allow it. That early "links do not work" was the `buttons` bug: a page
 * that never sees a click cannot follow a link, and fixing `MouseEvent.buttons` fixed it.
 *
 * So what this handler is actually for, in the order of who needs it: **a `target="_blank"` link**, which WebKit reports as
 * a NEW_WINDOW_ACTION and then IGNORES by default because there is no window to put it in — here it loads in this view, and
 * that is asserted; **the suppression below**, which has nowhere else to live; and an explicit yes to the `RESPONSE` loads
 * a page starts itself.
 *
 * `NEW_WINDOW_ACTION` is answered by loading in this view rather than being dropped: there are no tabs yet, and a link with
 * `target="_blank"` that does nothing is worse than one that takes over the window it came from. When tabs arrive, this is
 * the branch that makes one. `RESPONSE` covers a load the page started itself (a form, a script's redirect).
 *
 * AND ONE REFUSAL: **a selection drag must not navigate.** The emulated drag is a sequence of shift-clicks, and a
 * shift-click that lands on a link would open it — a person selecting a sentence across a link would leave the page. The
 * view suppresses navigation for the duration of a drag, which is the one case this handler says no. */
static gboolean ag_web_decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision,
				     WebKitPolicyDecisionType type, gpointer data)
{
	AGWebEngine *engine = data;

	(void)view;
	if (engine->suppress_navigation) {
		webkit_policy_decision_ignore(decision);
		return TRUE;
	}
	switch (type) {
	case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION:
	case WEBKIT_POLICY_DECISION_TYPE_RESPONSE:
		webkit_policy_decision_use(decision);
		return TRUE;
	case WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION: {
		/* **`use` DOES NOT PUT A NEW-WINDOW ACTION IN THIS VIEW — IT NEEDS A WINDOW, AND THERE ISN'T ONE.** MEASURED: a
		 * `target="_blank"` link with `webkit_policy_decision_use` did nothing at all, because WebKit got no view to open
		 * (that is the `create` signal's job, and this embedder creates none). So the request is taken out of the decision
		 * and loaded HERE, and the decision is dropped. When tabs arrive, this branch opens a tab instead. */
		WebKitNavigationAction *action;
		WebKitURIRequest *request;
		const char *target;

		action = webkit_navigation_policy_decision_get_navigation_action(
			WEBKIT_NAVIGATION_POLICY_DECISION(decision));
		request = action != NULL ? webkit_navigation_action_get_request(action) : NULL;
		target = request != NULL ? webkit_uri_request_get_uri(request) : NULL;
		if (target != NULL) {
			webkit_web_view_load_uri(view, target);
		}
		webkit_policy_decision_ignore(decision);
		return TRUE;
	}
	default:
		return FALSE;
	}
}

void AGWebEngineSetSuppressNavigation(AGWebEngine *engine, int suppressed)
{
	if (engine == NULL) {
		return;
	}
	engine->suppress_navigation = suppressed ? 1 : 0;
}

/* **THE CURSOR, ANNOUNCED.** The signal carries the fork's `WebKitCursorType`; the mapping to this seam's enum is the same
 * one the getter uses, so both doors agree by construction. `ag_web_cursor_from_platform` is that mapping, in one place. */
static AGWebCursor ag_web_cursor_from_platform(int type)
{
	switch (type) {
	case 1: return AGWebCursorArrow;
	case 2: return AGWebCursorCrosshair;
	case 3: return AGWebCursorHand;
	case 4: return AGWebCursorIBeam;
	case 5: return AGWebCursorBusy;
	case 6: return AGWebCursorHelp;
	case 7: return AGWebCursorResizeEast;
	case 8: return AGWebCursorResizeNorth;
	case 9: return AGWebCursorResizeNorthEast;
	case 10: return AGWebCursorResizeNorthWest;
	case 11: return AGWebCursorResizeSouth;
	case 12: return AGWebCursorResizeSouthEast;
	case 13: return AGWebCursorResizeSouthWest;
	case 14: return AGWebCursorResizeWest;
	case 15: return AGWebCursorResizeColumn;
	case 16: return AGWebCursorResizeRow;
	case 17: return AGWebCursorMove;
	case 18: return AGWebCursorNotAllowed;
	case 19: return AGWebCursorNone;
	case 0: return AGWebCursorInvalid;
	default: return AGWebCursorOther;
	}
}

static void ag_web_cursor_changed(WebKitWebView *view, guint cursor, gpointer data)
{
	AGWebEngine *engine = data;

	(void)view;
	engine->cursor = ag_web_cursor_from_platform((int)cursor);
	if (engine->callbacks.cursor_changed != NULL) {
		engine->callbacks.cursor_changed(engine->context, engine, engine->cursor);
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
	return AGWebEngineCreateWithExport(width, height, AGWebExportSHM, callbacks, context);
}

AGWebEngine *AGWebEngineCreateWithExport(int width, int height, AGWebExport export,
					 const AGWebEngineCallbacks *callbacks, void *context)
{
	return AGWebEngineCreateWithOptions(width, height, export, NULL, callbacks, context);
}

/* **THE FIRST LOAD COST 6.5 SECONDS, AND IT WAS A VBLANK WAIT ON A DISPLAY THIS PROCESS DOES NOT OWN.** MEASURED on this
 * machine, nested on X11 and with WPE WebKit 2.54.2 built locally: `started` (the load's own first stage) arrived 6418 ms
 * after `AGWebEngineLoadURI`, for an EMPTY page and for a `file://` one alike — so not network, not layout, not content.
 * A `gdb` backtrace of the UI process while it waited named the place exactly:
 *
 *   WebPageProxy::loadRequest → launchProcess → finishAttachingToWebProcess → initializeWebPage →
 *   creationParameters → DisplayLink::platformInitialize → DisplayVBlankMonitorDRM::create
 *
 * — and that function ends in `drmWaitVBlank()` on a CRTC that belongs to the X server. WebKit's own escape hatch is
 * `WEBKIT_FORCE_VBLANK_TIMER`, read in `DisplayVBlankMonitor::create`: any value but "0" makes it use a timer at the
 * nominal refresh rate instead of waiting for a display's vertical blank. MEASURED with it set: `started` 256 ms, commit
 * 273 ms, and osnews finished at 678 ms — the 6.5 s is gone, and the same binary that took 7.3 s to load osnews takes
 * 0.68 s.
 *
 * IT IS SET HERE, IN THE ENGINE, because it is a fact about this seam and not about an application: the engine is a CLIENT
 * of Agora's window server and owns no display, so waiting for a vblank it cannot receive is always wrong here. A caller
 * that knows better can set the variable itself — `setenv` with overwrite 0 leaves it alone — including "0" to force the
 * DRM path back on. `webengine-startup` is the regression test: it asserts the first commit arrives inside 1.5 s. */
static void ag_web_want_vblank_timer(void)
{
	if (getenv("WEBKIT_FORCE_VBLANK_TIMER") == NULL) {
		setenv("WEBKIT_FORCE_VBLANK_TIMER", "1", 0);
	}
}

AGWebEngine *AGWebEngineCreateWithOptions(int width, int height, AGWebExport export, void *gbm_device,
					  const AGWebEngineCallbacks *callbacks, void *context)
{
	AGWebEngine *engine;
	WebKitWebViewBackend *view_backend;
	int use_egl = 0;

	ag_web_want_vblank_timer();
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
	engine->present_fd = -1;
	engine->device_scale = 1.0;
	engine->width = width > 0 ? width : 1;
	engine->height = height > 0 ? height : 1;

	/* **THE GPU PATH IS ATTEMPTED FIRST WHEN IT WAS ASKED FOR, AND ABANDONED QUIETLY WHEN IT CANNOT RUN.** A machine with
	 * no EGL — or with an EGL that cannot import a dma-buf — is not a failure of the engine: the frames are the same
	 * pixels either way, and `AGWebEngineExportPath` is how the caller finds out which one it got. */
	if (export == AGWebExportEGL || export == AGWebExportEGLPresent) {
		/* **THE DESTINATION'S OWN DEVICE WHEN THE CALLER NAMED ONE.** A present-mode engine draws into a window's dma-buf,
		 * and a context on any other device cannot import it. */
		AGGLContext *gl = (gbm_device != NULL) ? AGGLContextCreateWithGBM(gbm_device) : AGGLContextCreate(NULL);
		void *display = NULL;

		if (gl != NULL) {
			AGGLContextHandles(gl, &display, NULL, NULL);
			if (display != NULL && wpe_fdo_initialize_for_egl_display((EGLDisplay)display)) {
				engine->gl = gl;
				use_egl = 1;
			} else {
				AGGLContextDestroy(gl);
			}
		}
	}
	if (use_egl) {
		if (getenv("AG_WEBVIEW_DIAG") != NULL) {
			AGGLInfo info;

			AGGLContextInfo(engine->gl, &info);
			fprintf(stderr, "AGWebEngine: EGL %s / %s (gbm=%d wayland=%d)\n", info.vendor, info.renderer,
				info.gbm_display, info.wayland_display);
		}
		engine->exportable = wpe_view_backend_exportable_fdo_egl_create(&ag_web_egl_client, engine,
									       (uint32_t)engine->width,
									       (uint32_t)engine->height);
		engine->export_path = (export == AGWebExportEGLPresent) ? AGWebExportEGLPresent : AGWebExportEGL;
	} else {
		if (export == AGWebExportEGL || export == AGWebExportEGLPresent) {
			/* **ONE EXPORT PER PROCESS, AND THE FIRST ENGINE DECIDES.** `libwpebackend-fdo` is initialised ONCE — for
			 * shared memory or for an EGL display, not both — so a process that has already made a shm engine cannot
			 * have an EGL one. MEASURED: the second of the two in one process falls back, whatever order they are asked
			 * for in. The message says so, because "EGL is unavailable" is wrong and would send the next person looking
			 * at their driver. */
			fprintf(stderr, "AGWebEngine: the EGL export is unavailable — no AGGL context, or this process already "
					"initialised libwpebackend-fdo for another export. Falling back to shared memory.\n");
		}
		if (!ag_web_shm_ready()) {
			free(engine);
			return NULL;
		}
		engine->exportable = wpe_view_backend_exportable_fdo_create(&ag_web_client, engine,
									   (uint32_t)engine->width,
									   (uint32_t)engine->height);
		engine->export_path = AGWebExportSHM;
	}
	if (engine->exportable == NULL) {
		fprintf(stderr, "AGWebEngine: the FDO backend refused to make a view backend\n");
		AGGLContextDestroy(engine->gl);
		free(engine);
		return NULL;
	}
	engine->backend = wpe_view_backend_exportable_fdo_get_view_backend(engine->exportable);
	ag_web_push_geometry(engine, 1, 1);

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
	g_signal_connect(engine->view, "decide-policy", G_CALLBACK(ag_web_decide_policy), engine);
	/* **THE SIGNAL IS LOOKED UP FIRST, BECAUSE THE DISTRO'S WEBKIT DOES NOT HAVE IT.** Connecting to a signal that does not
	 * exist is a GLib warning per engine, which would be noise on every machine without the fork; the lookup is one call
	 * and its absence is the same information `AGWebEngineCursorSupported` reports. */
	if (g_signal_lookup("cursor-changed", G_OBJECT_TYPE(engine->view)) != 0) {
		g_signal_connect(engine->view, "cursor-changed", G_CALLBACK(ag_web_cursor_changed), engine);
		engine->cursor_supported = 1;
	}
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
	if (engine->gl != NULL) {
		AGGLContextDestroy(engine->gl);
		engine->gl = NULL;
	}
	free(engine->scratch);
	engine->scratch = NULL;
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

/* **THE TWO NUMBERS WEBKIT NEEDS ARE PUSHED TOGETHER**, because each is meaningless without the other: a size in points
 * says how much page there is, and the factor says how many pixels a point owns. */
/* **ONLY WHAT CHANGED IS SENT, WHICH IS CORRECTNESS RATHER THAN SPEED.** `set_device_scale_factor` with the value WebKit
 * already has is a view-state change, and WebKit answers a view-state change by laying the page out again — so re-sending
 * it on every resize would be asking for work nobody wanted. A/B MEASURED over twenty resizes with a settle between each:
 * medians of 22.3 ms and 20.0 ms, i.e. a difference inside this measurement's own noise (±2 ms run to run). The change is
 * kept for the reason above and NOT claimed as a speed-up. */
static void ag_web_push_geometry(AGWebEngine *engine, int size_changed, int scale_changed)
{
	if (engine == NULL || engine->backend == NULL) {
		return;
	}
	if (scale_changed) {
		wpe_view_backend_dispatch_set_device_scale_factor(engine->backend, (float)engine->device_scale);
	}
	if (size_changed) {
		wpe_view_backend_dispatch_set_size(engine->backend, (uint32_t)engine->width, (uint32_t)engine->height);
	}
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
	ag_web_push_geometry(engine, 1, 0);
}

void AGWebEngineSetDeviceScaleFactor(AGWebEngine *engine, double factor)
{
	if (engine == NULL) {
		return;
	}
	engine->device_scale = (factor > 0.0) ? factor : 1.0;
	ag_web_push_geometry(engine, 0, 1);
}

const char *AGWebEngineExportPath(AGWebEngine *engine)
{
	if (engine == NULL) {
		return "none";
	}
	switch (engine->export_path) {
	case AGWebExportEGL:
		return "egl";
	case AGWebExportEGLPresent:
		return "egl-present";
	default:
		return "shm";
	}
}

int AGWebEngineSetPresentDMABUF(AGWebEngine *engine, int fd, int width, int height, int stride, unsigned int fourcc,
				unsigned long long modifier)
{
	if (engine == NULL || engine->export_path != AGWebExportEGLPresent) {
		return 0;
	}
	if (fd < 0 || width <= 0 || height <= 0 || stride <= 0) {
		engine->present_fd = -1;
		return 0;
	}
	engine->present_fd = fd;
	engine->present_width = width;
	engine->present_height = height;
	engine->present_stride = stride;
	engine->present_fourcc = fourcc;
	engine->present_modifier = modifier;
	/* **A FRAME THAT ALREADY ARRIVED GOES OUT NOW.** Without this the host's very first destination is filled by the
	 * next frame instead of the one waiting, and a page that paints once looks like a page that never painted. */
	ag_web_present_pending(engine);
	return 1;
}

unsigned int AGWebEngineFramesPresented(AGWebEngine *engine)
{
	return (engine != NULL) ? engine->frames_presented : 0;
}

const char *AGWebEngineURI(AGWebEngine *engine)
{
	if (engine == NULL || engine->view == NULL) {
		return NULL;
	}
	return webkit_web_view_get_uri(engine->view);
}

/* **THE CURSOR IS ASKED FOR BY NAME, NOT LINKED AGAINST.** `webkit_web_view_get_cursor_type` exists only in a WebKit built
 * from the Agora fork (it is not upstream API — see that fork's `agora/README.md`), and this component must keep building
 * and running against the distro's library, where it is absent. `dlsym` is the honest answer: one lookup, cached, and a
 * capability flag a caller can read instead of a link error at load time.
 *
 * The integer values below are that API's `WebKitCursorType`, in its declared order, and they are written out rather than
 * cast for exactly one reason: if the fork ever reorders them, this mapping says so in the code review, and
 * `webengine-cursor` fails on the first semantic mismatch (a hand over a link) instead of quietly showing the wrong arrow. */
static int (*ag_web_cursor_type_fn(void))(void *)
{
	static int resolved;
	static int (*fn)(void *);

	if (!resolved) {
		fn = (int (*)(void *))dlsym(RTLD_DEFAULT, "webkit_web_view_get_cursor_type");
		resolved = 1;
	}
	return fn;
}

int AGWebEngineCursorSupported(AGWebEngine *engine)
{
	if (engine == NULL) {
		return 0;
	}
	return (engine->cursor_supported || (engine->view != NULL && ag_web_cursor_type_fn() != NULL)) ? 1 : 0;
}

AGWebCursor AGWebEngineCursor(AGWebEngine *engine)
{
	int (*fn)(void *) = ag_web_cursor_type_fn();
	int type;

	if (engine == NULL || engine->view == NULL || fn == NULL) {
		return AGWebCursorInvalid;
	}
	type = fn(engine->view);
	return ag_web_cursor_from_platform(type);
}

int AGWebEngineCanGoBack(AGWebEngine *engine)
{
	if (engine == NULL || engine->view == NULL) {
		return 0;
	}
	return webkit_web_view_can_go_back(engine->view) ? 1 : 0;
}

void AGWebEngineGoBack(AGWebEngine *engine)
{
	if (engine == NULL || engine->view == NULL) {
		return;
	}
	webkit_web_view_go_back(engine->view);
}

int AGWebEngineCanGoForward(AGWebEngine *engine)
{
	if (engine == NULL || engine->view == NULL) {
		return 0;
	}
	return webkit_web_view_can_go_forward(engine->view) ? 1 : 0;
}

void AGWebEngineGoForward(AGWebEngine *engine)
{
	if (engine == NULL || engine->view == NULL) {
		return;
	}
	webkit_web_view_go_forward(engine->view);
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
