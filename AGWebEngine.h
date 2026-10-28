/*
 * AGWebEngine.h — a web page as pixels, which is the whole of what a WebView class can be built on.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * WHY A SEAM AND NOT A CLASS. Agora's AppKit draws with the CPU into a `0x00RRGGBB` buffer, so a web view is a
 * rectangle of pixels and an engine that produces them — and which engine is a decision this header does not make. It
 * says what a page IS (a size, a load, a title, a progress, damage, and pixels to draw) and nothing about WebKit,
 * because the day there is a second engine — Servo, or a document renderer for `text/html` that is not a browser —
 * the AppKit must not have to know.
 *
 * **THE PIXELS ARE THE CONTRACT, AND THEY ARE THE WINDOW SEAM'S OWN FORMAT.** `AGWebEngineDraw` writes `0x00RRGGBB`
 * rows, top-down, exactly as `AGWindowBeginFrame` hands them over and exactly as a `CGImage` is made from. A caller
 * therefore has no conversion to make and no orientation to get wrong: present it into a window, or wrap it and draw it
 * in `-drawRect:`, and both are the same memory.
 *
 * **THE ENGINE OWNS A MAIN LOOP AND THE CALLER MUST PUMP IT.** WPE WebKit is GLib: its loads complete, its frames
 * arrive and its process supervision all run on the default `GMainContext`, so an application whose loop is its own
 * must call `AGWebEngineIterate` from it — once a turn is enough, and it never blocks. That is the same shape the
 * window seam already has (`AGWindowServerDispatch`), which is why the AppKit's run loop can drive both and a headless
 * test needs nothing else.
 *
 * THE IMPLEMENTATION THAT EXISTS IS WPE WEBKIT, over `libwpe` + `libwpebackend-fdo`, taking frames through the FDO
 * backend's SHARED-MEMORY export — `wl_shm` pixels with no GPU involved, which is the path a CPU-drawn AppKit wants and
 * the one that works without EGL. Its EGL export gives the same picture as an `EGLImage` for a GPU caller; that is
 * measured (`AGGL` imports it) and is not this door's default.
 *
 * THREADING. One engine, one thread: the engine is created, iterated, drawn and destroyed on the thread whose GLib
 * context it runs on. WebKit's own processes are its business.
 */

#ifndef AG_WEB_ENGINE_H
#define AG_WEB_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AGWebEngine AGWebEngine;

/* The load's stage, in WebKit's own terms and the era's: a page starts, may be redirected, is committed (the first
 * content is in), and finishes. A caller that only wants "it is ready" watches for `AGWebLoadFinished`. */
typedef enum {
	AGWebLoadStarted = 0,
	AGWebLoadRedirected = 1,
	AGWebLoadCommitted = 2,
	AGWebLoadFinished = 3
} AGWebLoadStage;

/* **EVERY CALLBACK IS OPTIONAL AND EVERY ONE COMES FROM `AGWebEngineIterate`.** `damaged` means "these pixels changed"
 * and is the only one a caller must act on; it is called once per engine frame, and a caller that ignores it simply draws
 * the newest pixels whenever it next paints, which is correct but a frame late.
 *
 * **THE RECTANGLE IS THE WHOLE POINT OF THE CALLBACK BEING FOUR NUMBERS.** WPE hands its host a finished buffer and no
 * damage, so the engine computes it while copying — comparing each pixel with the one already in its frame, which costs a
 * compare per pixel on a pass that was happening anyway. The result is a bounding box in DEVICE pixels with a top-down
 * origin: the same space `AGWebEngineDraw` writes and the same space `AGWindowDamage` takes. A view uses it to invalidate
 * a rectangle instead of its whole self, which at 1280x800 is the difference between repainting 4 MB and repainting the
 * 40x40 a hover changed.
 *
 * The first frame, a resize and a scroll are the whole frame, and that is honest rather than pessimistic: those really
 * did change everything. */
typedef enum {
	AGWebCursorInvalid = 0,		/* nothing reported yet, or this engine cannot report */
	AGWebCursorArrow,
	AGWebCursorCrosshair,
	AGWebCursorHand,		/* links, buttons, anything `cursor: pointer` */
	AGWebCursorIBeam,		/* selectable text and editable fields */
	AGWebCursorBusy,
	AGWebCursorHelp,
	AGWebCursorResizeEast,
	AGWebCursorResizeNorth,
	AGWebCursorResizeNorthEast,
	AGWebCursorResizeNorthWest,
	AGWebCursorResizeSouth,
	AGWebCursorResizeSouthEast,
	AGWebCursorResizeSouthWest,
	AGWebCursorResizeWest,
	AGWebCursorResizeColumn,
	AGWebCursorResizeRow,
	AGWebCursorMove,
	AGWebCursorNotAllowed,
	AGWebCursorNone,		/* `cursor: none`: the page is drawing its own pointer */
	AGWebCursorOther,		/* a custom image, or a type this enum does not name */
} AGWebCursor;

typedef struct AGWebEngineCallbacks {
	void (*load_changed)(void *context, AGWebEngine *engine, AGWebLoadStage stage);
	void (*title_changed)(void *context, AGWebEngine *engine, const char *title);
	void (*damaged)(void *context, AGWebEngine *engine, int x, int y, int width, int height);
	/* **THE POINTER'S APPEARANCE, ANNOUNCED RATHER THAN POLLED.** WebKit decides what the cursor should be and says so on
	 * a change; this is that moment, handed to the embedder, which is the only party that can move a real pointer. Null on
	 * an engine that cannot report it (the distro's WebKit), in which case nothing here happens. */
	void (*cursor_changed)(void *context, AGWebEngine *engine, AGWebCursor cursor);
} AGWebEngineCallbacks;

/* **HOW THE ENGINE'S FRAMES REACH THIS PROCESS.** Two of WPE's export paths produce the same picture and cost different
 * things:
 *
 *   - `AGWebExportSHM` is shared memory: the engine's pixels arrive as a `wl_shm` buffer and are copied into this seam's
 *     frame. No EGL, no device, no driver — the path a CPU-drawn AppKit wants, and the one that works on a machine with
 *     no GPU at all. It is the default.
 *   - `AGWebExportEGL` is the same frames as `EGLImage`s: the seam imports each one as a texture on an `AGGL` context,
 *     blits it into an offscreen target and reads it back. It exists for a caller that already composites on the GPU (in
 *     which case the readback can go away entirely) and it is measured against the shm path here rather than assumed
 *     better — on this machine the readback makes it SLOWER, and the numbers are in the README.
 *
 * `AGWebExportEGL` is a PREFERENCE and not a promise: a machine with no usable EGL falls back to shared memory, and
 * `AGWebEngineExportPath` is how a caller finds out which one it got.
 *
 * **AND ONE PROCESS GETS ONE OF THEM.** `libwpebackend-fdo` is initialised once, for shared memory or for an EGL
 * display, and never both — so THE FIRST ENGINE IN A PROCESS DECIDES, and a later engine asking for the other path falls
 * back. MEASURED, in both orders. An application that wants the GPU path must ask for it before it makes a shared-memory
 * engine, and `AGWebEngineExportPath` is how it checks what it got. */
typedef enum {
	AGWebExportSHM = 0,
	AGWebExportEGL = 1,
	/* **THE SAME EGL FRAMES, COMPOSITED STRAIGHT INTO A BUFFER THE HOST NAMES** — a window's own dma-buf, through
	 * `AGWebEngineSetPresentDMABUF`. Nothing is read back, so `AGWebEngineDraw` has no pixels to give in this mode (it
	 * answers 0), and the cost of a frame is a GPU blit instead of a megapixel readback: MEASURED at 1280x800, 35 frames a
	 * second read back against the display's own refresh without. */
	AGWebExportEGLPresent = 2
} AGWebExport;

/* Create an engine sized in POINTS, with nothing loaded (the page is blank white, which is what a browser shows).
 * `callbacks` may be NULL and `context` is passed back untouched. Returns NULL when the machine cannot start the engine
 * at all — no `libWPEBackend-fdo`, no WebKit — and the reason is printed to stderr, because a web view that silently
 * never paints is worse than one that refuses. */
AGWebEngine *AGWebEngineCreate(int width, int height, const AGWebEngineCallbacks *callbacks, void *context);

/* The same, choosing the export path. `AGWebEngineCreate` is this with `AGWebExportSHM`. */
AGWebEngine *AGWebEngineCreateWithExport(int width, int height, AGWebExport export,
					 const AGWebEngineCallbacks *callbacks, void *context);

/* **AND THE SAME WITH THE DEVICE THE DESTINATION BUFFERS COME FROM**, which a present-mode engine MUST be told: it draws
 * into a window's dma-buf, and a GL context on another device cannot import it. MEASURED: importing a window's buffer from
 * a surfaceless context on this machine fails, and from outside that is a frame that never arrives. Pass
 * `AGWindowServerNativeGBMDevice(server)` — a `struct gbm_device *`, typed `void *` here as everywhere in this seam — or
 * NULL to let the engine pick its own device (which is right for `AGWebExportSHM` and for an EGL engine whose frames are
 * only read back). */
AGWebEngine *AGWebEngineCreateWithOptions(int width, int height, AGWebExport export, void *gbm_device,
					  const AGWebEngineCallbacks *callbacks, void *context);

/* **WHERE THE NEXT FRAMES ARE DRAWN, IN THE PRESENT MODE.** The descriptor is the destination's — a window's GPU buffer,
 * typically — and it is BORROWED: the caller owns it and keeps it open, and it stays valid for as long as the caller says
 * (a double-buffered window therefore registers its slot once per frame, before the engine is pumped).
 *
 * Returns 0 when this engine is not in a present mode, because the mode is a decision about what the engine produces and
 * not something to change halfway. */
int AGWebEngineSetPresentDMABUF(AGWebEngine *engine, int fd, int width, int height, int stride, unsigned int fourcc,
				unsigned long long modifier);

/* **HOW MANY FRAMES HAVE BEEN COMPOSITED**, which is what a host's loop needs to decide whether it has anything to commit:
 * a frame began, the engine was pumped, nothing arrived, and `AGWindowCancelFrame` gives the slot back. */
unsigned int AGWebEngineFramesPresented(AGWebEngine *engine);

/* **HOW MANY TIMES THIS ENGINE HAS ASKED WEBKIT TO LAY THE PAGE OUT** — one per size that was actually dispatched, which is
 * not the same as one per call to `AGWebEngineSetSize`. A drag is dozens of resize events and a layout is 25-30 ms, so the
 * engine keeps ONE layout in flight and remembers the newest size; MEASURED, a 20-step drag asks for 8 layouts instead of 20.
 * This door exists so a caller (and a test) can see that, rather than inferring it from a stopwatch. */
unsigned int AGWebEngineLayoutCount(AGWebEngine *engine);

/* **WHICH PATH THIS ENGINE ACTUALLY TOOK**, as a name a diagnostic can print: `"shm"`, `"egl"`, or `"none"` for an
 * engine that is not running. A caller that asked for EGL and fell back needs to be able to see that rather than infer it
 * from a frame rate. */
const char *AGWebEngineExportPath(AGWebEngine *engine);
void AGWebEngineDestroy(AGWebEngine *engine);

/* **PUMP THE ENGINE, AND NEVER BLOCK.** Returns 1 when it did any work, 0 when there was nothing to do; a caller's loop
 * calls it every turn either way. This is where loads complete and frames arrive. */
int AGWebEngineIterate(AGWebEngine *engine);

/* Load a URL (`http`, `https`, `file`, `data`) or a string of HTML. Both are asynchronous: the answer is the callbacks.
 * The base URI may be NULL, and is what relative links and `about:blank` use. */
void AGWebEngineLoadURI(AGWebEngine *engine, const char *uri);
void AGWebEngineLoadHTML(AGWebEngine *engine, const char *html, const char *base_uri);

/* **THE OUTPUT'S DEVICE PIXELS PER POINT, WHICH IS WHAT MAKES TEXT SHARP.** Without it the engine renders at 1x and the
 * view draws that bitmap into a 1.332x surface, so every glyph is resampled: correct, and soft. With it, WebKit lays the
 * page out for the same number of POINTS and paints into that many more pixels. `factor <= 0` returns it to 1.
 *
 * It is a separate door from the size because the two answer different questions — how much page there is, and how many
 * pixels each of its points owns — and a caller may set either alone. */
void AGWebEngineSetDeviceScaleFactor(AGWebEngine *engine, double factor);

/* **AND THE PAGE'S SCROLLBARS ARE THE DESKTOP'S, WHICH IS A CHOICE THIS SEAM MAKES FOR EVERY CALLER.** A page's bars are
 * drawn by the web process, and this port's are Adwaita OVERLAY bars — invisible until something scrolls — so the engine
 * installs a user style sheet that redraws them as the AppKit's `NSScroller`: fifteen points wide with a #d9d9d9 groove,
 * a white knob edged in #bcbcbc and an arrow button at each end. It also ends the overlay behaviour, because styling
 * `::-webkit-scrollbar` makes the bar part of the page's LAYOUT — so a page lays out fifteen points narrower than it did
 * with overlays, which is exactly what a scroll view with a scroller does.
 *
 * IT IS NOT A DOOR, because there is nothing for a caller to decide: a web view in a window either wears the desktop's
 * bar or wears a foreign one. `AGWEBVIEW_NO_SCROLLBAR_STYLE` in the environment is the way to see the difference, and it is
 * read when the engine is MADE. `webengine-scrollbar` is the test. */

/* **A RESIZE IS THE ENGINE'S TO RELAYOUT**, which is why it is a door and not a property: WebKit re-lays out the page
 * for the new viewport and paints when it is ready, and a caller that only resized its own buffer would keep drawing
 * the old layout stretched. Sizes are DEVICE pixels — `round(points * AGWindowServerScale(server))`, the number a
 * window's buffer is. */
void AGWebEngineSetSize(AGWebEngine *engine, int width, int height);

/* What is loaded: the URL after any redirect, the title the page set, and WebKit's own estimate of the load's progress
 * in [0, 1]. Any of the three may be NULL/0 before the first commit, which is a state and not an error. */
const char *AGWebEngineURI(AGWebEngine *engine);
const char *AGWebEngineTitle(AGWebEngine *engine);
double AGWebEngineProgress(AGWebEngine *engine);

/* **HISTORY, WHICH IS WHAT A BACK BUTTON IS MADE OF.** WebKit keeps a back/forward list that `AGWebEngineLoadURI` appends
 * to, so these four doors are all a browser chrome needs: two questions and two commands. "Can" is asked SEPARATELY from
 * doing it because a button has to be greyed out rather than do nothing — `GoBack` on an empty history is a no-op that
 * leaves a person pressing a dead button, which reads as a bug in the app rather than as the end of the history.
 *
 * The answers are WebKit's own (`webkit_web_view_can_go_back` and its forward twin); nothing is remembered here. */
/* **NAVIGATION MAY BE SUPPRESSED FOR THE DURATION OF A SELECTION.** WebKit asks its embedder before it follows a link, and
 * this embedder's answer is "yes" — except while an emulated drag is in flight, because that drag is a series of
 * SHIFT-CLICKS and a shift-click that lands on a link is a navigation. A drag that crosses a link must not open it; that
 * was measured as the risk of the emulation before links worked at all, and it is real as soon as they do. The view turns
 * this on when its drag starts and off when the button is released. */
void AGWebEngineSetSuppressNavigation(AGWebEngine *engine, int suppressed);

/* **WHAT THE POINTER SHOULD LOOK LIKE, WHICH IS THE PAGE'S DECISION AND NOT A HIT TEST.** A page says `cursor: pointer`
 * on a `<div>`, `not-allowed` on a control, `text` in a `contenteditable`, `none` on a video it is drawing itself — and all
 * of those are indistinguishable from an ordinary element by hit testing alone. WebKit computes the cursor; this seam
 * carries it out to the embedder, which is the only party that can change a real pointer.
 *
 * `AGWebEngineCursorSupported` is false against a WebKit without the reporting API (the distro's 2.48, for instance), and
 * then `AGWebEngineCursor` answers `AGWebCursorInvalid` for ever: a caller keeps its default pointer rather than guessing.
 * `webengine-cursor` is the test, and it SKIPS on such a build rather than failing. */

int AGWebEngineCursorSupported(AGWebEngine *engine);
AGWebCursor AGWebEngineCursor(AGWebEngine *engine);

int AGWebEngineCanGoBack(AGWebEngine *engine);
void AGWebEngineGoBack(AGWebEngine *engine);
int AGWebEngineCanGoForward(AGWebEngine *engine);
void AGWebEngineGoForward(AGWebEngine *engine);

/* ================================================================================================================
 * INPUT — THE OTHER HALF OF BEING A VIEW
 *
 * **SIZES AND POINTS ARE IN POINTS — CSS PIXELS — AND THE FRAME THAT COMES BACK IS IN DEVICE PIXELS.** A view is measured
 * in points on every display, and `AGWebEngineSetDeviceScaleFactor` is what turns those into the pixels the engine
 * renders: at 2.0 a 400x300 engine paints an 800x600 frame, its text is laid out for 400 CSS pixels and drawn at 800, and
 * `AGWebEngineFrameSize` answers 800x600 because that is what a caller holding a buffer needs. The default factor is 1,
 * where the two are the same number.
 *
 * **EVERY COORDINATE HERE IS IN THE ENGINE'S OWN POINTS, WITH (0,0) AT THE PAGE'S TOP LEFT**, which is the same
 * space `AGWebEngineDraw` writes. That is not the AppKit's space: an `NSEvent` arrives in points, so the view that
 * forwards one multiplies by the buffer scale before calling these doors — the same number `AGWindowBeginFrameSized`
 * answers with. A caller that skips that step sends every click to the wrong place on a fractional display, which is
 * the sort of bug that only shows up on the one machine that has one.
 *
 * The modifier mask is `AGModifier…` from `AGWindowServer.h` — the AppKit's own vocabulary, because the events being
 * forwarded are `AGEvent`s — and it is mapped to WPE's different numbering here rather than by every caller.
 * ================================================================================================================ */

/* The pointer moved. `modifiers` may be 0. */
void AGWebEnginePointerMotion(AGWebEngine *engine, int x, int y, uint32_t modifiers, uint32_t time_ms);

/* A pointer button changed state. `button` is the EVDEV code (`AGEvent.button`: 0x110 left, 0x111 middle, 0x112 right)
 * and `pressed` is 1 for down. WebKit tracks the hover state from these and from motion, so a click without a preceding
 * motion lands where the coordinates say and nowhere else.
 *
 * **THE FIRST CLICK A PAGE EVER RECEIVES IS TAKEN BY THE FOCUS, WHICH IS THE BROWSER BEHAVIOUR AND NOT A BUG HERE.**
 * MEASURED on a page whose listeners paint it: with the pointer already hovering, the first press/release pair produced a
 * `mousemove` and a `mousedown`, and the second produced the `click`. A caller that needs a click on an unfocused view —
 * which is what a `WebView` in a window nobody has clicked yet is — sends one to take the focus and the real one after
 * it, exactly as a person would. */
void AGWebEnginePointerButton(AGWebEngine *engine, int x, int y, uint32_t button, int pressed, uint32_t modifiers,
			      uint32_t time_ms);

/* **HOW FAR ONE LINE OF WHEEL IS, IN PAGE POINTS — THE ONE NUMBER THAT DECIDES HOW FAST A PAGE SCROLLS.** A wheel
 * notch is one LINE in this tree's vocabulary (the AppKit's `-deltaY` is 1.0 per notch, and the compositor's
 * `wl_pointer.axis` distance is 15.0), and a line is forty points: `WebCore::Scrollbar::pixelsPerLineStep()`'s own
 * constant, which is what Apple's macOS wheel bridge multiplies a non-precise event's `deltaY` by. It is here rather than
 * in either caller because BOTH need it — the view to turn an `NSEvent` into a distance, and this seam to fall back on it
 * when a caller has only `value120` — and two copies of it would be two scroll speeds.
 *
 * **MEASURED, AND THE BUG IT FIXES WAS A FACTOR OF FORTY.** The view used to hand the AppKit's line count to
 * `AGWebEngineScroll` as if it were a distance, and this seam used to pass that straight to WPE as if it were pixels — so
 * one notch scrolled ONE point, over a fractional display even less (WebKit divides by the device scale factor), and a
 * page moved a pixel per click of the wheel. A factor that big is not a taste; it is a unit error, and it read as
 * "scrolling is way too slow". */
#define AGWebEnginePointsPerWheelLine 40.0

/* **ONE AXIS OF ONE GESTURE**, in PAGE POINTS: `delta` is a distance, positive meaning "scroll down", and one wheel
 * line is `AGWebEnginePointsPerWheelLine` of them. `value120` is the same motion in hundred-and-twentieths (120 per
 * notch) for a device or a caller that has steps and no precise distance, and is consulted only when `delta` is 0.
 * `axis` is `AGAxisVertical` or `AGAxisHorizontal`. The seam flips the sign on its way to WebKit, because WPE's positive
 * value means "scroll up", and multiplies by the device scale factor because WPE's axis is in device pixels; see
 * `AGWebEngine-WPE.c`. */
void AGWebEngineScroll(AGWebEngine *engine, int x, int y, uint32_t axis, double delta, int32_t value120,
		       uint32_t modifiers, uint32_t time_ms);

/* **A KEY, AS THE PAGE WILL SEE IT.** `keysym` is xkb's `XKB_KEY_…` and what the page's `key` is derived from;
 * `hardware_keycode` is the evdev code (`AGEvent.keycode`) and what WebKit hands the page as `keyCode`/`code`; both are
 * needed, because a layout is a mapping between them and a page can ask for either. `pressed` is 1 for down. */
void AGWebEngineKey(AGWebEngine *engine, uint32_t keysym, uint32_t hardware_keycode, uint32_t modifiers, int pressed,
		    uint32_t time_ms);

/* **THE FRAME'S SIZE IN DEVICE PIXELS**, which is the size the last exported frame actually was: `points x factor`, and
 * the number a caller allocating a buffer has to know. It is not the size the engine was asked for once a factor is set —
 * that is in points, and asking this door is how a caller sees the difference. */
void AGWebEngineFrameSize(AGWebEngine *engine, int *width, int *height);

/* **DRAW THE NEWEST FRAME INTO THE CALLER'S PIXELS.** `dst` is `0x00RRGGBB` with a TOP-DOWN origin and
 * `dst_stride_bytes` between rows; the engine's frame lands at (0, 0) and is clipped to `width` x `height`. Returns 1
 * when pixels were written, 0 when there is no frame yet (the engine was just made, or nothing has painted) — a caller
 * that gets 0 leaves whatever was there, which is the blank page's white if it drew that first.
 *
 * Nothing is cleared and nothing is scaled: a caller that wants the page smaller than its frame crops it, and one that
 * wants it at a different size asks the engine for that size. Scaling a rendered page is what a viewport is for. */
int AGWebEngineDraw(AGWebEngine *engine, uint32_t *dst, int dst_stride_bytes, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* AG_WEB_ENGINE_H */
