#!/bin/sh
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
#
# One desktop workload, as "desk" in the Xfce session (desktop-up.sh run). It
# drives input with xdotool at 60 Hz, the way a person would, and prints one
# "metric NAME VALUE" line where the workload has a throughput of its own.
#
#   lag-wl.sh idle|drag|term|firefox|menus|drag-gl
set -u
W=${1:?workload}
DT=0.016

term_window() {   # an 80x24 terminal at a known place; prints its window id
	xfce4-terminal --geometry=80x24+120+120 --title=lagterm --hold -x sh -c 'echo lag; exec sleep 600' &
	# (the first terminal of a session takes a while to map on these Atoms)
	for i in $(seq 1 120); do
		id=$(xdotool search --name '^lagterm$' 2>/dev/null | head -1)
		[ -n "$id" ] && { sleep 1; echo "$id"; return; }
		sleep 0.5
	done
	echo "lag-wl.sh: no terminal window in 60 s" >&2
	exit 1
}

drag() {   # the window by its title bar: 300 steps of (3, 2) and back
	id=$1
	eval "$(xdotool getwindowgeometry --shell "$id")"
	xdotool mousemove $((X + 150)) $((Y - 12))
	sleep 0.3
	xdotool mousedown 1
	for i in $(seq 1 150); do xdotool mousemove_relative -- 3 2; sleep $DT; done
	for i in $(seq 1 150); do xdotool mousemove_relative -- -3 -2; sleep $DT; done
	xdotool mouseup 1
}

case $W in
idle)
	sleep 15
	;;
drag)
	id=$(term_window) || exit 1
	t0=$(date +%s.%N)
	drag "$id"
	echo "metric drag_s $(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN { print b - a }')"
	xdotool windowkill "$id"
	;;
drag-gl)   # the same drag while a GL client draws in its own window
	glmark2 --size 400x300 -b build:use-vbo=true --run-forever > /dev/null 2>&1 &
	g=$!
	sleep 8
	id=$(term_window) || { kill $g; exit 1; }
	drag "$id"
	xdotool windowkill "$id"
	kill $g
	;;
term)   # text through glamor: 60000 lines into a terminal, timed
	rm -f /tmp/lagterm.t
	xfce4-terminal --geometry=100x40+60+60 --title=lagscroll -x sh -c \
		's=$(date +%s.%N); seq -f "line %06g of text through the terminal, glyph by glyph" 1 60000; \
		 echo "$(date +%s.%N) $s" > /tmp/lagterm.t' &
	for i in $(seq 1 240); do [ -s /tmp/lagterm.t ] && break; sleep 0.5; done
	read e s < /tmp/lagterm.t
	echo "metric term_lines_per_s $(awk -v e="$e" -v s="$s" 'BEGIN { printf "%.0f", 60000 / (e - s) }')"
	;;
firefox)   # a long page scrolled by key and by wheel
	p=/tmp/lag.html
	{ echo '<html><body style="font: 15px sans-serif">'
	  for i in $(seq 1 300); do
		echo "<h2>Section $i</h2><p style=\"background: linear-gradient(90deg, #fee, #eef)\">"
		echo "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris.</p>"
	  done; echo '</body></html>'; } > $p
	rm -rf /tmp/lagff; mkdir -p /tmp/lagff
	firefox-esr --new-instance --profile /tmp/lagff -width 1000 -height 700 "file://$p" > /tmp/lagff.log 2>&1 &
	sleep 25
	id=$(xdotool search --class firefox 2>/dev/null | tail -1)
	xdotool windowactivate --sync "$id" 2>/dev/null; sleep 1
	t0=$(date +%s.%N)
	for i in $(seq 1 40); do xdotool key Page_Down; sleep 0.1; done
	for i in $(seq 1 120); do xdotool click 4; sleep 0.03; done
	echo "metric firefox_scroll_s $(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN { print b - a }')"
	pkill -f 'firefox-esr --new-instance'
	;;
menus)   # the panel's applications menu and the desktop menu, opened and closed
	for i in $(seq 1 10); do
		xfce4-popup-applicationsmenu; sleep 0.6; xdotool key Escape; sleep 0.4
		xdotool mousemove 700 500 click 3; sleep 0.6; xdotool key Escape; sleep 0.4
	done
	;;
*)
	echo "lag-wl.sh: no workload $W" >&2
	exit 2
	;;
esac
