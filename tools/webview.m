/*
 * webview — a window with a page in it, on the desktop.
 *
 * Copyright (C) 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHAT THIS IS FOR. `WKWebView` is a class, and a class nobody can look at is a class nobody trusts. This is the smallest
 * application that puts one on a real screen: an `NSApplication`, an `NSWindow`, a `WKWebView` filling it, and a URL from
 * the command line or a built-in page when there is none. It is what `demo/agora-webview.sh` starts.
 *
 * **IT DRAWS THROUGH THE APPKIT, NOT THROUGH THE GPU PRESENTER, AND THAT IS DELIBERATE.** A view inside a CPU-drawn window
 * takes the readback path (`AGWebViewDraw` → `CGImage` → `-drawRect:`), which is the one a window with other views in it
 * needs. The GPU composition (`AGWebExportEGLPresent` → a window's own dma-buf) is a WHOLE-WINDOW shape — the browser as an
 * application — and it belongs to a different door, not to `NSView`.
 *
 *   webview [URL] [--seconds N] [--size WxH]
 *
 * With `--seconds N` it exits after N seconds and prints what it saw, which is how the nested runner asserts it; without
 * it, it runs until the window closes.
 */

#import <AgoraWebView/WKWebView.h>

#import <AppKit/NSApplication.h>
#import <AppKit/NSWindow.h>
#import <AppKit/NSView.h>
#import <AppKit/NSColor.h>
#import <AppKit/NSGraphicsContext.h>
#import <AppKit/AGStatusBarItemView.h>
#import <AppKit/NSScreen.h>

#import <Foundation/NSAutoreleasePool.h>
#import <Foundation/NSDate.h>
#import <Foundation/NSRunLoop.h>
#import <Foundation/NSTimer.h>
#import <Foundation/NSString.h>
#import <Foundation/NSURL.h>
#import <Foundation/NSString.h>

#include <limits.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>

/* **A PAGE THAT NEEDS NO NETWORK, BECAUSE A DEMO THAT DEPENDS ON ONE IS A DEMO THAT IS SOMETIMES A BLANK SCREEN.** It has
 * a title, two bands of colour, a hover rule and a key handler — the four things a person can check by looking at it. */
static NSString *const kDefaultPage =
	@"<html><head><title>Agora web view</title><style>"
	 @"  body { margin:0; font:16px sans-serif; color:#ffffff; }"
	 @"  #band { height:48px; background:#c00000; }"
	 @"  #band:hover { background:#00a000; }"
	 @"  #body { padding:24px; background:#0000c0; height:400px; }"
	 @"  h1 { margin:0 0 8px 0; font-size:22px; }"
	 @"</style></head><body>"
	 @"<div id=band></div>"
	 @"<div id=body><h1>Agora web view</h1>"
	 @"<p>This is WPE WebKit, composited by libagora-webview and drawn by AppKit.</p>"
	 @"<p>Hover the red strip. Click in the text and type: the page hears the keyboard.</p></div>"
	 @"<script>window.addEventListener('keydown',function(e){"
	 @"document.getElementById('band').style.background=e.key=='q'?'#000000':'#008000';});</script>"
	 @"</body></html>";

/* **THE WINDOW TITLE MIRRORS THE PAGE'S TITLE, WHICH MAKES THE APP ITS OWN DIAGNOSTIC.** A page's title is the only channel
 * it has to say anything about itself, and a selection count is exactly the kind of thing that is invisible from outside:
 * when a click fails to clear a selection, this says whether the PAGE still has it (the click never arrived) or the page has
 * 0 and the PICTURE is stale (a repaint bug). Those need opposite fixes, and the difference costs a look at the title bar. */
@interface AGTitleMirror : NSObject
{
@public
	WKWebView *view;
	NSWindow *window;
}
- (void)tick:(NSTimer *)timer;
@end

@implementation AGTitleMirror
- (void)tick:(NSTimer *)timer
{
	NSString *page = [view title];

	(void)timer;
	if (page != nil && ![[window title] isEqualToString:page]) {
		/* **LOGGED AS WELL AS SHOWN**, so a person using the demo does not have to transcribe what happened: the page's own
		 * answer about itself — a selection count, say — lands in the demo's log where it can be read afterwards. */
		fprintf(stderr, "webview: the page says \"%s\"\n", [page UTF8String]);
		fflush(stderr);
		[window setTitle:page];
	}
}
@end

@interface AGWebViewDelegate : NSObject <WKNavigationDelegate>
{
@public
	int finished;
	int committed;
}
@end

@implementation AGWebViewDelegate
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
	printf("webview: the page finished loading — \"%s\"\n", [[webView title] UTF8String] ?: "");
	fflush(stdout);
}
@end

int main(int argc, char **argv)
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	AGWebViewDelegate *delegate = [[AGWebViewDelegate alloc] init];
	NSApplication *application = [NSApplication sharedApplication];
	NSWindow *window;
	WKWebView *view;
	NSURL *url = nil;
	NSRect content;
	const char *title = "Agora web view";
	int seconds = 0;
	int width = 0;
	int height = 0;
	int at_x = INT_MIN;		/* frame origin override, for the placement tests */
	int at_y = INT_MIN;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
			seconds = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
			int x = 0;
			int y = 0;

			if (sscanf(argv[++i], "%d,%d", &x, &y) == 2) {
				at_x = x;
				at_y = y;
			}
		} else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			int w = 0;
			int h = 0;

			if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
				width = w;
				height = h;
			}
		} else if (argv[i][0] != '-') {
			url = [NSURL URLWithString:[NSString stringWithUTF8String:argv[i]]];
		} else {
			fprintf(stderr, "usage: %s [URL] [--seconds N] [--size WxH] [--at X,Y]\n", argv[0]);
			return 2;
		}
	}

	[application setActivationPolicy:NSApplicationActivationPolicyRegular];
	[application finishLaunching];

	/* **A WINDOW THAT FITS THE SCREEN IT IS ON, UNLESS IT WAS TOLD HOW BIG TO BE.** A demo that opens at 640x480 on a
	 * 1280x940 output shows a third of a page and invites the question of whether the page or the window is broken — and
	 * the frame's ORIGIN IS THE BOTTOM-LEFT, so a hardcoded `y = 80` puts the window at the foot of the screen rather
	 * than near its top. Both are measured rather than stylistic: this is what the first screenshot of osnews.com showed.
	 */
	{
		NSScreen *screen = getenv("WK_WEBVIEW_NO_SCREEN") != NULL ? nil : [NSScreen mainScreen];
		NSRect visible = (screen != nil) ? [screen visibleFrame] : NSMakeRect(0.0, 0.0, 1280.0, 940.0);

		if (width <= 0 || height <= 0) {
			width = (int)(visible.size.width * 0.86);
			height = (int)(visible.size.height * 0.90);
		}
		content = NSMakeRect(visible.origin.x + (visible.size.width - (CGFloat)width) / 2.0,
				     visible.origin.y + (visible.size.height - (CGFloat)height) / 2.0,
				     (CGFloat)width, (CGFloat)height);
		if (at_x != INT_MIN && at_y != INT_MIN) {
			content.origin.x = (CGFloat)at_x;
			content.origin.y = (CGFloat)at_y;
		}
	}

	window = [[NSWindow alloc] initWithContentRect:content
					     styleMask:NSResizableWindowMask | NSTitledWindowMask | NSClosableWindowMask
					       backing:NSBackingStoreBuffered
						 defer:NO];
	if (getenv("WK_WEBVIEW_DIAG") != NULL) {
		fprintf(stderr, "  DIAG window: content %.0fx%.0f at (%.0f,%.0f), screen %.0fx%.0f at (%.0f,%.0f)\n",
			content.size.width, content.size.height, content.origin.x, content.origin.y,
			[NSScreen mainScreen] ? [[NSScreen mainScreen] visibleFrame].size.width : -1.0,
			[NSScreen mainScreen] ? [[NSScreen mainScreen] visibleFrame].size.height : -1.0,
			[NSScreen mainScreen] ? [[NSScreen mainScreen] visibleFrame].origin.x : -1.0,
			[NSScreen mainScreen] ? [[NSScreen mainScreen] visibleFrame].origin.y : -1.0);
	}
	[window setTitle:[NSString stringWithUTF8String:title]];
	view = [[WKWebView alloc] initWithFrame:NSMakeRect(0.0, 0.0, (CGFloat)width, (CGFloat)height)];
	[view setNavigationDelegate:delegate];
	[window setContentView:view];
	[window makeKeyAndOrderFront:nil];
	{
		AGTitleMirror *mirror = [[AGTitleMirror alloc] init];

		mirror->view = view;
		mirror->window = window;
		[NSTimer scheduledTimerWithTimeInterval:0.2
						 target:mirror
					       selector:@selector(tick:)
					       userInfo:nil
						repeats:YES];
	}
	if (getenv("WK_WEBVIEW_DIAG") != NULL) {
		fprintf(stderr, "  DIAG frame: %.0fx%.0f at (%.0f,%.0f), visible %d, content %.0fx%.0f\n",
			[window frame].size.width, [window frame].size.height, [window frame].origin.x, [window frame].origin.y,
			[window isVisible] ? 1 : 0, [view frame].size.width, [view frame].size.height);
	}

	if (url != nil) {
		[view loadRequest:[NSURLRequest requestWithURL:url]];
	} else {
		[view loadHTMLString:kDefaultPage baseURL:nil];
	}

	/* **THE RUN LOOP IS THE APPKIT'S, AND THE VIEW'S OWN TIMER DOES THE PUMPING.** `WKWebView` schedules an `NSTimer` that
	 * calls the engine each turn, so `-runMode:beforeDate:` here is enough to load a page and paint it — nothing in this
	 * program iterates GLib. */
	if (seconds > 0) {
		NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:(NSTimeInterval)seconds];

		/* **`-updateWindows` IS THE HALF THAT PAINTS, AND WITHOUT IT NOTHING DOES.** `-run` runs the run loop AND displays
		 * the windows that asked; a hand-pumped loop only runs the run loop, so the view's `-drawRect:` is never called
		 * and the window stays the colour of its background — MEASURED as an application that loaded its page, reported
		 * the right title, and painted nothing at all. */
		while ([deadline timeIntervalSinceNow] > 0.0) {
			[[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
						 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
			[application updateWindows];
		}
	} else {
		[application run];
	}

	printf("webview: %d commit(s), %d finish(es), title \"%s\", %s\n", delegate->committed, delegate->finished,
	       (char *)([[view title] UTF8String] ?: ""), [view isLoading] ? "still loading" : "loaded");
	[view release];
	[window release];
	[delegate release];
	[pool release];
	return delegate->finished > 0 ? 0 : 1;
}
