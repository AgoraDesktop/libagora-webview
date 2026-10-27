/*
 * appkit_webview — the class, offscreen: does a page arrive in a view, upright, and does the pointer reach it?
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHAT IS ASKED, and why each is a separate claim:
 *
 *   1. **THE VIEW PAINTS THE PAGE, UPRIGHT.** The page is a red band on top and blue below it. A bitmap rep is the
 *      surface (no window, no compositor), `-drawRect:` is called directly, and the pixels are read back. WHICH END of
 *      the rep is the view's top is MEASURED FIRST, with a magenta strip drawn by hand — because "red is at y < 40" is
 *      a claim about the view only if the test knows what y means in a rep it made itself. A class that flipped the
 *      image the wrong way would otherwise pass by drawing upside down into an upside-down surface.
 *   2. **THE TITLE IS THE PAGE'S.** The engine's answer, not a copy the view kept.
 *   3. **THE POINTER REACHES THE PAGE, THROUGH THE CLASS'S OWN CONVERSION.** A `mouseMoved:` is built in WINDOW
 *      coordinates over a `:hover` box, and the page must repaint green — which needs the view's y flip AND the scale
 *      to be right, since a wrong either puts the pointer somewhere else on the page. This is the half of the class
 *      that a page's pixels cannot prove on their own.
 *   4. **THE WHEEL REACHES IT TOO, WITH THE SIGN THE RIGHT WAY.** A cyan band below the fold must come into view after
 *      a scroll built the way AppKit builds one (positive `deltaY` = scroll up).
 *
 * NO WINDOW SERVER IS NEEDED. The engine is independent of the compositor, the view has no window (so the scale is 1
 * and `-convertPoint:` is the identity), and the run loop is the test's own — which is also what makes this fast enough
 * to be a `ctest`.
 */

#import <AgoraWebView/WKWebView.h>

#import <AppKit/NSView.h>
#import <AppKit/NSWindow.h>
#import <AppKit/NSEvent.h>
#import <AppKit/NSColor.h>
#import <AppKit/NSBitmapImageRep.h>
#import <AppKit/NSGraphicsContext.h>
#import <AppKit/NSBezierPath.h>
#import <Foundation/NSRunLoop.h>
#import <Foundation/NSDate.h>

#include <stdio.h>
#include <string.h>

#define W 320
#define H 200

static int s_failed;

static void check(int ok, const char *name)
{
	printf("  %s   %s\n", ok ? "ok  " : "FAIL", name);
	if (!ok) {
		s_failed++;
	}
}

/* A DELEGATE THAT COUNTS, so "the load finished" is a number rather than a hope. */
@interface AGWebDelegate : NSObject <WKNavigationDelegate>
{
@public
	int finished;
	int committed;
}
@end

@implementation AGWebDelegate
- (void)webView:(WKWebView *)webView didCommitNavigation:(id)navigation
{
	(void)webView;
	(void)navigation;
	committed++;
}
- (void)webView:(WKWebView *)webView didFinishNavigation:(id)navigation
{
	(void)webView;
	(void)navigation;
	finished++;
}
@end

static NSBitmapImageRep *new_rep(void)
{
	return [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:W pixelsHigh:H
		bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
		colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:W * 4 bitsPerPixel:32];
}

/* **WHICH WAY UP IS A REP?** A magenta strip is filled at the BOTTOM of the view's own rectangle — the view's y = 0,
 * which is its bottom because an unflipped view's origin is there — and the rows that come out magenta say which way the
 * rep's rows run. Everything else in this file that talks about "the top of the view" is stated in those terms. */
static int rep_top_is_view_top(void)
{
	NSBitmapImageRep *rep = new_rep();
	int top_is_top;

	[NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];
	[[NSColor whiteColor] setFill];
	[[NSBezierPath bezierPathWithRect:NSMakeRect(0.0, 0.0, (CGFloat)W, (CGFloat)H)] fill];
	[[NSColor magentaColor] setFill];
	[[NSBezierPath bezierPathWithRect:NSMakeRect(0.0, 0.0, 10.0, 10.0)] fill];

	/* The strip is at the view's bottom, so if it is in the LOW rows of the rep then low rows are the bottom. */
	top_is_top = [rep colorAtX:2 y:2] != nil && [[rep colorAtX:2 y:2] redComponent] > 0.9 &&
		     [[rep colorAtX:2 y:2] blueComponent] > 0.9 &&
		     [[rep colorAtX:2 y:H - 3] redComponent] < 0.1 ? 0 : 1;
	return top_is_top;
}

/* DRAW THE VIEW INTO A FRESH REP, so every read starts from a clean surface. */
static NSBitmapImageRep *draw(NSView *view)
{
	NSBitmapImageRep *rep = new_rep();

	[NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];
	[[NSColor whiteColor] setFill];
	[[NSBezierPath bezierPathWithRect:NSMakeRect(0.0, 0.0, (CGFloat)W, (CGFloat)H)] fill];
	[view drawRect:[view bounds]];
	return rep;
}

static int is_red(NSColor *c)
{
	return c != nil && [c redComponent] > 0.85 && [c greenComponent] < 0.15 && [c blueComponent] < 0.15;
}

static int is_blue(NSColor *c)
{
	return c != nil && [c blueComponent] > 0.6 && [c redComponent] < 0.15 && [c greenComponent] < 0.15;
}

static int is_green(NSColor *c)
{
	return c != nil && [c greenComponent] > 0.6 && [c redComponent] < 0.15 && [c blueComponent] < 0.15;
}

static int is_cyan(NSColor *c)
{
	return c != nil && [c greenComponent] > 0.7 && [c blueComponent] > 0.7 && [c redComponent] < 0.2;
}

static int rep_has(NSBitmapImageRep *rep, int (*predicate)(NSColor *))
{
	int x, y;

	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			if (predicate([rep colorAtX:x y:y])) {
				return 1;
			}
		}
	}
	return 0;
}

static int row_has(NSBitmapImageRep *rep, int y, int (*predicate)(NSColor *))
{
	int x;

	for (x = 0; x < W; x++) {
		if (predicate([rep colorAtX:x y:y])) {
			return 1;
		}
	}
	return 0;
}

/* **RUN THE RUN LOOP, WHICH IS WHAT FIRES THE VIEW'S PUMP TIMER.** The engine's work happens in the timer, and a test
 * that slept instead would wait for a callback nothing was driving. */
static void pump(NSTimeInterval seconds)
{
	[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

int main(void)
{
	static const char *page =
		"<html><head><title>Agora web view</title><style>"
		"  body { margin:0; background:#0000c0; }"
		"  #strip { height:40px; background:#ff0000; }"
		"  #hover { height:40px; background:#002040; }"
		"  #hover:hover { background:#00c000; }"
		"  #spacer { height:200px; background:#0000c0; }"
		"  #marker { height:200px; background:#00ffff; }"
		"</style></head><body>"
		"<div id=strip></div><div id=hover></div><div id=spacer></div><div id=marker></div>"
		"<div style='height:400px;background:#0000c0'></div>"
		"</body></html>";
	AGWebDelegate *delegate = [[AGWebDelegate alloc] init];
	WKWebView *view;
	NSBitmapImageRep *rep;
	int top_is_top;
	int waited;
	NSEvent *event;

	printf("appkit_webview: building the view\n");
	view = [[WKWebView alloc] initWithFrame:NSMakeRect(0.0, 0.0, (CGFloat)W, (CGFloat)H)];
	[view setNavigationDelegate:delegate];
	[view loadHTMLString:[NSString stringWithUTF8String:page] baseURL:nil];

	/* WAIT FOR THE LOAD, and then a little longer: the page's own frame arrives after the load finishes. */
	waited = 0;
	while (delegate->finished == 0 && waited < 400) {
		pump(0.05);
		waited++;
	}
	pump(0.5);
	printf("appkit_webview: %d commit(s), %d finish(es) after %d slice(s)\n", delegate->committed, delegate->finished,
	       waited);

	check(delegate->finished > 0, "the page finished loading (the delegate heard it)");
	check([view isLoading] == NO, "the view reports the load finished");
	check([view title] != nil && [[view title] isEqualToString:@"Agora web view"], "the title is the page's own");
	check([view frameSizeInPixels].width == W && [view frameSizeInPixels].height == H,
	      "the engine's frame is the view's size in device pixels");

	/* 1. THE PAGE, UPRIGHT. */
	top_is_top = rep_top_is_view_top();
	printf("appkit_webview: this rep's rows run %s\n", top_is_top ? "down from the view's top" : "up from its bottom");
	rep = draw(view);
	{
		int top_row = top_is_top ? 10 : H - 10;
		int bottom_row = top_is_top ? H - 10 : 10;

		check(row_has(rep, top_row, is_red), "the page's red band is at the TOP of the view");
		check(row_has(rep, bottom_row, is_blue), "the page's blue body is at the BOTTOM of the view");
	}

	/* 2. THE POINTER, THROUGH THE VIEW'S OWN CONVERSION. The `:hover` box is the page's SECOND 40 points — y 40 to 79 —
	 * so its middle is 60 down the page, which in the view's own coordinates (unflipped, origin at the bottom) is
	 * 200 - 60 = 140. A view that did not flip y would put this pointer in a different strip and paint nothing. */
	event = [NSEvent mouseEventWithType:NSMouseMoved
				   location:NSMakePoint(160.0, (CGFloat)(H - 60))
			      modifierFlags:0
				  timestamp:1.0
			       windowNumber:0
				    context:nil
				eventNumber:1
				 clickCount:1
				   pressure:0.0f];
	[view mouseMoved:event];
	{
		int hovered = 0;
		int tries;

		for (tries = 0; tries < 40 && !hovered; tries++) {
			pump(0.05);
			[rep release];
			rep = draw(view);
			hovered = rep_has(rep, is_green);
		}
		check(hovered, "the pointer reached the page (its :hover box turned green)");
	}

	/* 3. THE WHEEL, IN APPKIT'S SIGN. `deltaY` is positive when a person scrolls UP, and the cyan band begins 280 points
	 * down the page — 80 points below the fold — so the scroll has to be more than one notch. Four events of -60 are sent
	 * with the run loop pumped between them, because WebKit's scroll is smooth and a burst with no loop between is a
	 * burst into a stopped engine. */
	{
		int scrolled = 0;
		int step;

		for (step = 0; step < 4; step++) {
			event = [NSEvent cg_scrollEventWithDeltaX:0.0 deltaY:-60.0
							location:NSMakePoint(160.0, (CGFloat)(H - 60))
						   modifierFlags:0
						       timestamp:2.0 + step
						    windowNumber:0];
			[view scrollWheel:event];
			pump(0.1);
		}
		{
			int tries;

			for (tries = 0; tries < 80 && !scrolled; tries++) {
				pump(0.05);
				[rep release];
				rep = draw(view);
				scrolled = rep_has(rep, is_cyan);
			}
		}
		check(scrolled, "the wheel reached the page (the band below the fold came into view)");
	}

	[rep release];
	[view release];
	[delegate release];
	if (s_failed != 0) {
		printf("appkit_webview: %d check(s) failed\n", s_failed);
		return 1;
	}
	printf("appkit_webview: ok — a page in a view, upright, with the pointer and the wheel\n");
	return 0;
}
