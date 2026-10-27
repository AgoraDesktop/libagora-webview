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

## Input

Pointer motion, buttons and keys are implemented and each is asserted by a colour the page paints. Two behaviours were
measured rather than assumed, and both are in `AGWebEngine-WPE.c` where they bite:

- **WPE counts buttons from one.** `wpe_input_pointer_event.button` is 1 left, 2 middle, 3 right, so the AppKit's evdev
  code (`0x110`) passed straight through produces a click nobody hears — measured: `0x110` produced neither `mousedown`
  nor `click`, `1` produced both. The seam translates, because every other door in it speaks evdev.
- **The first click a page ever receives is taken by the focus.** Measured: the first press/release pair produced a
  `mousemove` and a `mousedown`, the second produced the `click`. A view in an untouched window therefore sends one click
  to take the focus, exactly as a person does.

**There is no wheel door, deliberately.** It was implemented and removed: `wpe_view_backend_dispatch_axis_event` in both
of WPE's wire forms (the 1-D event and the 2-D `motion_smooth | mask_2d`), on axes 0, 1, `REL_WHEEL` 8 and `REL_HWHEEL`
6, at magnitudes from 15 to 75 000, both directions, against a 1400-point document and against a `div` with
`overflow:auto`, with the pointer inside and the page focused — **no combination moved a pixel**. Keys, motion and
buttons arrive through the same input client, so the gap is specific to the axis path. The next experiment is an
EGL-export host, where WebKit's asynchronous scrolling lives; the matrix is recorded in `AGWebEngine-WPE.c` so nobody
repeats it.

## Tests

| test | what it asks | needs |
|---|---|---|
| `webengine-page` | a real page — red strip above, blue body below — reaches the glass: the **composited screenshot** is asserted, along with all three load stages, the title, and two drawn frames (the first is the blank page) | `window-server` built (else SKIP 77) |
| `webengine-teardown` | two engines can be made and dropped in one process — the lifetime a real application has on `-terminate:` | nothing |
| `webengine-input` | the page hears the pointer (`:hover`), the button (an inline style set by the page's own script) and the keys (a `keydown` listener with no field focused) — each asserted as a colour in the engine's own frame | nothing |

```sh
/usr/bin/clang-19 cmake -S . -B build -DCMAKE_C_COMPILER=/usr/bin/clang-19 \
  -DCMAKE_PREFIX_PATH=../build-prefix -DCMAKE_INSTALL_PREFIX=../build-prefix
cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure
```

`appkit-readme-refs`-style guards are not here yet: this document names no test files, and the two tests are registered
by name.

## What is not here yet, in the order it is worth doing

1. **A `WKWebView`-shaped class** (`NSView` in this component, drawing `AGWebEngineDraw`'s pixels through a `CGImage`,
   driven by the AppKit's run loop calling `AGWebEngineIterate`, forwarding `AGEvent`s through the input doors scaled by
   the buffer scale). The drawing half is the small half; the run-loop integration is the interesting one.
2. **The wheel**, which needs the EGL-export host experiment described above before it can have a door.
3. **Resize under load** and the scale factor (`wpe_view_backend_dispatch_set_device_scale_factor`), so a fractional
   Agora output renders pages at its own device pixels.
4. **The EGL export path**, for a caller that composites on the GPU: the same seam, `AGGLTextureFromImage` instead of a
   memcpy — and the place the wheel is most likely to start working.

## Licence

LGPL-3.0-or-later, like every library in this federation. The engine it hosts is the system's, under its own licences.
