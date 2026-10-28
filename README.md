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

## What a person found that the tests did not

Two symptoms reported from the running demo, and both were real:

- **Resizing the window stretched the page instead of reflowing it.** `NSWindow` resizes its content view through
  `-setFrame:`; `WKWebView` overrode only `-setFrameSize:`, so the engine kept its birth size and `CGContextDrawImage`
  scaled the old frame into the new bounds. The class now overrides both, and re-derives its geometry on every pump and
  every paint.
- **The page was soft by default.** The device scale factor was read ONCE, in `-viewDidMoveToWindow` — and MEASURED with a
  diagnostic on the demo application, that door sees **1.000** while the first pump sees **1.332**. The stored factor
  stayed 1.0 for the life of the view, so the page was rendered at 1x and resampled into a 1.332x surface. The factor is
  now asked of the window every time, and pushed only when it changes.

- **And a resize showed the page stretching before it reflowed.** The frame on screen while WebKit lays the page out again is
  the PREVIOUS size, and `-drawRect:` drew it into the NEW bounds — a bitmap squashed to fit. A stale frame is now drawn at
  the size it was painted, anchored top-left, with the page's background where the window has grown; and the new frame is
  requested in the same pass as the resize rather than at the next timer turn. MEASURED in `appkit-webview`: growing the
  view by 160 points and drawing immediately leaves the page's own pixels untouched and the new width white — drawing the
  stale frame into the full bounds instead turns that white area blue, which is the bug.

`webview-resize` runs the class in a **real window at `--scale 2`** and asserts the outcome both ways: 400x300 points gives
an 800x600 frame, and a window grown to 520x378 gives 1040x756. Disabling the fix reproduces the reported bug exactly — the
frame stays 800x600 while the view grows, which is a page stretched rather than re-laid out.

## The demo: a page on the desktop

```sh
demo/agora-webview.sh                       # the built-in page
demo/agora-webview.sh https://example.org   # a URL
demo/agora-webview.sh --session 960x720     # plus Workspace and the Dock
```

`tools/webview.m` is the application: an `NSApplication`, an `NSWindow`, a `WKWebView` filling it, and a URL or a built-in
page. It draws through the **AppKit** path (readback → `CGImage` → `-drawRect:`), because a view inside a CPU-drawn window
with other views in it is that shape; the GPU presenter is a whole-window composition and belongs to a different door.

**ONE THING IT TAUGHT THE CLASS:** a hand-pumped loop that runs the run loop and not `-[NSApplication updateWindows]` never
calls `-drawRect:` at all — measured as an application that loaded its page, printed the right title, and painted nothing.
`webview-app` asserts the page's own colours in the composited frame, so that failure cannot come back quietly.

## Input in the present mode

Nothing about input is mode-specific — the same four doors dispatch to the same view backend — but nothing had asserted it
either. `webengine-gpu` now sends a pointer motion and a key in the GPU-present mode and reads the **destination's** pixels
for the two colours the page paints, so "input works when the engine is not reading its frames back" is a measurement and
not an assumption.

**And it found a test bug worth recording**: the first version of that page set its base colours with `style="…"` attributes,
and an inline style beats any `:hover` rule — so the hover painted nothing, in either mode, and looked like a broken door.

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
| `webview-app` | the **demo application**: AppKit paints the view into a window's buffer and the compositor shows the page — nested and opt-in, asserted on the composited screenshot | a compositor |
| `webengine-window` | the same, into a real **window's** own buffer, asserted on the composited screenshot — nested and opt-in (`AGORA_ALLOW_NESTED=1`) | EGL + a compositor that imports dma-buf |
| `webengine-egl` | the same page through the **EGL export**: imported, blitted flipped, read back — asserted as a red band at the top and blue below, because a flipped blit or a channel swap is a frame that is not the page. SKIPS when the machine's EGL cannot import a dma-buf | EGL |
| `webengine-input` | the page hears the pointer (`:hover`), the button (an inline style set by the page's own script), the keys (a `keydown` listener with no field focused) and the wheel (a band below the fold comes into view) — each asserted as a colour in the engine's own frame | nothing |
| `webengine-scrollbar` | the page's bars are the AppKit's scroller — fifteen points of #d9d9d9 groove with a white knob edged in #bcbcbc and arrow buttons of black triangles — that they take their points out of the LAYOUT, that the other axis gets one too, that a page which paints its own bar loses to the desktop's, and that `AGWEBVIEW_NO_SCROLLBAR_STYLE` puts WebKit's overlay bars back | nothing |

```sh
/usr/bin/clang-19 cmake -S . -B build -DCMAKE_C_COMPILER=/usr/bin/clang-19 \
  -DCMAKE_PREFIX_PATH=../build-prefix -DCMAKE_INSTALL_PREFIX=../build-prefix
cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure
```

`appkit-readme-refs`-style guards are not here yet: this document names no test files, and the two tests are registered
by name.

## Input is in device pixels, sizes are in points

The two are deliberately opposite, and getting it backwards is a bug a person finds straight away. Sizes are points and the
device scale factor turns them into pixels; `wpe_input_pointer_event`'s coordinates are in the surface's **device pixels** and
WebKit divides them by the factor itself. When this seam moved sizes to points it also stopped scaling the pointer, and at
scale 2 a pointer at window y=120 reached the page as `clientY=90` instead of 180 — half the distance, which is why a drag
selected the text ABOVE where the person pressed.

The measurement is now a test: `webview-resize` sends a pointer at a known window position on a `--scale 2` output and the
page reports the CSS pixel it saw.

```
resize: window y=120 -> view y=120.0 -> the page says clientY=180 (want 180.0)
  ok   the page sees the pointer where the window put it
```

## Selecting text: a build option, a modifier bit, and a click count

Three separate things were tangled together here, and two of them were blamed on WebKit before measurement said otherwise.

### `MouseEvent.buttons` was our bug

A page reads a held button from `MouseEvent.buttons`, and WebKit builds that mask from the pointer event's **`modifiers`**
— `wpe_input_pointer_modifier_button1/2/3` — not from `wpe_input_pointer_event.state`. That is not guesswork: it is
`pressedMouseButtons()` in WebKit 2.48's own `Source/WebKit/Shared/libwpe/WebEventFactory.cpp`, which says so in comments
quoting the UI Events spec. This embedder set `state` faithfully and left the modifier bits at zero for a long time, so
every page saw `buttons === 0` throughout every drag: drag handlers, drag-scrolling and `if (e.buttons === 1)` checks all
saw a released mouse.

The engine now keeps the held buttons in the mask and ORs them into every event (`pressed_buttons`). `webengine_input`
asserts the page's own view of it:

```
input: with nothing held the page sees "buttons=0 button=0"
input: with the left button held the page sees "buttons=1 button=0"
input: after the release the page sees "buttons=0 button=0"
```

Remove the fix and the middle line fails with `wanted buttons=1`, which is how it was verified.

### Dragging still cannot select — and that is a build option

With `buttons` fixed, a real press-motion-release drag *still* selects nothing. The reason is not a bug in the port but a
compile-time feature: WebKit 2.48's `Source/cmake/WebKitFeatures.cmake` defines

```cmake
WEBKIT_OPTION_DEFINE(ENABLE_DRAG_SUPPORT "Toggle support of drag actions (including selection of text with mouse)" PRIVATE OFF)
```

— **PRIVATE OFF**, and Debian's `debian/rules` never turns it on. The branch that extends a selection on mouse move,
`EventHandler::handleMouseDraggedEvent`, is called from `handleMouseMoveEvent` inside `#if ENABLE_DRAG_SUPPORT`, so in this
build it does not exist.

MEASURED, because "not compiled in" has to be paid for: a press followed by ten motions each carrying `buttons === 1`
selects **nothing** (`sel=0`), while in the same run a plain press places a caret (`anchorOffset=1` in a text node) and the
drag's first motion fires `selectstart`. The press half of the path is intact; only the drag half is missing.

So selection is extended the classic way: the press sets the anchor and a **SHIFT-modified click moves the extent**. A
WebKit built with `-DENABLE_DRAG_SUPPORT=ON` would let the natural path work; nothing here depends on that.

### A local WebKit can do it properly

`~/Development/wpewebkit` builds WPE WebKit 2.54.2 with `ENABLE_DRAG_SUPPORT=ON` (see its `agora/README.md`). Naming that
install does two things that must happen together:

```sh
cmake -S . -B build -DAGORA_WPE_PREFIX=~/Development/wpewebkit/agora/prefix   # tests use it too
AGORA_WPE_PREFIX=~/Development/wpewebkit/agora/prefix demo/agora-webview.sh 1280x720 https://example.com
```

The library path puts the fork first and `AGWEBVIEW_REAL_DRAG=1` switches the view from shift-clicks to a real drag.
MEASURED: that drag selects **29** characters on the fork, the emulation selects **29** on both, and a real drag sent to
the system library selects **0** — so the variable and the library have to travel together, and the demo script and the
test environment both set both.

Why an environment variable rather than a probe: the port offers no runtime answer. `typeof DragEvent` is "function" in
a build with drag support OFF as well as ON (measured), and no settings object exposes the flag, so asking WebKit would
be guessing.

### A double click does select the word

WebKit counts clicks itself — `clickCount()` in the same `WebEventFactory.cpp` compares time and distance (500 ms, 5 px)
with no help from the embedder. Two plain presses on a word select it:

```
appkit_webview: a double click selected 9 character(s) — the page says "Agora web view sel=9"   ("Selection")
```

An earlier note in this file claimed double-click selection could not work because no click count reaches WebKit. That was
wrong: it already worked, and the assertion exists so that a future change to the pointer stream cannot quietly break it.

### The emulation was too aggressive at first

Dragging over a word selected the *whole paragraph*. WebKit counts presses as multi-clicks by time AND distance, and a
drag's first events move one or two pixels — so a press per event made a double click (the word) and then a triple (the
paragraph). MEASURED: presses 1-3 pixels apart selected **263** characters where the same drag with six points between
presses selected **30**. A shift-press is now only sent once the pointer has really moved (six points), and the release
comes once at mouse-up, so the steps cannot become clicks.

### And the button numbers were wrong

WPE's enum is 1 = LEFT, **2 = RIGHT, 3 = MIDDLE** (every other value arrives as LEFT), where this seam had middle and right
swapped. Found by sending each number to a page that prints `MouseEvent.button`; fixed, with the numbers in the comment.

## Links, and what a policy handler is actually for

Clicking a link now navigates, and `webengine-links` asserts it: two pages on disk, a click, the URI changes, Back returns.
What that probe also established, at the cost of a wrong comment: **ordinary links never needed a `decide-policy` handler.**
This file used to say clicks did nothing because none was installed; measured with the handler removed, an `<a href>` still
navigates, because WebKit's default for a navigation action is to allow it. The early "links do not work" was the
`MouseEvent.buttons` bug — a page that never sees a click cannot follow a link.

The handler exists for the two things the default cannot do:

- **`target="_blank"`** is a `NEW_WINDOW_ACTION`, and `webkit_policy_decision_use` on one does *nothing* here — measured —
  because it needs a view to open and this embedder creates none. The handler takes the request out of the decision and
  loads it in this view; that is the honest single-view answer until tabs exist, and the branch that becomes one.
- **A selection drag must not follow the links it crosses.** The emulated drag is a run of shift-clicks, so a drag over a
  link would navigate the moment links work. `AGWebEngineSetSuppressNavigation` covers exactly that window, and the probe
  checks both halves — the suppressed click stays put, and the same click works again once suppression is lifted (a latch
  here would leave the browser unable to follow anything after one drag).

Measured, handler disconnected vs connected: the ordinary link passes both times; the `_blank` step and the suppressed
click fail without it.

## The demo's toolbar: back, forward, and a URL field

`tools/webview.m` puts an `NSToolbar` on the window with two buttons and an editable `NSTextField`. Nothing about it is a
mock: the field is a real one, so typing goes through the AppKit's field editor, Return ends the session and sends the
control's action, and the buttons' own actions are what navigate.

The engine grew four doors for it — `AGWebEngineCanGoBack`/`GoBack`/`CanGoForward`/`GoForward` — wrapping WebKit's own
back/forward list; `WKWebView` exposes them as `-canGoBack`, `-goBack`, `-canGoForward`, `-goForward`. "Can" is asked
separately from doing it so the buttons grey out instead of being pressed into a no-op, and a typed address with no scheme
gets `https://` because that is what people type.

`webview-toolbar` asserts the whole gesture, twice over:

```
ok: the URL field loaded the typed address, and Back returned to the first page
ok: the frame after Back is the first page
```

It runs the real application on a real compositor with `--type-url` and `--back-after`, which make the app act like a hand:
make the field first responder, deliver each character as a real `NSEvent` through `-sendEvent:`, press Return, then
`-performClick:` the Back button. The assertions are the *page's* titles — logged by the demo from the page itself — so
"the typed address loaded" and "back went to the first page" are claims about WebKit, not about the app's own log line;
the composed frame then has to show the first page's colour on the glass.

## The pointer over a page: the page's decision, carried out by the view

`WKWebView` now changes the pointer the way a browser does — a hand over a link, an I-beam over text, a crosshair where a
page asked for one, a "no" sign where it refuses a drop, and nothing where a page draws its own pointer. It is part of the
class, not the demo: `AGWebEngineCallbacks.cursor_changed` carries the value, `-resetCursorRects` hands it to the AppKit as
a cursor rect, and `-ag_cursorName` says which one, for logs and tests.

Three things had to happen, and two of them were wrong the first time.

**The cursor came from the port.** `WPEWebView.h`'s `setCursor` was an empty virtual, so WPE's legacy view discarded the
cursor the page computed. The fork (`~/Development/wpewebkit`) now reports it: `WebKitCursorType`, a getter, and a
`cursor-changed` signal. Hit testing could not have done this — a `pointer` on a plain `<div>`, `not-allowed` on a control
and `text` in a `contenteditable` are indistinguishable there.

**Polling does not work, and the measurements say why.** The first implementation asked for the value after each move. On a
page of five bands it reported the *previous* band, and three seconds of pumping changed nothing; a repeated motion
sometimes helped and sometimes did not; two motions in the same turn never did, and repeating them every 64 ms while a
"pending" flag was set still raced. A probe using `ag_now_ms()` did nothing at all, because its timestamp jumped a million
milliseconds past the event it followed. The signal removed the whole question: the view installs the cursor in the turn
WebKit announces it, **0 ms** in the offscreen test.

**And the view must not release a cursor.** `+[NSCursor arrowCursor]` and its siblings return one shared instance per class,
held by the AppKit; the first version retained the new one and released the old, which deallocated the singleton and left the
class with a dangling pointer — the cursor then stopped changing at all.

**And a cursor is only half the wire: the NAME is the other half, and the view has to ask for the pointer.** An `NSCursor`
carries no pixels here — `-image` is nil for every shared cursor — so what the view installs has to be told to the compositor
by name (`NSCursor -name`, and `AGWindowSetCursor`/`agora_window.set_cursor` in `libagora-app` and `window-server`). Which
means the page's decision reaches the glass only if the engine hears the pointer AT ALL: `-acceptsMouseMovedEvents` is NO by
default on a window, so `-mouseMoved:` never runs, the engine never learns what is under the pointer, and the page's cursor
is never asked for. MEASURED on this tree's own demo before the fix: a page that was one big link sent the compositor
`left_ptr` and nothing else, and not one motion reached the view. So the VIEW installs the `NSTrackingArea`
(`NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect`) rather than leaving it to an embedder that cannot
know — and the page's `:hover` came back with it.

MEASURED end to end afterwards, on `demo/agora-webview.sh` against a page that is one link and with `AWS_CURSOR_DIAG=1`:
three motions reached the view, the engine reported `3 -> pointer`, and the compositor answered
`DIAG cursor: window 1 asks for pointer, which the theme has at 32x32, hotspot (16,4)` — the theme's hand, re-applied to
the host's X cursor. That is the whole pipeline in one log: page → `NSCursor` → name → protocol → artwork.

```
appkit_webview: the cursor over the link is "pointer" after 0 ms, over the prose is "text" after 0 ms
  ok     the pointer over a link is a hand
  ok     the pointer over prose is an I-beam

webengine-cursor: a link -> 3, text -> 4, crosshair -> 2, not-allowed -> 18, none -> 19
```

`webengine-cursor` SKIPS on a WebKit without the fork's API, which is the honest answer there: the component still builds,
runs and keeps its own pointer.

## The wheel moved the page one point per notch, because two units were never converted

A person reported that scrolling in a web view was **way too slow**, and it was a factor of forty: one click of the wheel
moved the page ONE point, and on a fractional display less than that, because WebKit divides the axis by the device scale
factor. A page crawled a pixel at a time.

Two unit errors, one on each side of the door, and neither looked wrong on its own:

- `-[WKWebView scrollWheel:]` handed the AppKit's `-deltaY` — the count of WHEEL LINES, 1.0 per notch — to
  `AGWebEngineScroll` as if it were a distance. The seam's own `AGEvent.delta` is 15.0 per notch, so even the seam's
  unit was off by fifteen.
- `AGWebEngineScroll` passed whatever it got straight to WPE, whose 2-D smooth axis is in **device pixels** (WebKit
  divides it by the device scale factor on its way to CSS pixels). So the number was neither in the seam's unit nor in
  the wire's.

The door now speaks **page points**, which is the unit that file's whole INPUT section already uses for sizes and
positions, and both conversions happen once each: the view turns wheel lines into points, and the seam turns points into
device pixels. One line is `AGWebEnginePointsPerWheelLine` — **forty points**, which is WebCore's own
`Scrollbar::pixelsPerLineStep()` and what Apple's macOS wheel bridge multiplies a non-precise event's `deltaY` by. That
constant is deliberately in the header rather than at either end: the view needs it and the `value120` fallback needs it,
and two copies would be two scroll speeds.

**MEASURED BY THE PAGE, NOT BY THE SCROLL.** A page that scrolls "the right way" says nothing about the distance, so the
probe's page now reports its own `wheel` event's `deltaY` in the title:

```
input: one notch of steps (120) reached the page as "wheel dy=40"
input: two lines of distance reached the page as "wheel dy=80"
input: one line at 2x reached the page as "wheel dy=40"   (the device scale factor is applied, so 2x is not half)

appkit_webview: one notch of wheel reached the page as "Agora web view wheel dy=40" after 20 ms
appkit_webview: after 3 notch(es) the band below the fold is visible
```

**AND THAT TEST WAS WRITTEN AROUND THE BUG.** `appkit_webview` had sent `deltaY = -60` four times to bring a band 280
points down into view — sensible only if a line were a pixel — so it passed on the broken code and would have failed on
the fix. It sends notches now. The same is true of the one-axis WPE form: `wpe_input_axis_event` reads its value as a
TICK and multiplies by its own line step, which would have been correct for a single notch and wrong for a coalesced
batch of three, so the 2-D smooth form (which carries a distance) stays.

What this does NOT add is wheel **acceleration**: a wheel is one line per notch here, while macOS applies acceleration and
most X11 toolkits turn a notch into three lines. `AGWebEnginePointsPerWheelLine` is the one knob if a person wants a
fuller page per click.

## The page's scrollbars are the AppKit's scroller now

A page's bars are drawn by the WEB PROCESS — `ScrollbarTheme::nativeTheme()` is `ScrollbarThemeAdwaita` for this port —
and on WPE they are **overlay** bars: `usesOverlayScrollbars()` is `SystemSettings::overlayScrolling().value_or(true)`,
and unlike GTK there is no `GTK_OVERLAY_SCROLLING` override on this platform. So they appear while a page is being scrolled
and fade away, and a page at rest has no bar at all. MEASURED on a nested session with a 3300-point document: two seconds
after a scroll the page's own colour ran to its last column and there was nothing to see.

They are restyled through a **user style sheet**, which needs no fork change at all: `webkit_web_view_get_user_content_manager`
is public in WPE, so it is two calls on the view the engine already made.

**THE NUMBERS ARE THE APPKIT'S, AND THE COPY IS THE ONE COUPLING WORTH KNOWING ABOUT.** This component is a layer below the
AppKit and links none of it, so it cannot ask `+[NSScroller scrollerWidth]` or read a colour; the values are copies of
`libagora-app/NSColor.m`'s doors with their sources named in `AGWebEngine-WPE.c`, and the probe measures the drawn result
rather than trusting either side:

| the bar | the AppKit's door |
|---|---|
| 15 points thick | `+[NSScroller scrollerWidth]` |
| groove `#d9d9d9` | `+cg_scrollerTrackColor` (0.85) |
| knob white on `#bcbcbc`, radius 3 | `+controlBackgroundColor` over `+controlDarkShadowColor` (0.737), `NS_SCROLLER_BAR_RADIUS` |
| one arrow at each end, black triangle 5.2 by 3.5 points | `NSScrollerCell`'s arrow, `NS_SCROLLER_ARROW_HALF`/`…_HEIGHT` of a 15-point button |

**TWO THINGS ABOUT WEBKIT'S CUSTOM SCROLLBARS ARE NOT OBVIOUS.** Its theme puts TWO buttons at each end
(`ScrollbarButtonsDoubleBoth`), and `:single-button` is a question an author can ask and not a choice one can make — it
matches whatever placement the theme already has. So the second of each pair is dropped with `display: none`, which also
gives its fifteen points back to the groove: MEASURED before that, the bar was up-down-knob-groove-up-down. And the
triangles are inline SVG, because CSS has no triangle primitive and the button pseudo-element takes only an image.

**STYLING `::-webkit-scrollbar` ALSO ENDS THE OVERLAY BEHAVIOUR, AND THAT IS THE POINT RATHER THAN A SIDE EFFECT.**
WebKit's `usesLegacyScrollbarStyle` makes the bar part of the layout (`LocalFrameView.cpp:1677`), so it keeps its fifteen
points out of the viewport and stays on screen — what an `NSScroller` does in a scroll view, and the reason a person can
see where they are without touching the wheel. The reflow is the honest cost, and it is the same one every desktop browser
had before overlay bars.

MEASURED in `webengine-scrollbar`, on the engine's own frame: the bar's column holds `#d9d9d9` ×149, `#ffffff` ×43, the
knob's `#bcbcbc` borders, the arrow ink, and it is IDENTICAL a second and a half later; the page's colour stops at x=304 of
320; a page wider than the viewport gets the same treatment along the bottom; and a page that declares its own
`::-webkit-scrollbar { width: 30px; background: #ff0000 }` gets the desktop's fifteen points instead, with no red anywhere
— the user level "always override[s] other style sheets", as WebKit's own header says. `AGWEBVIEW_NO_SCROLLBAR_STYLE=1`
puts WebKit's bars back, which is what makes the difference measurable: without the sheet the column holds only tints of
the page's own colour.

The one thing NOT copied is the knob's grip: Agora's scroller draws three dimples down the knob's centre, and at fifteen
points with a thumb that is often shorter than their span a CSS gradient reads as a smudge rather than as a grip.

## The first load took 6.5 seconds, and it was a vblank wait

Building WPE WebKit 2.54.2 locally made every page take seven seconds before anything appeared. The number came from the
view's own reflow timer, so the first guess was layout — and that was wrong in an instructive way.

Measured with a bare engine (no window, no toolbar, no AppKit), timing each load stage:

| | 2.48 (Debian) | 2.54.2 (local) | 2.54.2 + `WEBKIT_FORCE_VBLANK_TIMER=1` |
|---|---|---|---|
| load started | 314 ms | **6418 ms** | 256 ms |
| commit | 319 ms | 6421 ms | 273 ms |
| osnews finished | 921 ms | 7266 ms | **678 ms** |

An **empty page** and a `file://` page cost the same 6.4 s, and the **second** load in the same engine took 13 ms — so it
was neither network nor layout nor content: it was a one-time wait before the load even started. A `gdb` backtrace of the
UI process while it waited named it:

```
WebPageProxy::loadRequest → launchProcess → finishAttachingToWebProcess → initializeWebPage
  → creationParameters → DisplayLink::platformInitialize → DisplayVBlankMonitorDRM::create
```

`DisplayVBlankMonitorDRM::create` ends in `drmWaitVBlank()` on a CRTC the X server owns. This process is a client of Agora's
window server; it has no display to wait on. WebKit's own escape hatch is `WEBKIT_FORCE_VBLANK_TIMER`, read in
`DisplayVBlankMonitor::create`, and the engine sets it (`ag_web_want_vblank_timer`, only when the variable is unset) —
6.5 s → 0.27 s, osnews 7.3 s → 0.68 s.

`webengine-startup` is the regression test: a first load must start inside 1.5 s. Run it with `WEBKIT_FORCE_VBLANK_TIMER=0`
and it reports the stall instead — **6411 ms** measured that way.

**And one test had to change with the fix.** `webengine-page` required *two* damaged frames, on the reasoning that the first
frame is the blank page exported before content commits. That was true of a slow start and false of a fast one: with the
stall gone the first frame is already the finished page, nothing changes afterwards, so the damage callback fires once and
the probe failed while its screenshot was perfect. It now samples the engine's own frame for the page's two colours — a
stronger claim than "a frame happened", and one that does not depend on timing.

## How long a reflow takes, and what did not help

A resize has to wait for WebKit to lay the page out again, and that is the whole of the delay. MEASURED on this machine with
a document of a few thousand points: **median ~20 ms, 26-35 ms immediately after a drag**, and the frame copy the seam does
per frame is 0.36 ms at 900x700 — an order of magnitude less than the layout, so our side is not where the time goes.

Two things were tried and **both measured inside the noise**, so neither shipped:

- **Coalescing resizes** (one layout in flight, newest size wins): 20 resize events asked for 19 layouts anyway, because
  WebKit answers a size change with a frame at the new size quickly enough that the "in flight" gate never held — and the
  gate broke a resize that happened *while a page was loading*. Reverted.
- **A 2 ms pump while a resize is unanswered**, painting the moment the new frame lands: 34.9 ms before, 30-34 ms after —
  inside the ±2 ms run-to-run spread. Removed rather than kept as an unproven second timer.

What is left is the honest part: the resize requests its frame in the same pass as the geometry change, and the wait is
spent showing the previous frame **undistorted** at its own size rather than squashed to fit. A page that is briefly too
small reads as "not laid out yet"; a page bent to fit reads as a rendering bug, which is how it was reported.

## The two numbers a page is laid out with

**Sizes and points are in POINTS; the frame that comes back is in DEVICE PIXELS.** `AGWebEngineSetDeviceScaleFactor` is what
turns one into the other, and it is a door of its own because the two answer different questions: how much page there is,
and how many pixels each of its points owns. MEASURED, in `webengine-scale`:

- a 400x300 engine at factor 2 hands back an **800x600** frame whose 40-POINT band is **80 device rows** — the page laid out
  for 400 CSS pixels and painted at 800, which is the difference between sharp text and text resampled once;
- an engine resized **while its page is still loading**, 640x480 -> 480x360 -> 320x200, ends with a **320x200** frame and a
  band of **40 rows**. Forty, not seventeen: seventeen is what a 640x480 layout rescaled into 320x200 would give, so the
  band's height is the proof that the page re-laid out rather than being stretched.

`WKWebView` passes its window's `-userSpaceScaleFactor` and its own bounds in points, so a page inside an AppKit window is
rendered at the output's own resolution — and its pointer conversion is in points too, because a click at CSS pixel (160,
60) is the same place at 1x and at 2x.

**A warning from this pass, since it cost an hour here:** both claims first "failed" because the test sampled once, right
after the load finished, when the frame can still be the blank page. Polling for the page's own colour is what made them
pass — and it also showed that a geometry push repeated when a page commits changes nothing, so that defensive code was
removed rather than kept with a comment claiming it had fixed something.
