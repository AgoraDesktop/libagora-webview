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
#import <AppKit/NSButton.h>
#import <AppKit/NSTextField.h>
#import <AppKit/NSToolbar.h>
#import <AppKit/NSToolbarItem.h>
#import <Foundation/NSArray.h>

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


/* **THE CHROME: TWO BUTTONS AND A FIELD, IN AN `NSToolbar`.** The window already knows how to reserve a toolbar's band
 * (`-setToolbar:`); what it does not know is what the items should be, so that decision lives here.
 *
 * THE FIELD IS A REAL `NSTextField`, and that is a claim worth stating because it means the AppKit's field editor is doing
 * the typing: a click starts a session, keys go through `-keyDown:` to the editor, Return ends the session and sends the
 * control's action — so this toolbar exercises the text tier rather than drawing a box that looks like one.
 *
 * THE BACK AND FORWARD BUTTONS ASK BEFORE THEY ACT. `-canGoBack` greys them out; a button that stays lit and does nothing
 * when the history is empty is the classic way a back button "doesn't work" for a user while being perfectly correct in
 * the code. The buttons and the field are re-synced from the view ten times a second, which is the same rate a person
 * notices and far cheaper than a callback through the seam. */
@interface AGBrowserToolbar : NSObject <NSToolbarDelegate>
{
@public
	WKWebView *view;
	NSWindow *window;
	NSButton *back;
	NSButton *forward;
	NSTextField *field;
	NSToolbarItem *backItem;
	NSToolbarItem *forwardItem;
	NSToolbarItem *fieldItem;
	NSString *shown;
}
- (NSToolbar *)makeToolbar;
- (void)tick:(NSTimer *)timer;
@end

@implementation AGBrowserToolbar
- (NSToolbar *)makeToolbar
{
	NSToolbar *toolbar;

	back = [[NSButton alloc] initWithFrame:NSMakeRect(0.0, 0.0, 34.0, 24.0)];
	[back setTitle:@"\u25c0"];		/* ◀ — a triangle draws in every font this stack has; an SF Symbol does not */
	[back setBezelStyle:NSRoundedBezelStyle];
	[back setTarget:self];
	[back setAction:@selector(goBack:)];
	[back setEnabled:NO];

	forward = [[NSButton alloc] initWithFrame:NSMakeRect(0.0, 0.0, 34.0, 24.0)];
	[forward setTitle:@"\u25b6"];		/* ▶ */
	[forward setBezelStyle:NSRoundedBezelStyle];
	[forward setTarget:self];
	[forward setAction:@selector(goForward:)];
	[forward setEnabled:NO];

	field = [[NSTextField alloc] initWithFrame:NSMakeRect(0.0, 0.0, 460.0, 24.0)];
	[field setEditable:YES];
	[field setSelectable:YES];
	[field setBezeled:YES];
	[field setStringValue:@""];
	[field setTarget:self];
	[field setAction:@selector(enterURL:)];

	backItem = [[NSToolbarItem alloc] initWithItemIdentifier:@"back"];
	[backItem setLabel:@"Back"];
	[backItem setView:back];

	forwardItem = [[NSToolbarItem alloc] initWithItemIdentifier:@"forward"];
	[forwardItem setLabel:@"Forward"];
	[forwardItem setView:forward];

	/* **THE FIELD TAKES THE ROOM THE BUTTONS LEAVE.** A toolbar item reports a minimum and a maximum width and the bar
	 * gives it as much as it can up to the maximum; a maximum of 10000 is how this tier spells "as wide as possible". */
	fieldItem = [[NSToolbarItem alloc] initWithItemIdentifier:@"url"];
	[fieldItem setLabel:@"URL"];
	[fieldItem setView:field];
	[fieldItem setMinSize:NSMakeSize(160.0, 24.0)];
	[fieldItem setMaxSize:NSMakeSize(10000.0, 24.0)];

	toolbar = [[NSToolbar alloc] initWithIdentifier:@"agora-browser"];
	[toolbar setDelegate:self];
	[toolbar setDisplayMode:NSToolbarDisplayModeIconOnly];
	return toolbar;
}

- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar
     itemForItemIdentifier:(NSString *)identifier
 willBeInsertedIntoToolbar:(BOOL)flag
{
	(void)toolbar;
	(void)flag;
	if ([identifier isEqualToString:@"back"]) {
		return backItem;
	}
	if ([identifier isEqualToString:@"forward"]) {
		return forwardItem;
	}
	if ([identifier isEqualToString:@"url"]) {
		return fieldItem;
	}
	return nil;
}

- (NSArray *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar
{
	(void)toolbar;
	return [NSArray arrayWithObjects:@"back", @"forward", @"url", nil];
}

- (NSArray *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar
{
	(void)toolbar;
	return [NSArray arrayWithObjects:@"back", @"forward", @"url", nil];
}

/* **WHAT A TYPED URL MEANS.** People type `example.com`, not `https://example.com`, and a field that refuses the first is a
 * field that appears broken. A string with no scheme gets `https://`; anything that already parses as a URL is left alone,
 * which keeps `file:`, `data:` and `about:` usable — the tests and the demo's own default page need two of those. */
- (void)enterURL:(id)sender
{
	NSString *text = [field stringValue];
	NSURL *url;

	(void)sender;
	if (text == nil || [text length] == 0) {
		return;
	}
	url = [NSURL URLWithString:text];
	if (url == nil || [url scheme] == nil) {
		text = [@"https://" stringByAppendingString:text];
		url = [NSURL URLWithString:text];
	}
	if (url == nil) {
		return;
	}
	fprintf(stderr, "webview: typed \"%s\"\n", [text UTF8String]);
	fflush(stderr);
	shown = nil;		/* let the next tick write the URI WebKit actually commits, not what was typed */
	[window makeFirstResponder:view];
	[view loadRequest:[NSURLRequest requestWithURL:url]];
}

- (void)goBack:(id)sender
{
	(void)sender;
	fprintf(stderr, "webview: back\n");
	fflush(stderr);
	shown = nil;
	[view goBack];
}

- (void)goForward:(id)sender
{
	(void)sender;
	fprintf(stderr, "webview: forward\n");
	fflush(stderr);
	shown = nil;
	[view goForward];
}

- (void)tick:(NSTimer *)timer
{
	NSURL *url;
	NSString *text;

	(void)timer;
	if (view == nil) {
		return;
	}
	[back setEnabled:[view canGoBack]];
	[forward setEnabled:[view canGoForward]];
	/* **AND THE FIELD FOLLOWS THE VIEW, UNLESS SOMEONE IS TYPING IN IT.** Overwriting a half-typed address because a
	 * previous page finished loading is the sort of thing that makes a URL bar feel hostile; `-currentEditor` is non-nil
	 * exactly while a field editor is running, which is the AppKit's own answer to "is this field in use". */
	if ([field currentEditor] != nil) {
		return;
	}
	url = [view URL];
	text = (url != nil) ? [url absoluteString] : @"";
	if (shown == nil || ![shown isEqualToString:text]) {
		[shown release];
		shown = [text retain];
		[field setStringValue:text];
	}
}
@end


/* **A SCRIPTED HAND, SO THE TOOLBAR CAN BE TESTED WITHOUT ONE.** Everything below is a person's gesture in the order a
 * person makes it: click in the field, type, press Return, then press Back. The keys are real `NSEvent`s delivered through
 * `-sendEvent:`, so the AppKit's field editor takes them the way it takes a keyboard's — a test that called
 * `-loadRequest:` directly would prove the engine and nothing about the field. `--type-url` and `--back-after` are how a
 * runner asks for it; both are off by default and print what they did, so a log says what the run meant to prove. */
@interface AGToolbarScript : NSObject
{
@public
	NSString *text;
	int back_after_ms;
	NSWindow *window;
	WKWebView *view;
	AGBrowserToolbar *toolbar;
	NSTextField *field;
	NSButton *back;
	int typed;
	int backed;
	double typed_at;
}
- (void)tick:(NSTimer *)timer;
- (void)typeText:(NSString *)string;
@end

@implementation AGToolbarScript
- (void)typeText:(NSString *)string
{
	NSUInteger i;
	NSUInteger length = [string length];

	/* **THE CLICK THAT STARTS A SESSION IS `-makeFirstResponder:`.** A field's text lives in the window's field editor
	 * once a session begins, and keys reach it only while it is the first responder — so this is the same step a person's
	 * click performs, without a pointer. */
	[window makeFirstResponder:field];
	for (i = 0; i < length; i++) {
		NSString *one = [string substringWithRange:NSMakeRange(i, 1)];
		NSEvent *key = [NSEvent keyEventWithType:NSKeyDown
						location:NSMakePoint(0.0, 0.0)
					   modifierFlags:0
					       timestamp:0.0
					    windowNumber:0
						 context:nil
					      characters:one
				 charactersIgnoringModifiers:one
					   isARepeat:NO
					     keyCode:0];

		[window sendEvent:key];
	}
	{
		/* RETURN IS WHAT SENDS THE FIELD'S ACTION — see `NSTextField`, where Return and Tab call the control's target. */
		NSEvent *ret = [NSEvent keyEventWithType:NSKeyDown
						location:NSMakePoint(0.0, 0.0)
					   modifierFlags:0
					       timestamp:0.0
					    windowNumber:0
						 context:nil
					      characters:@"\r"
				 charactersIgnoringModifiers:@"\r"
					   isARepeat:NO
					     keyCode:36];

		[window sendEvent:ret];
	}
}

- (void)tick:(NSTimer *)timer
{
	(void)timer;
	if (!typed && text != nil && [view title] != nil) {
		fprintf(stderr, "webview: typing \"%s\"\n", [text UTF8String]);
		fflush(stderr);
		[self typeText:text];
		typed = 1;
		typed_at = [NSDate timeIntervalSinceReferenceDate];
		return;
	}
	if (typed && back_after_ms > 0 && !backed &&
	    [NSDate timeIntervalSinceReferenceDate] - typed_at >= (double)back_after_ms / 1000.0) {
		fprintf(stderr, "webview: pressing back\n");
		fflush(stderr);
		[back performClick:nil];
		backed = 1;
	}
}
@end

int main(int argc, char **argv)
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	AGWebViewDelegate *delegate = [[AGWebViewDelegate alloc] init];
	NSApplication *application = [NSApplication sharedApplication];
	NSWindow *window;
	WKWebView *view;
	AGBrowserToolbar *toolbar;
	NSURL *url = nil;
	NSString *type_url = nil;
	int back_after_ms = 0;
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
		} else if (strcmp(argv[i], "--type-url") == 0 && i + 1 < argc) {
			type_url = [NSString stringWithUTF8String:argv[++i]];
		} else if (strcmp(argv[i], "--back-after") == 0 && i + 1 < argc) {
			back_after_ms = atoi(argv[++i]);
		} else if (argv[i][0] != '-') {
			url = [NSURL URLWithString:[NSString stringWithUTF8String:argv[i]]];
		} else {
			fprintf(stderr, "usage: %s [URL] [--seconds N] [--size WxH] [--at X,Y]"
					" [--type-url URL] [--back-after MS]\n", argv[0]);
			return 2;
		}
	}

	toolbar = nil;
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
	/* **THE CONTENT VIEW IS A CONTAINER, BECAUSE THE TOOLBAR TAKES A BAND OFF THE TOP.** With `-setToolbar:` the window
	 * reserves that band itself and hands the rest to its content view; the web view inside is asked to follow the
	 * container's size, which is what `NSViewWidthSizable|NSViewHeightSizable` means here. */
	{
		NSRect bounds;
		NSView *container;

		bounds = NSMakeRect(0.0, 0.0, (CGFloat)width, (CGFloat)height);
		container = [[NSView alloc] initWithFrame:bounds];
		view = [[WKWebView alloc] initWithFrame:bounds];
		[view setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
		[container addSubview:view];
		[window setContentView:container];
	}
	[view setNavigationDelegate:delegate];
	{
		AGBrowserToolbar *bar = [[AGBrowserToolbar alloc] init];

		toolbar = bar;
		bar->view = view;
		bar->window = window;
		[window setToolbar:[bar makeToolbar]];
		[NSTimer scheduledTimerWithTimeInterval:0.1
						 target:bar
					       selector:@selector(tick:)
					       userInfo:nil
						repeats:YES];
		/* **AND THE WEB VIEW IS TOLD THE SIZE THE TOOLBAR LEFT IT**, because the window's content rect shrank when the
		 * toolbar was attached and nothing else will tell the view. A view that keeps the old height draws the page
		 * under the toolbar's band. */
		{
			NSRect content = [window contentRectForFrameRect:[window frame]];

			[view setFrame:NSMakeRect(0.0, 0.0, content.size.width, content.size.height)];
		}
	}
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

	if (type_url != nil && toolbar != nil) {
		AGToolbarScript *script = [[AGToolbarScript alloc] init];

		script->text = [type_url retain];
		script->back_after_ms = back_after_ms;
		script->window = window;
		script->view = view;
		script->field = toolbar->field;
		script->back = toolbar->back;
		[NSTimer scheduledTimerWithTimeInterval:0.1
						 target:script
					       selector:@selector(tick:)
					       userInfo:nil
						repeats:YES];
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
