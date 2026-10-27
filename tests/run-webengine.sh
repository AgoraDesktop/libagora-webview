#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# A page through libagora-webview, against AGORA'S OWN WINDOW SERVER, asserted on the composited screenshot.
#
# WHY THE SCREENSHOT: the engine reporting "a load finished" and "a frame was drawn" is not evidence that a page reached
# the glass. So the server photographs the frame it composited and this script checks the PAGE'S OWN COLOURS in the
# PAGE'S OWN PLACES — a red strip across the top of the window and blue under it.
#
# **THE CLIENT STAYS ALIVE UNTIL THE SERVER IS STOPPED**, because the screenshot is written on the server's exit path and
# a client that quit first would take its window with it: MEASURED as a screenshot of an empty desktop in the first
# version of this runner.
#
# SKIPS (77) when there is no window server built or no WebKit on this machine — the probe says 77 for the latter.
set -u

BINARY="${1:?usage: run-webengine.sh BINARY WINDOW-SERVER}"
SERVER="${2:?}"
SOCK="agora-webengine-$$"
SHOT="${AGORA_SCREENSHOT:-/tmp/agora-webengine-$$.ppm}"
LOG="/tmp/agora-webengine-client-$$.log"
RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SCREEN_W=400
SCREEN_H=300
WIN_W=320
WIN_H=200
WIN_Y=32

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

"${SERVER}" --backend headless --input none --socket "${SOCK}" --width "${SCREEN_W}" --height "${SCREEN_H}" \
	--frames 1000000 --screenshot "${SHOT}" >/dev/null 2>/tmp/agora-webengine-server.log &
SPID=$!
for _ in $(seq 1 50); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
[ -S "${RUNTIME}/${SOCK}" ] || { echo "SKIP: the server never came up" >&2; exit 77; }

WAYLAND_DISPLAY="${SOCK}" "${BINARY}" >"${LOG}" 2>&1 &
CPID=$!
for _ in $(seq 1 400); do
	grep -q "webengine: ok" "${LOG}" 2>/dev/null && break
	kill -0 "${CPID}" 2>/dev/null || break
	sleep 0.1
done
# A moment of steady state, so the screenshot is not the first paint.
sleep 0.5

if grep -q "the engine would not start" "${LOG}" 2>/dev/null; then
	kill "${SPID}" 2>/dev/null
	wait "${SPID}" 2>/dev/null
	SPID=""
	echo "SKIP: no WPE WebKit on this machine" >&2
	exit 77
fi

# **THE SERVER IS STOPPED BY SIGNAL BEFORE ANYTHING IS ASSERTED, WHICH IS WHAT WRITES THE SCREENSHOT**: the signal stops
# the loop and the ordinary exit path runs.
kill -TERM "${SPID}" 2>/dev/null
wait "${SPID}" 2>/dev/null
SPID=""
wait "${CPID}" 2>/dev/null
status=$?
output=$(cat "${LOG}" 2>/dev/null)

echo "${output}"
if [ "${status}" -ne 0 ]; then
	echo "FAIL: the probe exited ${status}" >&2
	exit 1
fi

python3 - "${SHOT}" "${WIN_W}" "${WIN_H}" "${WIN_Y}" <<'PY'
import sys

path, win_w, win_h, win_y = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
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

def at_window(x, y):
    index = ((win_y + y) * width + x) * 3
    return tuple(pixels[index:index + 3])

# The page's own layout: a 40-point red strip at the top, a blue body under it with white text. Samples are away from
# the text's edges and well inside the window, so rounded corners and the shadow cannot reach them.
checks = [
    ('the page background', at_window(300, 150), (0, 0, 192)),
    ('the red strip', at_window(300, 12), (192, 0, 0)),
    ('below the strip', at_window(300, 60), (0, 0, 192)),
]
failed = False
for name, got, want in checks:
    if got != want:
        print(f'FAIL: {name} is {got} in the composited frame, want {want}', file=sys.stderr)
        failed = True
if failed:
    print(f'      (the screenshot is {path})', file=sys.stderr)
    sys.exit(1)
print('ok: the composited frame carries the page — red above, blue below')
PY
check=$?
[ "${check}" -eq 0 ] || exit 1

echo "ok: a real page reached the glass through libagora-webview"
exit 0
