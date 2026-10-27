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

#import <Foundation/NSAutoreleasePool.h>
#import <Foundation/NSDate.h>
#import <Foundation/NSRunLoop.h>
#import <Foundation/NSURL.h>
#import <Foundation/NSString.h>

#include <stdio.h>
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
	const char *title = "Agora web view";
	int seconds = 0;
	int width = 640;
	int height = 480;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
			seconds = atoi(argv[++i]);
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
			fprintf(stderr, "usage: %s [URL] [--seconds N] [--size WxH]\n", argv[0]);
			return 2;
		}
	}

	[application setActivationPolicy:NSApplicationActivationPolicyRegular];
	[application finishLaunching];

	window = [[NSWindow alloc] initWithContentRect:NSMakeRect(60.0, 80.0, (CGFloat)width, (CGFloat)height)
					     styleMask:NSResizableWindowMask | NSTitledWindowMask | NSClosableWindowMask
					       backing:NSBackingStoreBuffered
						 defer:NO];
	[window setTitle:[NSString stringWithUTF8String:title]];
	view = [[WKWebView alloc] initWithFrame:NSMakeRect(0.0, 0.0, (CGFloat)width, (CGFloat)height)];
	[view setNavigationDelegate:delegate];
	[window setContentView:view];
	[window makeKeyAndOrderFront:nil];

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
