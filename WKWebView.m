/*
 * WKWebView.m — the view, the pump, the image, and the four events a page needs.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * THE FIVE DECISIONS, in the order they constrain each other:
 *
 * 1. **THE ENGINE IS CREATED LAZILY**, on the first load, and destroyed in `-dealloc`. Creating it in `-init` would mean a
 *    view that is made and never shown has already spawned a web process; and a view whose frame is 1x1 at that moment
 *    would have the engine sized to it, because a page lays out for the viewport it was told about and nothing re-lays
 *    out for free.
 *
 * 2. **THE SCALE COMES FROM THE WINDOW, WHEN THERE IS ONE.** `AGWindowBufferScale` is the compositor's own device pixels
 *    per point (1.332 on this machine's panel), and the engine's coordinates, its frame size and every mouse event are in
 *    THOSE pixels. A view with no window uses 1 and is resized when it gets one — `-viewDidMoveToWindow` is where that
 *    happens, and a fractional display is exactly the case where forgetting it puts every click in the wrong place while
 *    the pixels still look right.
 *
 * 3. **THE IMAGE IS PREMULTIPLIED ARGB AND THE ENGINE'S PIXELS ARE NOT.** `AGWebEngineDraw` writes `0x00RRGGBB`; a
 *    `kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little` image whose alpha byte is 0 is INVISIBLE, which is
 *    the failure this file's alpha fix-up exists to avoid. MEASURED: drawing the engine's buffer straight through gives a
 *    transparent image and a view that paints nothing at all — which looks exactly like a broken engine.
 *
 * 4. **THE VIEW IS UNFLIPPED AND THE PIXELS ARE TOP-DOWN.** AppKit's origin is the bottom-left and the engine's is the
 *    top-left, so `-drawRect:` flips the CTM before drawing the image and the mouse conversion flips y. One of those two
 *    being right and the other wrong is a page that draws upright and clicks upside down, which is why the test asserts
 *    BOTH — a colour at the bottom of the page, and a hover that needs the pointer's y to be converted.
 *
 * 5. **THE PUMP IS A TIMER, AND THE DAMAGE CALLBACK ONLY MARKS.** `AGWebEngineIterate` runs on the run loop, never
 *    inside `-drawRect:`: WebKit's load and its frames are asynchronous, and a view that iterated while painting would
 *    be a view that re-enters its own drawing. So the engine's damage callback sets a flag, the timer marks the view
 *    needing display, and AppKit decides when a paint happens — the same shape the rest of this tree's views have.
 */

#import <AgoraWebView/WKWebView.h>

#import <AppKit/NSGraphicsContext.h>
#import <AppKit/NSColor.h>
#import <AppKit/NSWindow.h>
#import <AppKit/NSEvent.h>
#import <AppKit/platform/AGKeyboard.h>
#import <AppKit/platform/AGWindowServer.h>

#import <AgoraWebView/AGWebEngine.h>

#import <Foundation/NSAutoreleasePool.h>
#import <Foundation/NSURL.h>
#import <Foundation/NSURLRequest.h>
#import <Foundation/NSError.h>
#import <Foundation/NSTimer.h>
#import <Foundation/NSRunLoop.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AG_WEBVIEW_PUMP_INTERVAL 0.016

/* **THE MODIFIER MASK, IN THE SEAM'S VOCABULARY.** `AGEvent.modifiers` is `AGModifier…`; `NSEvent`'s flags are Apple's
 * different bits, and the two agree on nothing but their meaning. */
static uint32_t ag_view_modifiers(NSUInteger flags)
{
	uint32_t modifiers = 0;

	if (flags & NSShiftKeyMask) {
		modifiers |= AGModifierShift;
	}
	if (flags & NSControlKeyMask) {
		modifiers |= AGModifierControl;
	}
	if (flags & NSAlternateKeyMask) {
		modifiers |= AGModifierAlt;
	}
	if (flags & NSCommandKeyMask) {
		modifiers |= AGModifierSuper;
	}
	return modifiers;
}

static double ag_now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
}

@interface WKWebView ()
- (void)ag_ensureEngine;
- (void)ag_pump:(NSTimer *)timer;
- (void)ag_engineDamagedInPixelsX:(int)x y:(int)y width:(int)width height:(int)height;
- (void)ag_flushDamage;
- (void)ag_engineLoadStage:(int)stage;
- (void)ag_syncGeometry;
@end

/* **THE ENGINE'S CALLBACKS, WITH THE VIEW AS CONTEXT.** They arrive from `AGWebEngineIterate` on the run loop's own
 * thread, which is the only thread this class knows about; the ObjC half does nothing but set flags and tell a delegate. */
static void ag_view_load_changed(void *context, AGWebEngine *engine, AGWebLoadStage stage);
static void ag_view_damaged(void *context, AGWebEngine *engine, int x, int y, int width, int height);
static void ag_view_title_changed(void *context, AGWebEngine *engine, const char *title);

static void ag_view_load_changed(void *context, AGWebEngine *engine, AGWebLoadStage stage)
{
	WKWebView *view = (WKWebView *)context;

	(void)engine;
	[view ag_engineLoadStage:(int)stage];
}

static void ag_view_damaged(void *context, AGWebEngine *engine, int x, int y, int width, int height)
{
	WKWebView *view = (WKWebView *)context;

	(void)engine;
	[view ag_engineDamagedInPixelsX:x y:y width:width height:height];
}

/* The title is read from the engine when a paint happens rather than kept here: the engine owns it, and a second copy
 * that could disagree with the first is a bug waiting for a slow page. */
static void ag_view_title_changed(void *context, AGWebEngine *engine, const char *title)
{
	(void)context;
	(void)engine;
	(void)title;
}

@implementation WKWebView

- (instancetype)initWithFrame:(NSRect)frameRect
{
	self = [super initWithFrame:frameRect];
	if (self == nil) {
		return nil;
	}
	_scale = 1.0;
	_dirty = YES;
	return self;
}

- (void)dealloc
{
	/* **THE TIMER GOES FIRST, AND IT IS THE ONE THING THAT CAN OUTLIVE THIS OBJECT.** A timer holds a reference to its
	 * target, so a view that released itself with a scheduled timer would be a view whose `-ag_pump:` runs after its own
	 * dealloc — the classic crash of this pattern. */
	[_pump invalidate];
	[_pump release];
	_pump = nil;
	/* The engine after the timer that pumps it, and before the pixels it wrote. */
	if (_engine != NULL) {
		AGWebEngineDestroy((AGWebEngine *)_engine);
		_engine = NULL;
	}
	if (_keyboard != NULL) {
		AGKeyboardDestroy((AGKeyboard *)_keyboard);
		_keyboard = NULL;
	}
	if (_image != NULL) {
		CGImageRelease((CGImageRef)_image);
		_image = NULL;
	}
	free(_pixels);
	_pixels = NULL;
	[_title release];
	[_uri release];
	[_pendingHTML release];
	[_pendingBase release];
	[_lastURI release];
	[super dealloc];
}

/* ----------------------------------------------------------------------------------------------------------------
 * the engine
 * ---------------------------------------------------------------------------------------------------------------- */

- (void)ag_ensureEngine
{
	if (_engine != NULL) {
		return;
	}
	{
		AGWebEngineCallbacks callbacks;
		NSRect bounds = [self bounds];
		/* **POINTS, NOT PIXELS**: the factor is what turns them into pixels, and it is handed over on the next line. */
		int width = (int)(bounds.size.width + 0.5);
		int height = (int)(bounds.size.height + 0.5);

		/* **MADE AT THE VIEW'S SIZE, NOT AT 1x1 AND THEN TOLD.** MEASURED: an engine created 1x1 and resized to the
		 * view's size before its first load rendered a 1x1 page — a single pixel, drawn into a 320x200 view — because the
		 * viewport the page laid out for was the one the engine was born with. The resize below is for every later
		 * change; this is for the first. */
		if (width < 1) {
			width = 1;
		}
		if (height < 1) {
			height = 1;
		}
		callbacks.load_changed = ag_view_load_changed;
		callbacks.title_changed = ag_view_title_changed;
		callbacks.damaged = ag_view_damaged;
		_engine = AGWebEngineCreate(width, height, &callbacks, self);
			/* The scale and the size are pushed by `-ag_syncGeometry`, which asks the window rather than trusting whatever
		 * `_scale` had been set to when this view was created. */
	}
	if (_engine == NULL) {
		return;
	}
	{
		AGKeyboard *keyboard = AGKeyboardCreate();

		if (keyboard != NULL) {
			AGKeyboardUseDefaultKeymap(keyboard);
			_keyboard = keyboard;
		}
	}
	[self ag_syncGeometry];
	/* **THE PUMP IS SCHEDULED ON THIS RUN LOOP AND KEPT.** A view created on a thread with no run loop — a test's
	 * `main` before `NSApplication` starts — gets a timer that fires as soon as one runs, which is what makes the
	 * offscreen test possible; the run loop the timer belongs to is the one current at creation. */
	if (_pump == nil) {
		_pump = [[NSTimer scheduledTimerWithTimeInterval:AG_WEBVIEW_PUMP_INTERVAL
							  target:self
							selector:@selector(ag_pump:)
							userInfo:nil
							 repeats:YES] retain];
	}
}

/* **THE ENGINE'S SIZE IS THE VIEW'S SIZE IN DEVICE PIXELS**, and it is asked of the window rather than assumed: a
 * fractional scale makes the two numbers different, and the engine's drawing, its frame and every mouse coordinate are
 * all in ITS pixels. */
/* **THE VIEW'S GEOMETRY IS RE-DERIVED, NOT REMEMBERED, AND THAT IS THE WHOLE OF TWO BUGS.** MEASURED in the demo:
 *
 *   1. RESIZING THE WINDOW STRETCHED THE PAGE INSTEAD OF REFLOWING IT. The AppKit resizes a window's content view through
 *      `-setFrame:`, and this class overrode only `-setFrameSize:` — so the engine kept the size it was born with and
 *      `CGContextDrawImage` scaled that old frame into the new bounds. The page was not re-laid out because nobody told
 *      the engine the viewport had changed.
 *   2. THE PAGE WAS SOFT BY DEFAULT. `_scale` was read ONCE in `-viewDidMoveToWindow`, and a window does not know its
 *      output's scale at that moment — it learns it from the compositor afterwards — so the factor stayed 1.0 and every
 *      glyph was rendered at 1x and resampled up.
 *
 * So the scale is asked of the window EVERY TIME, the size every time, and the pair is pushed only when it differs from
 * what was pushed last. Called from the frame doors, from `-drawRect:`, and from the pump — because "which door does the
 * AppKit use" is a question this class should not have to answer twice. */
- (void)ag_syncGeometry
{
	int width;
	int height;
	CGFloat scale = 1.0;

	if (_engine == NULL) {
		return;
	}
	if ([self window] != nil) {
		CGFloat reported = [[self window] userSpaceScaleFactor];

		if (reported > 0.0) {
			scale = reported;
		}
	}
	width = (int)([self bounds].size.width + 0.5);
	height = (int)([self bounds].size.height + 0.5);
	if (width < 1) {
		width = 1;
	}
	if (height < 1) {
		height = 1;
	}
	if (width == _pushed_points_width && height == _pushed_points_height && (double)scale == _pushed_scale) {
		return;
	}
	if (getenv("WK_WEBVIEW_DIAG") != NULL) {
		fprintf(stderr, "  DIAG geometry: %dx%d points at scale %.3f (was %dx%d at %.3f)\n", width, height,
			(double)scale, _pushed_points_width, _pushed_points_height, _pushed_scale);
	}
	{
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		_geometry_changed_ms = now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
		_geometry_answered_ms = 0.0;
	}
	_pushed_points_width = width;
	_pushed_points_height = height;
	_pushed_scale = (double)scale;
	_scale = scale;
	AGWebEngineSetDeviceScaleFactor((AGWebEngine *)_engine, (double)scale);
	AGWebEngineSetSize((AGWebEngine *)_engine, width, height);
	/* **AND THE NEW FRAME IS REQUESTED NOW, NOT AT THE NEXT TURN OF THE TIMER.** WebKit's relayout is asynchronous, so this
	 * cannot make it synchronous — but it starts it in the same pass as the resize rather than up to 16 ms later, which is
	 * the difference between one stale paint and several. */
	AGWebEngineIterate((AGWebEngine *)_engine);
}

- (void)ag_pump:(NSTimer *)timer
{
	(void)timer;
	if (_engine == NULL) {
		return;
	}
	AGWebEngineIterate((AGWebEngine *)_engine);
	/* **AND THE GEOMETRY, EVERY TURN, WHICH IS CHEAP AND MAKES THE VIEW SELF-CORRECTING.** A window learns its output's
	 * scale after it is on screen; a resize may arrive through any of the AppKit's doors; asking here means none of that
	 * has to be known in advance. */
	[self ag_syncGeometry];
	if (_dirty) {
		_dirty = NO;
		[self ag_flushDamage];
	}
}

/* **THE RECTANGLE IS UNIONED, NOT REPLACED.** Two frames can arrive between two paints — a load and a hover — and a view
 * told only about the last one would leave the first one's pixels stale. */
- (void)ag_engineDamagedInPixelsX:(int)x y:(int)y width:(int)width height:(int)height
{
	if (width <= 0 || height <= 0) {
		return;
	}
	if (_damage_width <= 0 || _damage_height <= 0) {
		_damage_x = x;
		_damage_y = y;
		_damage_width = width;
		_damage_height = height;
	} else {
		int right = MAX(_damage_x + _damage_width, x + width);
		int bottom = MAX(_damage_y + _damage_height, y + height);

		_damage_x = MIN(_damage_x, x);
		_damage_y = MIN(_damage_y, y);
		_damage_width = right - _damage_x;
		_damage_height = bottom - _damage_y;
	}
	_dirty = YES;
}

/* **A DEVICE-PIXEL RECTANGLE BECOMES A VIEW RECTANGLE**, which is a divide and a flip: the engine's y runs down from the
 * page's top and the view's runs up from its bottom. The result is what AppKit is told to redraw, so it is also the number
 * the compositor ends up damaging. */
- (void)ag_flushDamage
{
	CGFloat scale = (_scale > 0.0) ? _scale : 1.0;
	CGFloat height = [self bounds].size.height;
	NSRect rect;

	if (_damage_width <= 0 || _damage_height <= 0) {
		return;
	}
	rect.origin.x = (CGFloat)_damage_x / scale;
	rect.size.width = (CGFloat)_damage_width / scale;
	rect.origin.y = height - (CGFloat)(_damage_y + _damage_height) / scale;
	rect.size.height = (CGFloat)_damage_height / scale;
	_damageRect = rect;
	_damage_width = 0;
	_damage_height = 0;
	[self setNeedsDisplayInRect:rect];
}

- (void)ag_engineLoadStage:(int)stage
{
	if (stage == AGWebLoadStarted) {
		_loading = YES;
	} else if (stage == AGWebLoadFinished) {
		_loading = NO;
		_dirty = YES;
		if ([_navigationDelegate respondsToSelector:@selector(webView:didFinishNavigation:)]) {
			[_navigationDelegate webView:self didFinishNavigation:nil];
		}
	} else if (stage == AGWebLoadCommitted) {
		if ([_navigationDelegate respondsToSelector:@selector(webView:didCommitNavigation:)]) {
			[_navigationDelegate webView:self didCommitNavigation:nil];
		}
	}
}

/* ----------------------------------------------------------------------------------------------------------------
 * loading
 * ---------------------------------------------------------------------------------------------------------------- */

- (void)loadRequest:(NSURLRequest *)request
{
	NSURL *url = [request URL];
	NSString *string = [url absoluteString];

	if (string == nil) {
		return;
	}
	[self ag_ensureEngine];
	if (_engine == NULL) {
		return;
	}
	[_lastURI release];
	_lastURI = [string copy];
	[_pendingHTML release];
	_pendingHTML = nil;
	[_pendingBase release];
	_pendingBase = nil;
	_loading = YES;
	AGWebEngineLoadURI((AGWebEngine *)_engine, [string UTF8String]);
}

- (void)loadHTMLString:(NSString *)string baseURL:(NSURL *)baseURL
{
	if (string == nil) {
		return;
	}
	[self ag_ensureEngine];
	if (_engine == NULL) {
		return;
	}
	[_pendingHTML release];
	_pendingHTML = [string copy];
	[_pendingBase release];
	_pendingBase = [[baseURL absoluteString] copy];
	[_lastURI release];
	_lastURI = nil;
	_loading = YES;
	AGWebEngineLoadHTML((AGWebEngine *)_engine, [string UTF8String],
			    _pendingBase != nil ? [_pendingBase UTF8String] : NULL);
}

- (void)reload
{
	if (_pendingHTML != nil) {
		[self loadHTMLString:_pendingHTML baseURL:(_pendingBase != nil ? [NSURL URLWithString:_pendingBase] : nil)];
	} else if (_lastURI != nil) {
		[self loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:_lastURI]]];
	}
}

- (NSString *)title
{
	const char *title;

	if (_engine == NULL) {
		return nil;
	}
	title = AGWebEngineTitle((AGWebEngine *)_engine);
	return (title != NULL) ? [NSString stringWithUTF8String:title] : nil;
}

- (NSURL *)URL
{
	const char *uri;

	if (_engine == NULL) {
		return nil;
	}
	uri = AGWebEngineURI((AGWebEngine *)_engine);
	return (uri != NULL) ? [NSURL URLWithString:[NSString stringWithUTF8String:uri]] : nil;
}

- (BOOL)canGoBack
{
	return (_engine != NULL && AGWebEngineCanGoBack((AGWebEngine *)_engine)) ? YES : NO;
}

- (void)goBack
{
	if (_engine != NULL) {
		AGWebEngineGoBack((AGWebEngine *)_engine);
	}
}

- (BOOL)canGoForward
{
	return (_engine != NULL && AGWebEngineCanGoForward((AGWebEngine *)_engine)) ? YES : NO;
}

- (void)goForward
{
	if (_engine != NULL) {
		AGWebEngineGoForward((AGWebEngine *)_engine);
	}
}

- (double)estimatedProgress
{
	if (_engine == NULL) {
		return 0.0;
	}
	return AGWebEngineProgress((AGWebEngine *)_engine);
}

- (BOOL)isLoading
{
	return _loading;
}

- (id<WKNavigationDelegate>)navigationDelegate
{
	return _navigationDelegate;
}

- (void)setNavigationDelegate:(id<WKNavigationDelegate>)delegate
{
	_navigationDelegate = delegate;	/* assigned, as Apple's is */
}

- (NSRect)damageRect
{
	return _damageRect;
}

- (NSSize)frameSizeInPixels
{
	int width = 0;
	int height = 0;

	/* **ASKED OF THE ENGINE, NOT OF THE IMAGE.** The image is made at draw time; a caller asking before the first paint
	 * would otherwise be told 0x0 about a page that has already rendered. The engine knows, and it knows the number the
	 * page laid out for. */
	if (_engine != NULL) {
		AGWebEngineFrameSize((AGWebEngine *)_engine, &width, &height);
	}
	return NSMakeSize((CGFloat)width, (CGFloat)height);
}

/* ----------------------------------------------------------------------------------------------------------------
 * drawing
 * ---------------------------------------------------------------------------------------------------------------- */

- (void)viewDidMoveToWindow
{
	[super viewDidMoveToWindow];
	if ([self window] != nil) {
		/* **`-userSpaceScaleFactor` IS THE PUBLIC DOOR ONTO THE SEAM'S SCALE**, and the number it answers is the one the
		 * window's context was made with — the fraction, not a rounded 1 or 2, which is what a fractional display
		 * needs. The seam's `AGWindowBufferScale` is the same number one layer down; asking the AppKit means this class
		 * uses no private door of another component for the one thing it must get right. */
		CGFloat scale = [[self window] userSpaceScaleFactor];

		if (getenv("WK_WEBVIEW_DIAG") != NULL) {
			/* **WHAT THE WINDOW KNOWS AT THE MOMENT THE VIEW JOINS IT**, which is the number the old code trusted. */
			fprintf(stderr, "  DIAG join: window scale is %.3f at -viewDidMoveToWindow\n", (double)scale);
		}
		_scale = (scale > 0.0) ? scale : 1.0;
		[self ag_syncGeometry];
	}
	_dirty = YES;
	[self setNeedsDisplay:YES];
}

- (void)setFrameSize:(NSSize)newSize
{
	[super setFrameSize:newSize];
	[self ag_syncGeometry];
	_dirty = YES;
}

/* **AND THE DOOR THE APPKIT ACTUALLY USES FOR A WINDOW RESIZE.** `-setFrame:` is what `NSWindow` calls on its content view;
 * `-setFrameSize:` is what a caller uses. Overriding one and not the other is the bug above, so both are here. */
- (void)setFrame:(NSRect)frameRect
{
	[super setFrame:frameRect];
	[self ag_syncGeometry];
	_dirty = YES;
}

/* **THE FRAME, TURNED INTO AN IMAGE — AND THE ALPHA FIX-UP IS THE WHOLE OF WHY THIS IS NOT A ONE-LINER.** The engine's
 * pixels carry no alpha; a premultiplied image with alpha 0 draws nothing. So each pixel's top byte is set to 0xFF as
 * the image is made, and the image is made only when the engine has a frame and its size has changed since the last one. */
- (void)ag_updateImage
{
	int width = 0;
	int height = 0;
	size_t bytes;
	int y;
	CGDataProviderRef provider;
	CGColorSpaceRef space;

	if (_engine == NULL) {
		return;
	}
	AGWebEngineFrameSize((AGWebEngine *)_engine, &width, &height);
	if (width <= 0 || height <= 0) {
		return;
	}
	if (width != _pixelWidth || height != _pixelHeight || _pixels == NULL) {
		unsigned char *grown = realloc(_pixels, (size_t)width * (size_t)height * 4u);

		if (grown == NULL) {
			return;
		}
		_pixels = grown;
		_pixelWidth = width;
		_pixelHeight = height;
		/* **THE IMAGE GOES WITH THE BUFFER IT WRAPS.** A `realloc` that moves the pixels leaves the old image holding a
		 * pointer to freed memory, so a size change is the one moment the image must be made again. */
		if (_image != NULL) {
			CGImageRelease((CGImageRef)_image);
			_image = NULL;
		}
	}
	/* **EVERY FRAME IS COPIED, EVERY TIME.** The first version of this method returned early once an image existed —
	 * "the same image, and the engine draws into it below", said the comment, and nothing drew into it: the view showed
	 * the FIRST frame it ever saw, for ever. MEASURED as a page whose load painted nothing but whose hover also painted
	 * nothing, because neither frame was ever copied. */
	AGWebEngineDraw((AGWebEngine *)_engine, (uint32_t *)_pixels, width * 4, width, height);
	if (_geometry_answered_ms == 0.0 && _geometry_changed_ms > 0.0) {
		struct timespec now;
		double expected = (_pushed_scale > 0.0) ? _pushed_scale : 1.0;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (width == (int)((double)_pushed_points_width * expected + 0.5)) {
			_geometry_answered_ms = now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
			if (getenv("WK_WEBVIEW_DIAG") != NULL) {
				fprintf(stderr, "  DIAG reflow: %.1f ms from the geometry change to a frame at it\n",
					_geometry_answered_ms - _geometry_changed_ms);
			}
		}
	}
	for (y = 0; y < width * height; y++) {
		_pixels[y * 4u + 3u] = 0xFF;
	}
	if (_image != NULL) {
		return;
	}
	bytes = (size_t)width * (size_t)height * 4u;
	provider = CGDataProviderCreateWithData(NULL, _pixels, bytes, NULL);
	space = CGColorSpaceCreateDeviceRGB();
	if (provider != NULL && space != NULL) {
		_image = CGImageCreate((size_t)width, (size_t)height, 8, 32, (size_t)width * 4u, space,
				       kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, provider, NULL,
				       false, kCGRenderingIntentDefault);
	}
	if (_image == NULL) {
		fprintf(stderr, "WKWebView: could not make an image from a %dx%d frame (provider %p, space %p)\n", width,
			height, (void *)provider, (void *)space);
	}
	if (provider != NULL) {
		CGDataProviderRelease(provider);
	}
	if (space != NULL) {
		CGColorSpaceRelease(space);
	}
}

- (void)drawRect:(NSRect)dirtyRect
{
	CGContextRef cg;
	NSRect bounds;

	(void)dirtyRect;
	bounds = [self bounds];
	[self ag_syncGeometry];
	[self ag_updateImage];
	cg = [[NSGraphicsContext currentContext] CGContext];
	if (getenv("WK_WEBVIEW_DIAG") != NULL) {
		int width = 0;
		int height = 0;

		AGWebEngineFrameSize((AGWebEngine *)_engine, &width, &height);
		fprintf(stderr, "  DIAG draw: context %p cg %p image %p engine frame %dx%d bounds %.0fx%.0f\n",
			(void *)[NSGraphicsContext currentContext], (void *)cg, (void *)_image, width, height,
			bounds.size.width, bounds.size.height);
	}
	if (cg == NULL) {
		return;
	}
	if (_image == NULL) {
		/* **A VIEW WITH NO FRAME YET IS STILL A VIEW, AND IT PAINTS ITS PAGE'S BACKGROUND.** This used to return early, and
		 * the consequence was a window that showed NOTHING — not a blank page, nothing — until WebKit's first frame
		 * arrived, because the AppKit paints what views draw and commits what it painted. A browser shows white while it
		 * waits, and now so does this.
		 *
		 * **AND A WARNING ABOUT CHASING SYMPTOMS FROM A SCREENSHOT**, because this comment used to claim more than it knew:
		 * the invisibility I first blamed on this was a CAPTURE artefact (see `demo/agora-webview.sh` — a frame is written
		 * when the compositor stops, and an application that has already exited has no window in it). The change is still
		 * right for the reason above; it was not the cause of the disappearance. */
		[[NSColor whiteColor] setFill];
		NSRectFill(bounds);
		return;
	}
	/* **NO FLIP, AND THAT IS THE MEASUREMENT RATHER THAN A SIMPLIFICATION.** `CGContextDrawImage` puts the image's FIRST
	 * ROW at the TOP of the destination rectangle in USER SPACE, and this view's space is unflipped — so a rectangle
	 * whose top is the view's top lands the page's top row there with no transform at all. The first version of this
	 * method flipped the CTM "because the image is top-down", and MEASURED it drew every page upside down: the red band
	 * that belongs at the top came out at the bottom of the surface. A flip here is a double flip.
	 *
	 * **AND THE FRAME IS DRAWN AT ITS OWN SIZE, WHICH IS WHAT REMOVES THE NOISE DURING A RESIZE.** Reported by a person
	 * using the demo: dragging a window edge showed the page STRETCHING and SHRINKING before it reflowed. That was this
	 * method drawing the previous frame into the NEW bounds — a bitmap squashed to fit for as long as WebKit takes to lay
	 * the page out again. A stale frame is now drawn where it belongs, at the size it was painted, anchored top-left, and
	 * whatever the window has gained is the page's own background. A page that is momentarily too small is honest; a page
	 * distorted to fit is the artefact. */
	{
		CGFloat scale = (_scale > 0.0) ? _scale : 1.0;
		CGFloat image_points_w = (CGFloat)_pixelWidth / scale;
		CGFloat image_points_h = (CGFloat)_pixelHeight / scale;

		/* The area the frame does not cover — a window that has just grown, or the first paint after a shrink — is the
		 * page's background, which is what a browser shows before its content is laid out. */
		if (image_points_w < bounds.size.width || image_points_h < bounds.size.height) {
			[[NSColor whiteColor] setFill];
			NSRectFill(bounds);
		}
		CGContextDrawImage(cg, CGRectMake(0.0, bounds.size.height - image_points_h, image_points_w, image_points_h),
				   (CGImageRef)_image);
	}
}

/* ----------------------------------------------------------------------------------------------------------------
 * input
 * ---------------------------------------------------------------------------------------------------------------- */

- (BOOL)acceptsFirstResponder
{
	return YES;
}

- (BOOL)becomeFirstResponder
{
	return YES;
}

/* **A VIEW-POINT TO AN ENGINE-POINT, WHICH IS A FLIP AND A SCALE.** The window's location is converted into this view's
 * coordinates first (so a subview, a scroll offset or a moved window all work), then y is flipped because the engine's
 * origin is the page's top-left, then both are multiplied by the buffer scale because the engine counts device pixels. */
- (NSPoint)ag_enginePointForEvent:(NSEvent *)event
{
	NSPoint point = [self convertPoint:[event locationInWindow] fromView:nil];

	/* **THE INPUT IS IN DEVICE PIXELS, AND THAT IS THE OPPOSITE OF WHAT THE SIZE IS IN.** Sizes are points and the factor
	 * turns them into pixels (see `AGWebEngineSetDeviceScaleFactor`), but `wpe_input_pointer_event`'s coordinates are in the
	 * surface's DEVICE pixels and WebKit divides them by the factor itself to get CSS pixels. So a view that passed points
	 * put every click at 1/factor of its distance: MEASURED at scale 2 on a 300-point-tall view, a pointer at window y=120
	 * reached the page as `clientY=90` instead of 180 — half, and a person reported exactly that, as a drag selecting the
	 * text ABOVE where they pressed. */
	{
		CGFloat scale = (_scale > 0.0) ? _scale : 1.0;

		return NSMakePoint(point.x * scale, ([self bounds].size.height - point.y) * scale);
	}
}

- (uint32_t)ag_timeForEvent:(NSEvent *)event
{
	double milliseconds = [event timestamp] * 1000.0;

	return (uint32_t)(milliseconds > 0.0 ? milliseconds : 0.0);
}

- (void)ag_sendButton:(NSUInteger)button pressed:(BOOL)pressed withEvent:(NSEvent *)event
{
	NSPoint point;

	if (_engine == NULL) {
		return;
	}
	point = [self ag_enginePointForEvent:event];
	AGWebEnginePointerButton((AGWebEngine *)_engine, (int)(point.x + 0.5), (int)(point.y + 0.5), (uint32_t)button,
				 pressed ? 1 : 0, ag_view_modifiers([event modifierFlags]),
				 [self ag_timeForEvent:event]);
}

static BOOL ag_real_drag_enabled(void);

- (void)mouseDown:(NSEvent *)event
{
	NSPoint point = [self ag_enginePointForEvent:event];

	_drag_last_x = point.x;
	_drag_last_y = point.y;
	_drag_extending = NO;
	_last_drag_extend_ms = 0.0;
	[self ag_sendButton:0x110u pressed:YES withEvent:event];
}

- (void)mouseUp:(NSEvent *)event
{
	if (_drag_extending && _engine != NULL && !ag_real_drag_enabled()) {
		/* **THE RELEASE THAT ENDS THE DRAG, AND IT IS A SHIFT ONE.** The drag's extension is a shift-press per step and no
		 * release until here, so this balances it — a release in the middle would turn each step into a CLICK, and three
		 * clicks at nearly one place are a paragraph selection. */
		NSPoint point = [self ag_enginePointForEvent:event];

		_drag_extending = NO;
		AGWebEnginePointerButton((AGWebEngine *)_engine, (int)(point.x + 0.5), (int)(point.y + 0.5), 0x110u, 0,
					 AGModifierShift, [self ag_timeForEvent:event]);
		AGWebEngineSetSuppressNavigation((AGWebEngine *)_engine, 0);
		return;
	}
	[self ag_sendButton:0x110u pressed:NO withEvent:event];
}

- (void)rightMouseDown:(NSEvent *)event
{
	[self ag_sendButton:0x112u pressed:YES withEvent:event];
}

- (void)rightMouseUp:(NSEvent *)event
{
	[self ag_sendButton:0x112u pressed:NO withEvent:event];
}

/* **WHETHER THIS WEBKIT CAN SELECT BY DRAGGING IS NOT SOMETHING IT WILL SAY AT RUNTIME**, so the embedder says it. `-DENABLE_DRAG_SUPPORT=ON`
 * is a compile-time choice of the port, and the two obvious runtime probes are both useless — MEASURED: `typeof DragEvent`
 * is "function" in a build with drag support OFF as well as ON, so the IDL is not gated by that option, and no settings
 * object exposes it either. The honest door is an environment variable set by whoever chose the library: the Agora demo
 * sets `AGWEBVIEW_REAL_DRAG=1` when it puts a locally built WebKit (which has the option ON) on `LD_LIBRARY_PATH`, and
 * the AppKit's own tests leave it unset and therefore exercise the emulation every build has. */
static BOOL ag_real_drag_enabled(void)
{
	static int decided = 0;
	static BOOL enabled = NO;

	if (!decided) {
		const char *value = getenv("AGWEBVIEW_REAL_DRAG");

		enabled = (value != NULL && value[0] == '1' && value[1] == '\0') ? YES : NO;
		decided = 1;
	}
	return enabled;
}

/* **A DRAG SELECTS TEXT BY BEING A SHIFT-CLICK, BECAUSE THIS WEBKIT BUILD CANNOT DO IT ANY OTHER WAY.** Not a bug in this
 * seam and not a WebKit bug either — a BUILD OPTION. WebKit 2.48's `Source/cmake/WebKitFeatures.cmake` defines
 * `ENABLE_DRAG_SUPPORT` as PRIVATE **OFF**, and its own description names exactly what that costs: "Toggle support of drag
 * actions (including selection of text with mouse)". Debian's `debian/rules` does not turn it on, so dragging cannot select
 * text here for ANY embedder, and the branch that would do it (`EventHandler::handleMouseDraggedEvent`, called from
 * `handleMouseMoveEvent` inside `#if ENABLE_DRAG_SUPPORT`) is not compiled in.
 *
 * MEASURED, because "not compiled in" is a claim that has to be paid for: a press followed by ten motions each carrying
 * `buttons === 1` selects NOTHING (the page reports `sel=0`), while in the same run a plain press DOES place a caret
 * (`anchorOffset=1` in a text node) and the drag's first motion DOES fire `selectstart` — so the press half of the path is
 * intact and only the drag half is missing. Setting `MouseEvent.buttons` did not change that (see the engine's
 * `pressed_buttons`); it fixed a real bug of its own, drag handlers and drag-scrolling, but not this one.
 *
 * So selection is extended the classic way: the press sets the anchor, and a SHIFT-modified click moves the extent. That
 * works on EVERY build, which is why it is the default.
 *
 * WITH A LOCALLY BUILT WEBKIT (`ENABLE_DRAG_SUPPORT=ON`, see `~/Development/wpewebkit`) the natural path exists and is
 * better: MEASURED on 2.54.2 built that way, the same press-motion-release drag that selected 0 characters on the system
 * library selects 37, step by step (29, 33, 37), because a real drag is what the editor and the page both expect. That
 * path is taken only when `AGWEBVIEW_REAL_DRAG=1` says the library supports it — a drag that is sent as motion to a build
 * without drag support selects NOTHING, so guessing here would trade a working selection for a broken one.
 *
 * THE RISK WAS LINKS, and it was measured rather than assumed: a shift-click on a link in this engine does NOT navigate
 * (nor does a plain click — no navigation policy is installed yet), so a drag that crosses a link cannot open one. When
 * policy handling arrives, this comment is the place that has to be revisited. */
- (void)mouseDragged:(NSEvent *)event
{
	NSPoint point;
	double now;

	[self mouseMoved:event];
	if (_engine == NULL) {
		return;
	}
	now = ag_now_ms();
	if (now - _last_drag_extend_ms < 16.0) {
		return;			/* a drag is 60 events a second; the page needs a few */
	}
	if (ag_real_drag_enabled()) {
		/* **THE NATURAL DRAG: MOTION ONLY.** The press is already down and the engine carries the held button in
		 * `modifiers`, so this is what a browser sends — and it is all WebKit needs to extend its own selection. */
		_last_drag_extend_ms = now;
		[self mouseMoved:event];
		return;
	}
	point = [self ag_enginePointForEvent:event];
	/* **SIX POINTS OF TRAVEL BEFORE ANOTHER PRESS, AND THAT IS THE WHOLE DIFFERENCE BETWEEN A SELECTION AND A PARAGRAPH.**
	 * WebKit counts presses as multi-clicks by TIME and DISTANCE: a drag's first few events move one or two pixels, so a
	 * press per event made a double click (the word) and then a triple (the whole paragraph) — MEASURED as 263 characters
	 * selected where the drag itself covered about 30. A press is now only sent once the pointer has really moved. */
	{
		CGFloat scale = (_scale > 0.0) ? _scale : 1.0;
		CGFloat min_travel = 6.0 * scale;
		CGFloat dx = point.x - _drag_last_x;
		CGFloat dy = point.y - _drag_last_y;

		if (dx * dx + dy * dy < min_travel * min_travel) {
			return;
		}
	}
	_last_drag_extend_ms = now;
	_drag_last_x = point.x;
	_drag_last_y = point.y;
	_drag_extending = YES;
	/* **A DRAG MUST NOT FOLLOW THE LINKS IT CROSSES.** The emulation is a run of shift-clicks, and a shift-click that lands
	 * on a link is a navigation the moment the policy handler above says yes to links (which is what makes a browser a
	 * browser). Navigation is suppressed for as long as the drag's presses are in flight and restored on the release — see
	 * `AGWebEngineSetSuppressNavigation` and `webengine-links`, which asserts both halves. */
	AGWebEngineSetSuppressNavigation((AGWebEngine *)_engine, 1);
	AGWebEnginePointerButton((AGWebEngine *)_engine, (int)(point.x + 0.5), (int)(point.y + 0.5), 0x110u, 1,
				 AGModifierShift, [self ag_timeForEvent:event]);
}

- (void)mouseMoved:(NSEvent *)event
{
	NSPoint point;

	if (_engine == NULL) {
		return;
	}
	point = [self ag_enginePointForEvent:event];
	AGWebEnginePointerMotion((AGWebEngine *)_engine, (int)(point.x + 0.5), (int)(point.y + 0.5),
				 ag_view_modifiers([event modifierFlags]), [self ag_timeForEvent:event]);
}

/* **THE WHEEL, WITH ITS SIGN THE RIGHT WAY ROUND.** `NSEvent`'s `deltaY` is positive when the person scrolls UP (content
 * moves down); the seam's `delta` is `wl_pointer.axis`'s, positive when scrolling DOWN. So the sign flips here, exactly
 * once, and the other flips in this file are all about y. */
- (void)scrollWheel:(NSEvent *)event
{
	NSPoint point;
	CGFloat dx;
	CGFloat dy;

	if (_engine == NULL) {
		return;
	}
	point = [self ag_enginePointForEvent:event];
	dx = [event deltaX];
	dy = [event deltaY];
	if (dy != 0.0) {
		AGWebEngineScroll((AGWebEngine *)_engine, (int)(point.x + 0.5), (int)(point.y + 0.5), AGAxisVertical,
				  (double)(-dy), 0, ag_view_modifiers([event modifierFlags]),
				  [self ag_timeForEvent:event]);
	}
	if (dx != 0.0) {
		AGWebEngineScroll((AGWebEngine *)_engine, (int)(point.x + 0.5), (int)(point.y + 0.5), AGAxisHorizontal,
				  (double)(-dx), 0, ag_view_modifiers([event modifierFlags]),
				  [self ag_timeForEvent:event]);
	}
}

- (void)ag_sendKey:(NSEvent *)event pressed:(BOOL)pressed
{
	uint32_t keycode;
	uint32_t keysym = 0;

	if (_engine == NULL) {
		return;
	}
	keycode = (uint32_t)[event keyCode];
	/* **THE SEAM ALREADY KNOWS WHICH KEY THIS IS, SO THE VIEW ASKS IT FIRST.** `NSEvent.cg_keysym` is written by the
	 * seam's translation path from the compositor's own keymap — the layout the person is using — and a second keymap
	 * here would be a second answer to the same question, free to disagree after a layout switch. The local keyboard is
	 * only the fallback for an event that arrived another way (a synthetic one in a test, which has no keymap behind it). */
	keysym = [event cg_keysym];
	if (keysym == 0 && _keyboard != NULL) {
		keysym = AGKeyboardKeysym((AGKeyboard *)_keyboard, keycode);
	}
	AGWebEngineKey((AGWebEngine *)_engine, keysym, keycode, ag_view_modifiers([event modifierFlags]),
		       pressed ? 1 : 0, [self ag_timeForEvent:event]);
}

- (void)keyDown:(NSEvent *)event
{
	[self ag_sendKey:event pressed:YES];
}

- (void)keyUp:(NSEvent *)event
{
	[self ag_sendKey:event pressed:NO];
}

@end
