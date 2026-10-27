# libagora-webview — a web page as pixels

**Status: working, minimal.** A page loaded through `AGWebEngine` reaches an Agora window as `0x00RRGGBB` pixels, and a
test asserts the composited screenshot of a real page. There is no `NSView` yet and no input: this component is the
*engine host*, and the class that a person types into is the next tier.

## What it is, and why it is not part of the AppKit

Agora's AppKit draws with the CPU into shared memory (`AGWindowBeginFrame`). A web view, on that model, is a rectangle
of pixels plus something that produces them — and *which* engine produces them is a decision no part of Cocoa's contract
needs to make. So the engine lives here, behind `AGWebEngine.h`, and the AppKit stays what it was:

```
AGWebEngineCreate(width, height, callbacks, context)   → an engine, sized in DEVICE pixels
AGWebEngineLoadURI / AGWebEngineLoadHTML               → a page; both asynchronous
AGWebEngineIterate                                     → pump it, once a turn, never blocking
AGWebEngineDraw(engine, dst, stride, w, h)             → the newest frame, in the window's own format
AGWebEngineSetSize                                     → a viewport change, which the page relayouts for
AGWebEngineURI / Title / Progress                      → what is loaded
AGWebEnginePointerMotion / PointerButton / Key         → input, in the engine's device pixels
AGWebEngineScroll                                      → one axis of one gesture, sign already flipped
AGWebEngineDestroy                                     → and see "Teardown", below
```

`AGWebEngineDraw` writes `0x00RRGGBB`, top-down, exactly as `AGWindowBeginFrame` hands it over and exactly as a `CGImage`
is made from — so a caller presents it into a window, or wraps it and draws it in `-drawRect:`, with no conversion and
no orientation to get wrong.

## The engine: WPE WebKit over `libwpe` + `libwpebackend-fdo`

Both are **system libraries** on this project's own terms (Wayland, xkbcommon, ICU, libcurl): WPE WebKit 2.48 is
LGPL-2.1, `libwpe` and `libwpebackend-fdo` are BSD-2, and none is vendored or pinned here. Packages, on Debian trixie:

```sh
apt install libwpe-1.0-dev libwpebackend-fdo-1.0-dev libwpewebkit-2.0-dev
```

## Two export paths, and which one wins

| path | what it is | measured (a page animating at 60 Hz) |
|---|---|---|
| `AGWebExportSHM` (default) | `wl_shm` pixels, copied into the seam's frame — no EGL, no device | **62 fps** at 320x200, **59-62 fps** at 1280x800 |
| `AGWebExportEGL` | the same frames as `EGLImage`s, imported on an `AGGL` context, blitted (V flipped) and **read back** | **62 fps** at 320x200, **35 fps** at 1280x800 |

So the GPU path is SLOWER for a host that wants pixels, and saying otherwise would be a lie with a number behind it: the
readback of a megapixel costs about 13 ms a frame, and the shared-memory path's memcpy costs a fraction of that. EGL's
value is for a caller that *does not* read back — one that composites the engine's texture itself — and that door does not
exist yet (`AGGL` has the pieces; nothing in this component draws into a window's GPU buffer).

**AND ONE PROCESS GETS ONE OF THEM.** `libwpebackend-fdo` is initialised once, for shared memory or for an EGL display,
so the FIRST engine decides; a later engine asking for the other path falls back and says so. Measured in both orders.

Frames arrive through the FDO backend's **shared-memory** export — `wl_shm` pixels, no EGL, no device, no driver — which
is the path a CPU-drawn AppKit wants and the one that works on a machine with no GPU at all. Its EGL export produces the
same picture as an `EGLImage` for a GPU caller; `libagora-app`'s `AGGL` imports one, and that is measured (see that
component's README), but it is not this door's default.

## The sequence, because every step of it was measured

Written down in `AGWebEngine-WPE.c` in the order it has to happen. The three findings worth repeating:

- **`wpe_loader_init("libWPEBackend-fdo-1.0.so")` is not optional.** Without it `libwpe` looks for
  `libWPEBackend-default.so` when WebKit asks for a renderer host client and the process **aborts** the first time a page
  loads.
- **`wpe_view_backend_exportable_fdo_dispatch_frame_complete` is the call that is easy to miss.** Without it the host
  receives exactly **one** frame for ever: the blank white pre-load frame, after a load that reported all three stages.
  With it, the second frame is the page. `…dispatch_release_shm_exported_buffer` then `frame_complete` then
  `wpe_view_backend_dispatch_frame_displayed`, in that order.
- **The exportable is consumed by the view.** `webkit_web_view_backend_new` takes the `struct wpe_view_backend` with
  transfer full, so `wpe_view_backend_exportable_fdo_destroy` destroys it a second time — measured as a SIGSEGV inside
  `wpe_view_backend_destroy`, in both orders. This file therefore never calls it; that is a deliberate acceptance of a
  small host-side leak per engine, not an oversight.

The web process is **sandboxed** (bubblewrap) and needs no environment variable here; the `WEBKIT_DISABLE_SANDBOX…`
escape hatch is deliberately unused by this component and its tests.

## The GPU presenter, and where it stops

`AGWebExportEGLPresent` composites the page straight into a destination the host names — no readback at all. The whole
composition, from doors that exist for it:

```c
AGWindowBeginFrameDMABUF(window, &dst);              /* the window's slot, as a descriptor */
AGWebEngineSetPresentDMABUF(engine, dst.fd, ...);    /* the engine draws into it on the GPU */
AGWebEngineIterate(engine);                          /* WebKit's frame arrives and lands */
AGWindowEndFrame(window);                            /* the compositor samples it */
/* or, when no frame arrived: AGWindowCancelFrame(window); */
```

**AND A WINDOW'S BUFFER WORKS TOO — GPU TO GLASS.** MEASURED, in four steps, each one a change somewhere:

1. A window's GPU buffer allocated through **GBM** cannot be rendered into at all: `glEGLImageTargetTexture2DOES` answers
   `GL_INVALID_OPERATION` on the same device, in the same context. The seam now allocates those buffers through **EGL**
   (`AGGLTextureAllocate` + `EGL_MESA_image_dma_buf_export`), which imports and renders perfectly — the difference is the
   ALLOCATOR, not the device — and keeps GBM as the fallback.
2. Both of a window's slots are allocated **together**, at the resize: this driver refuses to export a second texture once
   the first buffer is being imported and rendered into elsewhere, so a lazily-made second slot fell back to GBM and the
   path collapsed.
3. A frame that arrives in a turn where the host has no free buffer is **kept** and flushed into the next one
   (`AGWebEngineSetPresentDMABUF` composites a pending frame; WebKit does not wait for the host's buffer).
4. **The last one was in the compositor**, and it looked like our bug: the page arrived with red and blue exchanged. The GL
   renderer uploads shared-memory surfaces from `ARGB8888` bytes and swaps `.bgr` in the shader to put red back — and it
   applied the same shader to a dma-buf, which EGL has ALREADY mapped onto GL's channels. The shader now takes the channel
   order as a uniform (`u_bgr`), set from how the texture was made: 1 for an upload, 0 for an import. The page is red
   above and blue below, which is what `webengine-window` asserts on the composited screenshot.

A destination this seam exports from its own EGL composites perfectly — `webengine-gpu` asserts the page's red band at the
top of that buffer, read back by the TEST and not by the engine (which is refused pixels of its own, so a pass cannot come
from a hidden readback). A window's buffer needed all four steps above; with them, `webengine-window` — nested, opt-in,
asserted on the composited screenshot — passes too.

## Partial damage

WPE hands its host a finished buffer and no damage, so the seam computes it **while copying**: each pixel is compared
with the one already in the frame, and the bounding box of the differences is what the `damaged` callback carries. A pass
that was happening anyway, one compare per pixel, and the rectangle is what a view needs to repaint a band instead of a
surface.

`WKWebView` unions that rectangle (in device pixels), converts it to view coordinates — a divide by the buffer scale and a
y flip — and calls `-setNeedsDisplayInRect:`. MEASURED in `appkit-webview`: a hover on a 40-point full-width box damages
**320x40 at (0,120)**, where before it damaged the whole view, and the first frame's damage is (correctly) everything.

## The class: `WKWebView`

`WKWebView` is an `NSView` that owns an engine: it loads, it draws the engine's frame in `-drawRect:` through a `CGImage`,
and it forwards the pointer, the buttons, the keys and the wheel. The engine is pumped by an `NSTimer` at 60 Hz on the run
loop the view was created in — the engine's work is asynchronous, so the damage callback only marks the view and AppKit
decides when a paint happens.

```
WKWebView(frame)
loadRequest: / loadHTMLString:baseURL: / reload
title / URL / estimatedProgress / isLoading / frameSizeInPixels
navigationDelegate  →  webView:didCommitNavigation: / didFinishNavigation: / didFailNavigation:withError:
```

**WHAT IT DELIBERATELY DOES NOT HAVE, AND WHY THAT IS NOT AN OVERSIGHT.** WebKit's `WKWebView` brings a configuration
object, a content controller with script message handlers, a back/forward list, process pools, `-evaluateJavaScript:`, a
find bar and downloads — every one of which needs a door this tree does not have. They are **absent, not
declared-and-refused**: a ported program that needs them fails at compile time, where it asked, instead of at 2 a.m. as a
callback that never came. What is here is what the seam can back.

Two things the class cost, both measured and both recorded at the line that pays for them:

- **`CGContextDrawImage` needs NO flip here.** Its first row goes to the top of the destination rectangle in user space,
  and the view's space is unflipped — so the page lands upright with no transform. The first version flipped the CTM
  "because the image is top-down" and drew every page upside down; the test caught it because it measures which way a
  bitmap rep's rows run before it calls anything "the top".
- **Every frame is copied, every time.** The first version returned early once an image existed — "the engine draws into
  it below" — and nothing ever did: the view showed the first frame it saw, for ever. A page that loads and then never
  changes its pixels is exactly what that looks like.

## Input

Pointer motion, buttons and keys are implemented and each is asserted by a colour the page paints. Two behaviours were
measured rather than assumed, and both are in `AGWebEngine-WPE.c` where they bite:

- **WPE counts buttons from one.** `wpe_input_pointer_event.button` is 1 left, 2 middle, 3 right, so the AppKit's evdev
  code (`0x110`) passed straight through produces a click nobody hears — measured: `0x110` produced neither `mousedown`
  nor `click`, `1` produced both. The seam translates, because every other door in it speaks evdev.
- **The first click a page ever receives is taken by the focus.** Measured: the first press/release pair produced a
  `mousemove` and a `mousedown`, the second produced the `click`. A view in an untouched window therefore sends one click
  to take the focus, exactly as a person does.

**The wheel works, and it took three measurements to say so** — the first two of which were wrong:

- **WPE's positive axis value means "scroll up".** The seam's `delta` is `wl_pointer.axis`'s, where positive means down,
  so the host negates on the way through. A page listening for `wheel` says which way is which: with a listener writing
  `e.deltaY` into `document.title`, sending `+15` reached the page as **`deltaY = -15`**. Passing the sign through would
  scroll every document backwards — a bug a person reports as "it feels inverted" and nobody can reproduce.
- **AND THE FIRST TWO TESTS PROVED NOTHING, WHICH IS WORTH RECORDING.** The wheel was "broken" for an hour because the
  probe sampled the middle of a 100-point colour band: 60 points of scroll stayed inside it, so a working wheel looked
  dead. The replacement (a 24-point marker) then gave two false negatives of its own — the document could not scroll far
  enough to reveal it, and then the scroll went past it. What is in the test now is a **200-point band 200 points below
  the fold**, which is on screen for any scroll between 80 and 280: the claim is "the document moved", and the assertion
  is sized to the gesture rather than to an imagined one.

## Tests

| test | what it asks | needs |
|---|---|---|
| `webengine-page` | a real page — red strip above, blue body below — reaches the glass: the **composited screenshot** is asserted, along with all three load stages, the title, and two drawn frames (the first is the blank page) | `window-server` built (else SKIP 77) |
| `webengine-teardown` | two engines can be made and dropped in one process — the lifetime a real application has on `-terminate:` | nothing |
| `appkit-webview` | the CLASS, offscreen: the page arrives **upright** (the rep's own orientation is measured first), the title is the page's, and the pointer and the wheel reach the page through the view's own y-flip and scale | nothing |
| `webengine-gpu` | the **present mode**: the page composited into a GPU buffer with no readback — the destination is read back by the test, and the engine is asserted to have no pixels of its own | EGL |
| `webengine-window` | the same, into a real **window's** own buffer, asserted on the composited screenshot — nested and opt-in (`AGORA_ALLOW_NESTED=1`) | EGL + a compositor that imports dma-buf |
| `webengine-egl` | the same page through the **EGL export**: imported, blitted flipped, read back — asserted as a red band at the top and blue below, because a flipped blit or a channel swap is a frame that is not the page. SKIPS when the machine's EGL cannot import a dma-buf | EGL |
| `webengine-input` | the page hears the pointer (`:hover`), the button (an inline style set by the page's own script), the keys (a `keydown` listener with no field focused) and the wheel (a band below the fold comes into view) — each asserted as a colour in the engine's own frame | nothing |

```sh
/usr/bin/clang-19 cmake -S . -B build -DCMAKE_C_COMPILER=/usr/bin/clang-19 \
  -DCMAKE_PREFIX_PATH=../build-prefix -DCMAKE_INSTALL_PREFIX=../build-prefix
cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure
```

`appkit-readme-refs`-style guards are not here yet: this document names no test files, and the two tests are registered
by name.

## What is not here yet, in the order it is worth doing

1. **Resize under load** and the scale factor (`wpe_view_backend_dispatch_set_device_scale_factor`), so a fractional
   Agora output renders pages at its own device pixels.
2. **Input in the present mode** — nothing about it is mode-specific, but nothing has asserted it either.

## Licence

LGPL-3.0-or-later, like every library in this federation. The engine it hosts is the system's, under its own licences.
