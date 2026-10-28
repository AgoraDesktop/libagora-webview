/*
 * WKWebView.h — a page in a view, which is what the engine seam turns into furniture.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * WHAT THIS IS AND WHAT IT IS NOT. `AGWebEngine` turns a URL into pixels; this is the `NSView` that owns one, hands it
 * the keyboard and the pointer, and draws what it produces in `-drawRect:` through a `CGImage`. It is shaped like
 * WebKit's `WKWebView` because that is the class a person writing against this platform will reach for, and because an
 * API nobody recognises is an API nobody uses.
 *
 * **IT IS NOT WKWebView AND DOES NOT PRETEND TO BE, AND THE DIFFERENCE IS DECLARED RATHER THAN FAKED.** WebKit's class
 * brings a configuration object, a content controller with script message handlers, a back/forward list, process pools,
 * `-evaluateJavaScript:`, a find bar and a download delegate, and every one of those needs a door this tree does not
 * have. So they are ABSENT, not declared-and-refused: a ported program that needs them fails at COMPILE TIME, in the
 * place it asked, rather than at 2 a.m. as a callback that never came. What is here is what the seam can back.
 *
 * THE LOOP. A view cannot sleep in a paint method: the engine has to be pumped for its page to load and for frames to
 * arrive. So the view schedules an `NSTimer` at 60 Hz on the run loop it was created in, calls `AGWebEngineIterate` from
 * it, and marks itself needing display when the engine says a frame is ready. Everything else about the view is
 * ordinary AppKit — `-drawRect:` draws, `-setNeedsDisplay:` decides when.
 *
 * THE PIXELS. The engine produces `0x00RRGGBB`, top-down, in DEVICE pixels (`round(points x the window's buffer
 * scale)`); the view keeps one image of that size and redraws the whole view when it changes. There is no partial
 * damage yet and no scrolling of the view itself: a page scrolls INSIDE the engine, which is where its scrollbars,
 * its fixed positioning and its `:hover` live. A caller that wants the page smaller than the view changes the frame.
 */

#ifndef AG_WK_WEB_VIEW_H
#define AG_WK_WEB_VIEW_H

#import <AgoraWebView/AGWebEngine.h>
#import <AppKit/NSView.h>
#import <AppKit/NSEvent.h>

NS_ASSUME_NONNULL_BEGIN

@class NSCursor;
@class WKWebView;
@class NSURL;
@class NSURLRequest;
@class NSError;

/* **THE NAVIGATION DELEGATE, WHICH IS THE WHOLE OF WHAT THIS TIER CAN TELL A CALLER.** WKWebView's protocol has
 * sixteen doors — policy decisions, redirects, challenges, process termination — and each of those describes a
 * mechanism this seam does not expose. Three describe things it does: a load started painting content, a load finished,
 * and a load failed. A program that conforms to this protocol and later moves to a fuller WebKit will find its methods
 * still called and more of them available. */
@protocol WKNavigationDelegate <NSObject>
@optional
- (void)webView:(WKWebView *)webView didCommitNavigation:(nullable id)navigation;
- (void)webView:(WKWebView *)webView didFinishNavigation:(nullable id)navigation;
- (void)webView:(WKWebView *)webView didFailNavigation:(nullable id)navigation withError:(NSError *)error;
@end

@interface WKWebView : NSView
{
@private
	/* THE ENGINE, OR NOTHING UNTIL SOMETHING IS LOADED. A view that is never used must not start a web process: the
	 * engine spawns one, and a preferences window with a help pane that is never opened should cost nothing. */
	void *_engine;
	void *_keyboard;
	id _pump;
	void *_image;
	unsigned char *_pixels;
	int _pixelWidth;
	int _pixelHeight;
	/* **WHAT THE ENGINE SAYS CHANGED SINCE THE LAST PAINT**, unioned in DEVICE pixels because that is where the engine
	 * speaks, and turned into a view rectangle when the view is asked to redraw. `_damageRect` is that rectangle as it was
	 * last reported, which is what `-damageRect` answers. */
	int _damage_x;
	int _damage_y;
	int _damage_width;
	int _damage_height;
	NSRect _damageRect;
	BOOL _dirty;
	BOOL _loading;
	CGFloat _scale;
	/* **WHAT HAS ACTUALLY BEEN PUSHED TO THE ENGINE**, so the geometry can be re-derived cheaply on every turn and
	 * pushed only when it really changed — see `-ag_syncGeometry`. */
	/* **WHEN THE GEOMETRY LAST CHANGED AND WHEN A FRAME AT THAT GEOMETRY FIRST ARRIVED**, so the delay a person feels
	 * during a resize is a number rather than an impression. */
	double _geometry_changed_ms;
	double _geometry_answered_ms;
	/* **WHEN AND WHERE THE LAST DRAG-EXTENSION WENT OUT.** The point matters as much as the time: two presses at
	 * (nearly) the same place inside the double-click interval are a DOUBLE click, and three are a triple — which selects a
	 * word and then a paragraph. MEASURED: presses 1-3 pixels apart selected 263 characters where the same drag with six
	 * points between presses selected 30. */
	double _last_drag_extend_ms;
	NSCursor *_cursor;		/* the pointer over the page, as the page asked for it */
	AGWebCursor _cursor_kind;
	/* **THE AREA THAT MAKES THE PAGE HEAR THE POINTER WITHOUT THE APPLICATION ASKING FOR MOVES.** A window's
	 * `-acceptsMouseMovedEvents` is NO by default — Apple's, and Agora follows it — so a view that needs to know where the
	 * pointer is must install an `NSTrackingArea` with `NSTrackingMouseMoved`, exactly as Apple's own documentation says and
	 * as WebKit does in its own embedders. Without this the engine never learns what is under the pointer, so a page's
	 * `:hover` is dead AND the cursor it would ask for is never asked for — MEASURED on this tree's own demo, where a page
	 * that was one big link sent the compositor nothing but the default arrow. Kept to be removed again; see
	 * `-updateTrackingAreas`. */
	NSTrackingArea *_pointerArea;
	CGFloat _drag_last_x;
	CGFloat _drag_last_y;
	BOOL _drag_extending;
	int _pushed_points_width;
	int _pushed_points_height;
	double _pushed_scale;
	NSString *_title;
	NSString *_uri;
	NSString *_pendingHTML;
	NSString *_pendingBase;
	NSString *_lastURI;
	id _navigationDelegate;
}

/* **THE FRAME INITIALISER, AND THE VIEW IS NOT OPTIONAL**: nothing is drawn until a page is loaded, and a page is
 * loaded into a view that exists. The engine is created on the first load, sized to this view at the window's buffer
 * scale — which is why a view created before its window is sized again when it gets one. */
- (instancetype)initWithFrame:(NSRect)frameRect;

/* **ASSIGNED, AS APPLE'S IS**: a delegate that is retained would keep its owner alive, and a view's owner is usually
 * the object the delegate lives in. */
- (nullable id<WKNavigationDelegate>)navigationDelegate;
- (void)setNavigationDelegate:(nullable id<WKNavigationDelegate>)delegate;

/* Load a URL, or a string of HTML with a base URL for its relative links. Both return nothing: a navigation object is
 * WebKit's bookkeeping and this tier has none, so a caller that needs to correlate an answer with a request compares
 * the URL in the delegate or reads `-URL`. */
- (void)loadRequest:(NSURLRequest *)request;
- (void)loadHTMLString:(NSString *)string baseURL:(nullable NSURL *)baseURL;

/* **RELOAD, WHICH IS THE LAST THING THE VIEW WAS ASKED FOR.** The engine has no reload door, so this re-issues the
 * request the view remembers — the URI, or the HTML string and its base. It is the same page either way, and it is
 * what a person pressing Command-R expects. */
- (void)reload;

/* The page's own answers, live: the title it set, the URL after redirects, WebKit's estimate of the load's progress in
 * [0, 1], and whether a load is in flight. All of them answer something sensible before the first load. */
- (nullable NSString *)title;
- (nullable NSURL *)URL;
/* **HISTORY, WHICH IS WHAT A BACK BUTTON IS MADE OF.** The engine keeps WebKit's own list; these four are the whole of
 * what a browser chrome needs, and "can" is asked separately from doing it so a button can be greyed out rather than
 * pressed into a no-op. */
/* **THE POINTER OVER THE PAGE IS THE PAGE'S DECISION**, carried out by the view as a cursor rect: `ag_cursor` is what the
 * engine last reported (nil when the engine cannot report, or before the page has set one), and `-resetCursorRects` hands
 * it to the AppKit, which resolves it against the pointer as it moves. A caller that wants to know why its arrow changed
 * can read `-ag_cursorName` — the CSS-shaped name, for logs and tests. */
- (void)ag_refreshCursor;
- (void)ag_takeCursor:(AGWebCursor)cursor;
- (nullable NSString *)ag_cursorName;

- (BOOL)canGoBack;
- (void)goBack;
- (BOOL)canGoForward;
- (void)goForward;
- (double)estimatedProgress;
- (BOOL)isLoading;

/* The engine's frame size in DEVICE pixels, which is what a caller comparing "the size I asked for" with "the pixels I
 * got" needs on a fractional display. */
- (NSSize)frameSizeInPixels;

/* **THE LAST FRAME'S CHANGED RECTANGLE, IN VIEW COORDINATES** — the union of everything that changed since the previous
 * paint, which is exactly what the view told AppKit to redraw. A hover is a 40x40 box and a scroll is the whole view; a
 * caller that wants to know which is which reads this, and a test uses it to prove the view is not repainting 4 MB to
 * show a button's highlight. */
- (NSRect)damageRect;

@end

NS_ASSUME_NONNULL_END

#endif /* AG_WK_WEB_VIEW_H */
