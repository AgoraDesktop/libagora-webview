#!/bin/bash
# Copyright (C) 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# A page in a window's OWN GPU BUFFER, asserted on the composited screenshot.
#
# WHY A NESTED SESSION AND NOT A HEADLESS ONE: the window's buffers are dma-bufs only when the compositor advertises that it
# can import them, and only the GL renderer can — which on this machine needs a drawable, so `--backend x11 --renderer gl`.
# A headless server runs the CPU renderer and imports nothing, so a window there silently uses shared memory and this test
# would be asserting a path it never took.
#
# THE NESTED RUNNERS IN THIS TREE ARE OPT-IN (AGORA_ALLOW_NESTED=1) because they start a session on the host's display, so
# this SKIPS unless it is asked for — and also when there is no X display to nest on.
set -u

BINARY="${1:?usage: run-webengine-window.sh BINARY WINDOW-SERVER}"
SERVER="${2:?}"
SOCK="agora-webengine-window-$$"
SHOT="${AGORA_SCREENSHOT:-/tmp/agora-webengine-window-$$.ppm}"
LOG="/tmp/agora-webengine-window-client-$$.log"
RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SCREEN_W=400
SCREEN_H=300
WIN_W=320
WIN_H=200
WIN_Y=32

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

"${SERVER}" --backend x11 --renderer gl --input host --socket "${SOCK}" --width "${SCREEN_W}" --height "${SCREEN_H}" \
	--frames 1000000 --screenshot "${SHOT}" >/dev/null 2>/tmp/agora-webengine-window-server.log &
SPID=$!
for _ in $(seq 1 100); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
[ -S "${RUNTIME}/${SOCK}" ] || { echo "SKIP: the server never came up" >&2; exit 77; }

# **THE WINDOW ASKS FOR A GPU BUFFER BEFORE IT IS MADE**, which is what AG_WINDOW_DMABUF=1 says and the only moment the
# choice can be made: the buffers are allocated at the first frame.
AG_WINDOW_DMABUF=1 WAYLAND_DISPLAY="${SOCK}" "${BINARY}" >"${LOG}" 2>&1 &
CPID=$!
for _ in $(seq 1 300); do
	grep -q "webengine_window: ok\|SKIP\|no GPU frame" "${LOG}" 2>/dev/null && break
	kill -0 "${CPID}" 2>/dev/null || break
	sleep 0.1
done
sleep 0.5

if grep -q "SKIP" "${LOG}" 2>/dev/null; then
	kill "${SPID}" 2>/dev/null
	wait "${SPID}" 2>/dev/null
	SPID=""
	cat "${LOG}" >&2
	echo "SKIP: this machine's window has no GPU buffers" >&2
	exit 77
fi

kill -TERM "${SPID}" 2>/dev/null
wait "${SPID}" 2>/dev/null
SPID=""
wait "${CPID}" 2>/dev/null
status=$?
output=$(cat "${LOG}" 2>/dev/null)
echo "${output}"
# **THE CLIENT'S OWN SKIP IS THE ONE THAT COUNTS, AND IT ARRIVES LAST.** The poll above can see the log a moment before the
# line is flushed, so the same decision is made again here from the exit status: 77 is the program saying "this machine
# cannot bind a window's GBM buffer as a texture", which is a capability and not a failure.
if [ "${status}" -eq 77 ]; then
	echo "SKIP: this machine's GBM buffers do not import into an EGL context" >&2
	exit 77
fi
if [ "${status}" -ne 0 ]; then
	echo "FAIL: the client exited ${status}" >&2
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

checks = [
    ('the page top', at_window(300, 12), (255, 0, 0)),
    ('the page body', at_window(300, 150), (0, 0, 192)),
]
failed = False
for name, got, want in checks:
    if got != want:
        print(f'FAIL: {name} is {got} in the composited frame, want {want}', file=sys.stderr)
        failed = True
if failed:
    # **RED AND BLUE EXCHANGED IS NAMED, BECAUSE IT LOOKS LIKE A LOST FRAME AND IS NOT.** It used to be the compositor
    # swapping a dma-buf it had not uploaded (fixed with `u_bgr`); if it comes back, the message says so instead of
    # "the page is not there".
    if at_window(300, 12) == (0, 0, 255) and at_window(300, 150) == (192, 0, 0):
        print('FAIL: the page arrived with red and blue exchanged — the compositor is swapping an EGL-mapped buffer',
              file=sys.stderr)
        sys.exit(1)
    print(f'      (the screenshot is {path})', file=sys.stderr)
    sys.exit(1)
print('ok: the composited frame carries the page — red above, blue below')
PY
check=$?
if [ "${check}" -eq 77 ]; then
	exit 77
fi
[ "${check}" -eq 0 ] || exit 1
echo "ok: a page reached a window's GPU buffer without a readback"
exit 0
