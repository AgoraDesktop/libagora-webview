#!/bin/bash
# Copyright (C) 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# `webview_resize` against a compositor at --scale 2, because neither of the two things it asserts can be seen at scale 1:
# a page rendered at 1x and resampled looks exactly like a sharp one when 1x IS the output, and a stretched frame is
# indistinguishable from a reflowed page when nothing was scaled to begin with. Nested and opt-in, like the other runners
# that need a session.
set -u

BINARY="${1:?usage: run-webview-resize.sh BINARY WINDOW-SERVER}"
SERVER="${2:?}"
SOCK="agora-webview-resize-$$"
LOG="/tmp/agora-webview-resize-client-$$.log"
RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"

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

cleanup() {
	[ -n "${CPID:-}" ] && kill "${CPID}" 2>/dev/null
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	[ -n "${CPID:-}" ] && wait "${CPID}" 2>/dev/null
	[ -n "${SPID:-}" ] && wait "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${LOG}"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"

# 600x500 points at scale 2 is a 1200x1000 device output, which holds the 520x400 window the probe resizes itself to.
"${SERVER}" --backend x11 --input host --socket "${SOCK}" --width 600 --height 500 --scale 2 \
	--frames 1000000 >/dev/null 2>/tmp/agora-webview-resize-server.log &
SPID=$!
for _ in $(seq 1 100); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
[ -S "${RUNTIME}/${SOCK}" ] || { echo "SKIP: the server never came up" >&2; exit 77; }

WAYLAND_DISPLAY="${SOCK}" "${BINARY}" >"${LOG}" 2>&1 &
CPID=$!
for _ in $(seq 1 200); do
	grep -q "resize: ok\|resize: [0-9]* check" "${LOG}" 2>/dev/null && break
	kill -0 "${CPID}" 2>/dev/null || break
	sleep 0.1
done
kill "${CPID}" 2>/dev/null
wait "${CPID}" 2>/dev/null
status=$?
CPID=""
# The probe prints "resize: ok" on success, so an exit code is not the only evidence — a killed run can still have passed.
output=$(cat "${LOG}" 2>/dev/null)
echo "${output}"
if printf '%s\n' "${output}" | grep -q "resize: ok"; then
	exit 0
fi
if [ "${status}" -eq 77 ]; then
	exit 77
fi
echo "FAIL: the probe did not finish (exit ${status})" >&2
exit 1
