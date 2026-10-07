#!/bin/sh
set -e

# Majestic's SigmaStar speaker control expects an exported GPIO; it owns the value afterward.
PIN=/sys/class/gpio/gpio43
[ -d "$PIN" ] || echo 43 > /sys/class/gpio/export
[ "$(cat "$PIN/direction")" = out ] || echo low > "$PIN/direction"
