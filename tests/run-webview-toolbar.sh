#!/bin/bash
# Copyright (C) 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# **THE TOOLBAR, DRIVEN THE WAY A PERSON DRIVES IT.** The demonstration app opens on one page, a scripted hand clicks into
# the URL field, types a second page's address and presses Return, and then presses the Back button. Every step goes through
# the AppKit: the field session is `-makeFirstResponder:` and real `NSEvent` key presses via `-sendEvent:`, so the text tier's
# field editor is what inserts the characters, and the button's own action is what navigates.
#
# THE ASSERTION IS THE PAGE'S OWN TITLE, twice. `webview: the page says "..."` is the title the page sets, logged by the demo,
# so "the typed address loaded" and "back went to the first page" are claims about WebKit's behaviour rather than about the
# log line the demo wrote itself. The composed screenshot then has to show the FIRST page's colour on the glass, which is the
# same hop `webview-app` proves — reached this time by pressing a button rather than by starting the app on a URL.
set -u

BINARY="${1:?usage: run-webview-toolbar.sh BINARY WINDOW-SERVER}"
SERVER="${2:?}"
SOCK="agora-webview-toolbar-$$"
SHOT="${AGORA_SCREENSHOT:-/tmp/agora-webview-toolbar-$$.ppm}"
LOG="/tmp/agora-webview-toolbar-client-$$.log"
PAGES="/tmp/agora-webview-toolbar-pages-$$"
RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SCREEN_W=760
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
	rm -rf "${PAGES}"
	[ -n "${AGORA_SCREENSHOT:-}" ] || rm -f "${SHOT}"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"
mkdir -p "${PAGES}"
# **TWO PAGES THAT CANNOT BE CONFUSED**: different titles and different background colours, so both the log and the frame say
# which one is on screen.
cat >"${PAGES}/a.html" <<'HTML'
<html><head><title>toolbar A</title></head>
<body style="margin:0;background:#c00000;color:#ffffff;font:24px sans-serif">
<p style="padding:40px">Page A — this is the first page, reached by going back.</p></body></html>
HTML
cat >"${PAGES}/b.html" <<'HTML'
<html><head><title>toolbar B</title></head>
<body style="margin:0;background:#0000c0;color:#ffffff;font:24px sans-serif">
<p style="padding:40px">Page B — this is the page the URL field asked for.</p></body></html>
HTML

"${SERVER}" --backend x11 --input host --socket "${SOCK}" --width "${SCREEN_W}" --height "${SCREEN_H}" \
	--frames 1000000 --screenshot "${SHOT}" >/dev/null 2>/tmp/agora-webview-toolbar-server.log &
SPID=$!
for _ in $(seq 1 100); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
[ -S "${RUNTIME}/${SOCK}" ] || { echo "SKIP: the server never came up" >&2; exit 77; }

WAYLAND_DISPLAY="${SOCK}" "${BINARY}" --seconds 18 --size 700x480 \
	--type-url "file://${PAGES}/b.html" --back-after 2500 "file://${PAGES}/a.html" >"${LOG}" 2>&1 &
CPID=$!

fail() {
	echo "FAIL: $1" >&2
	cat "${LOG}" >&2
	exit 1
}
wait_for() {		# wait_for <text> <seconds>
	local _i
	for _i in $(seq 1 $(($2 * 10))); do
		grep -qF "$1" "${LOG}" 2>/dev/null && return 0
		kill -0 "${CPID}" 2>/dev/null || return 1
		sleep 0.1
	done
	return 1
}

wait_for "the page says \"toolbar A\"" 10 || fail "the first page never loaded"
wait_for "webview: typing" 5 || fail "the scripted hand never typed into the URL field"
wait_for "the page says \"toolbar B\"" 10 || fail "the typed address never loaded"
wait_for "webview: pressing back" 10 || fail "the Back button was never pressed"
# **AND BACK IS THE HARD ONE**: the log already contains A's title from the first load, so the assertion is that A comes back
# AFTER the back press — counted, not merely present.
for _ in $(seq 1 100); do
	[ "$(grep -cF 'the page says "toolbar A"' "${LOG}" 2>/dev/null)" -ge 2 ] && break
	kill -0 "${CPID}" 2>/dev/null || break
	sleep 0.1
done
[ "$(grep -cF 'the page says "toolbar A"' "${LOG}" 2>/dev/null)" -ge 2 ] || fail "back did not return to the first page"
echo "ok: the URL field loaded the typed address, and Back returned to the first page"

kill -TERM "${SPID}" 2>/dev/null
wait "${SPID}" 2>/dev/null
SPID=""
wait "${CPID}" 2>/dev/null
CPID=""
cat "${LOG}"

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


def has(rgb, tolerance=10):
    for y in range(0, height, 2):
        for x in range(0, width, 2):
            got = colour(x, y)
            if all(abs(got[i] - rgb[i]) <= tolerance for i in range(3)):
                return True
    return False


if not has((192, 0, 0)):
    sys.exit('FAIL: after Back the first page\'s red is not in the composed frame')
if not has((0, 0, 192)):
    print('note: the second page\'s blue is no longer on screen, which is what Back should do')
print('ok: the frame after Back is the first page')
PY
check=$?
[ "${check}" -eq 0 ] || exit 1
echo "ok: the toolbar navigated the demo with the URL field and the Back button"
exit 0
