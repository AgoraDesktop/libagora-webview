/*
 * webview_resize — the class in a WINDOW, on a SCALED output: does it know its own geometry, and does it follow a resize?
 *
 * Copyright (C) 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * THE TWO BUGS THIS EXISTS FOR, both found by a person using the demo rather than by any test here:
 *
 *   1. **RESIZING THE WINDOW STRETCHED THE PAGE INSTEAD OF REFLOWING IT.** `NSWindow` resizes its content view through
 *      `-setFrame:`, and the class overrode only `-setFrameSize:` — so the engine kept its birth size and the view scaled
 *      that old frame into the new bounds. What is asserted here is the ENGINE's frame size after a resize, because a
 *      page that was told about the new viewport answers a new number and a page that was stretched does not.
 *   2. **THE PAGE WAS SOFT BY DEFAULT.** The device scale factor was read once, when the view joined its window, and a
 *      window does not know its output's scale at that moment — MEASURED, with a diagnostic on the demo application:
 *      `-viewDidMoveToWindow` sees **1.000** and the first pump sees **1.332**, so the stored factor was 1.0 for the life of
 *      the view and every glyph was rendered at 1x and resampled into a 1.332x surface.
 *
 *      **THE SCALE ASSERTION BELOW IS A GUARD RATHER THAN A REPRODUCTION**, and saying so matters: the AppKit also resizes
 *      the content view when the window appears, so even the old one-shot code arrived at the right factor in this
 *      ordering. The reproduction is the measurement above; what is guarded here is the OUTCOME — the engine's frame is
 *      the view in points times the output's scale — so a regression cannot quietly come back.
 *
 * The test needs a real window on a scaled compositor, which is why its runner starts one: a bitmap rep and no window
 * cannot show either bug, and `appkit-webview` (the offscreen test) passed throughout.
 */

#import <AgoraWebView/WKWebView.h>

#import <AppKit/NSApplication.h>
#import <AppKit/NSWindow.h>
#import <AppKit/NSView.h>
#import <AppKit/NSScreen.h>

#import <Foundation/NSAutoreleasePool.h>
#import <Foundation/NSDate.h>
#import <Foundation/NSRunLoop.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

@interface AGResizeDelegate : NSObject <WKNavigationDelegate>
{
@public
	int finished;
}
@end

@implementation AGResizeDelegate
- (void)webView:(WKWebView *)webView didFinishNavigation:(id)navigation
{
	(void)webView;
	(void)navigation;
	finished++;
}
@end

static int s_failed;

static void check(int ok, const char *name)
{
	printf("  %s   %s\n", ok ? "ok  " : "FAIL", name);
	if (!ok) {
		s_failed++;
	}
}

static void pump(NSApplication *application, NSTimeInterval seconds)
{
	NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];

	while ([deadline timeIntervalSinceNow] > 0.0) {
		[[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
					 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
		[application updateWindows];
	}
}

int main(void)
{
	static const char *page =
		"<html><head><title>resize probe</title><style>"
		"  body { margin:0; background:#0000c0; }"
		"  #band { height:40px; background:#ff0000; }"
		"</style></head><body><div id=band></div>"
		"<script>document.addEventListener('mousemove',function(e){"
		"document.title='resize probe client='+e.clientX+','+e.clientY;},true);</script>"
		"</body></html>";
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	NSApplication *application = [NSApplication sharedApplication];
	AGResizeDelegate *delegate = [[AGResizeDelegate alloc] init];
	NSWindow *window;
	WKWebView *view;
	CGFloat scale;
	NSSize before;
	NSSize after;
	NSSize bounds_after;
	int waited = 0;

	[application setActivationPolicy:NSApplicationActivationPolicyRegular];
	[application finishLaunching];

	/* **`defer:YES`, WHICH IS THE ORDERING THAT EXPOSED THE FUZZY BUG.** With `defer:NO` the window's seam exists before the
	 * view joins it and its output scale is already known; the demo created the view first and showed the window after, so
	 * `-viewDidMoveToWindow` ran when the scale was still 1.0 and a one-shot read kept it there. That is the ordering this
	 * test has to reproduce, or it asserts the scale only in the case where nothing was wrong. */
	window = [[NSWindow alloc] initWithContentRect:NSMakeRect(20.0, 20.0, 400.0, 300.0)
					     styleMask:NSResizableWindowMask | NSTitledWindowMask
					       backing:NSBackingStoreBuffered
						 defer:YES];
	view = [[WKWebView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 400.0, 300.0)];
	[view setNavigationDelegate:delegate];
	[window setContentView:view];
	[window makeKeyAndOrderFront:nil];
	[view loadHTMLString:[NSString stringWithUTF8String:page] baseURL:nil];

	while (delegate->finished == 0 && waited < 400) {
		pump(application, 0.02);
		waited++;
	}
	/* A few more turns, so the page's own frame has been painted at the right size. */
	pump(application, 0.4);

	scale = [window userSpaceScaleFactor];
	printf("resize: the window's scale is %g\n", (double)scale);
	check(scale > 1.5, "the output really is scaled (the runner starts the compositor at --scale 2)");
	check(delegate->finished > 0, "the page finished loading in a window");

	before = [view frameSizeInPixels];
	printf("resize: 400x300 points -> engine frame %.0fx%.0f device pixels\n", before.width, before.height);
	/* **THE SCALE FACTOR REACHED THE ENGINE**: the frame is the view in points times the output's scale. A view that read
	 * the scale once, too early, reports 400x300 here — the fuzzy page. */
	check(fabs(before.width - 400.0 * (double)scale) <= 2.0 && fabs(before.height - 300.0 * (double)scale) <= 2.0,
	      "the engine renders at the output's scale, not at 1x resampled");

	/* **WHERE DOES THE ENGINE THINK THE POINTER IS?** The page says which CSS pixel it saw, and the window said where the
	 * pointer was — so the conversion from window coordinates to page coordinates is a number rather than an assumption.
	 * This exists because a person reported that a drag selected the text ABOVE where they pressed. */
	{
		NSPoint window_point = NSMakePoint(60.0, 120.0);
		NSEvent *move = [NSEvent mouseEventWithType:NSMouseMoved
						  location:window_point
					     modifierFlags:0
						 timestamp:2.0
					      windowNumber:[window windowNumber]
						   context:nil
					       eventNumber:9
						clickCount:1
						  pressure:0.0f];
		NSPoint local = [view convertPoint:window_point fromView:nil];
		int tries;
		int client_y = -1;

		[view mouseMoved:move];
		for (tries = 0; tries < 30 && client_y < 0; tries++) {
			pump(application, 0.05);
			if ([view title] != nil) {
				const char *title = [[view title] UTF8String];
				const char *at = title != NULL ? strstr(title, "client=") : NULL;

				if (at != NULL) {
					int cx = -1;

					if (sscanf(at + 7, "%d,%d", &cx, &client_y) != 2) {
						client_y = -1;
					}
				}
			}
		}
		printf("resize: window y=%.0f -> view y=%.1f -> the page says clientY=%d (want %.1f)\n",
		       (double)window_point.y, (double)local.y, client_y,
		       (double)([view bounds].size.height - window_point.y));
		check(client_y >= 0 && fabs((double)client_y - ([view bounds].size.height - window_point.y)) <= 1.5,
		      "the page sees the pointer where the window put it");
	}

	/* **AND A WINDOW RESIZE IS A RELAYOUT.** `-setFrame:` is the door NSWindow uses; a view that only watches
	 * `-setFrameSize:` keeps its old frame and the page is stretched. */
	[window setFrame:NSMakeRect(20.0, 20.0, 520.0, 400.0) display:YES];
	pump(application, 0.6);
	bounds_after = [view bounds].size;
	after = [view frameSizeInPixels];
	printf("resize: %.0fx%.0f points -> engine frame %.0fx%.0f device pixels\n", bounds_after.width,
	       bounds_after.height, after.width, after.height);
	check(fabs(bounds_after.width - 520.0) <= 1.0, "the content view followed the window's new frame");
	check(fabs(after.width - bounds_after.width * (double)scale) <= 2.0 &&
		      fabs(after.height - bounds_after.height * (double)scale) <= 2.0,
	      "the engine's frame followed the resize (the page re-laid out rather than being stretched)");
	check(after.width > before.width, "and it got bigger, as the window did");

	[view release];
	[window release];
	[delegate release];
	[pool release];
	if (s_failed != 0) {
		printf("resize: %d check(s) failed\n", s_failed);
		return 1;
	}
	printf("resize: ok — the view knows its output's scale and follows a window resize\n");
	return 0;
}
