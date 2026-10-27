#!/bin/bash
# Copyright (C) 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# The demo APPLICATION: an NSApplication with an NSWindow and a WKWebView in it, on a real compositor, asserted on the
# composed screenshot.
#
# WHY THIS AND NOT THE CLASS TEST: `appkit-webview` proves the class offscreen, with a bitmap rep for a surface and no
# compositor at all. This proves the last hop — AppKit's paint into a window's shared-memory buffer, the compositor's
# composite, and the page's own colours on the glass — which is the hop every previous test stops short of. It is nested and
# opt-in for the same reason the other nested runners are: it starts a session on the host's display.
set -u

BINARY="${1:?usage: run-webview-app.sh BINARY WINDOW-SERVER}"
SERVER="${2:?}"
SOCK="agora-webview-app-$$"
SHOT="${AGORA_SCREENSHOT:-/tmp/agora-webview-app-$$.ppm}"
LOG="/tmp/agora-webview-app-client-$$.log"
RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SCREEN_W=720
SCREEN_H=560

if [ "${AGORA_ALLOW_NESTED:-0}" != "1" ]; then
	echo "SKIP: nested runs are opt-in (AGORA_ALLOW_NESTED=1)" >&2
	exit 77
fi
if [ -z "${DISPLAY:-}" ]; then
	echo "SKIP: no X display to nest on" >&2
	exit 77
fi
if [ ! -x "${SERVER}" ]; then
	echo "SKIP: no window server at ${SERVER}" >&2
	exit 77
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "SKIP: python3 is needed to read the screenshot" >&2
	exit 77
fi

cleanup() {
	[ -n "${CPID:-}" ] && kill "${CPID}" 2>/dev/null
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	[ -n "${CPID:-}" ] && wait "${CPID}" 2>/dev/null
	[ -n "${SPID:-}" ] && wait "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${LOG}"
	[ -n "${AGORA_SCREENSHOT:-}" ] || rm -f "${SHOT}"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"

"${SERVER}" --backend x11 --input host --socket "${SOCK}" --width "${SCREEN_W}" --height "${SCREEN_H}" \
	--frames 1000000 --screenshot "${SHOT}" >/dev/null 2>/tmp/agora-webview-app-server.log &
SPID=$!
for _ in $(seq 1 100); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
[ -S "${RUNTIME}/${SOCK}" ] || { echo "SKIP: the server never came up" >&2; exit 77; }

WAYLAND_DISPLAY="${SOCK}" "${BINARY}" --seconds 8 --size 640x480 >"${LOG}" 2>&1 &
CPID=$!
# The application stays up for its eight seconds; once the load has finished the screenshot is worth taking.
for _ in $(seq 1 120); do
	grep -q "the page finished loading" "${LOG}" 2>/dev/null && break
	kill -0 "${CPID}" 2>/dev/null || break
	sleep 0.1
done
sleep 0.7

if ! grep -q "the page finished loading" "${LOG}" 2>/dev/null; then
	kill "${CPID}" 2>/dev/null
	wait "${CPID}" 2>/dev/null
	CPID=""
	kill -TERM "${SPID}" 2>/dev/null
	wait "${SPID}" 2>/dev/null
	SPID=""
	cat "${LOG}" >&2
	echo "FAIL: the application never loaded its page" >&2
	exit 1
fi

kill -TERM "${SPID}" 2>/dev/null
wait "${SPID}" 2>/dev/null
SPID=""
wait "${CPID}" 2>/dev/null
status=$?
output=$(cat "${LOG}" 2>/dev/null)
echo "${output}"
if [ "${status}" -ne 0 ]; then
	echo "FAIL: the application exited ${status}" >&2
	exit 1
fi

python3 - "${SHOT}" <<'PY'
import sys

path = sys.argv[1]
data = open(path, 'rb').read()
if data[:2] != b'P6':
    sys.exit(f'FAIL: {path} is not a binary PPM')
fields, at = [], 2
while len(fields) < 3:
    while data[at:at + 1].isspace():
        at += 1
    start = at
    while not data[at:at + 1].isspace():
        at += 1
    fields.append(int(data[start:at]))
at += 1
width, height, _ = fields
pixels = data[at:]

def colour(x, y):
    index = (y * width + x) * 3
    return tuple(pixels[index:index + 3])

# **THE PAGE'S OWN COLOURS, FOUND ANYWHERE.** The window is placed by the application (60 points in, 80 down) and the screen
# is scaled by the compositor on this machine, so a fixed coordinate would encode two guesses. What is asserted instead is
# that the page's red band and its blue body are BOTH on the glass — which cannot happen unless AppKit painted the view's
# image into the window's buffer and the compositor sampled it.
def has(rgb, tolerance=8):
    for y in range(0, height, 2):
        for x in range(0, width, 2):
            got = colour(x, y)
            if all(abs(got[i] - rgb[i]) <= tolerance for i in range(3)):
                return True
    return False

checks = [('the page\'s red band', (192, 0, 0)), ('the page\'s blue body', (0, 0, 192))]
failed = False
for name, rgb in checks:
    if not has(rgb):
        print(f'FAIL: {name} is not in the composed frame', file=sys.stderr)
        failed = True
if failed:
    print(f'      (the screenshot is {path})', file=sys.stderr)
    sys.exit(1)
print('ok: the page reached the glass through AppKit')
PY
check=$?
[ "${check}" -eq 0 ] || exit 1
echo "ok: the demo application painted a page on the desktop"
exit 0
