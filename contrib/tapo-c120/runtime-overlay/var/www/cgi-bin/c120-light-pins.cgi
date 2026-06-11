#!/bin/sh

CONF=/etc/c120-light-pins.conf

header() {
	echo "HTTP/1.1 200 OK
Content-type: text/html
Cache-Control: no-store
Pragma: no-cache
"
}

html_escape() {
	sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g; s/"/\&quot;/g'
}

param() {
	name="$1="
	old_ifs="$IFS"
	IFS='&'
	set -- ${QUERY_STRING:-}
	IFS="$old_ifs"
	for part do
		case "$part" in
			"$name"*)
				printf '%s\n' "${part#"$name"}"
				return 0
				;;
		esac
	done
}

decode_pin_text() {
	printf '%s' "$1" |
		sed 's/+/ /g; s/%2[Cc]/,/g; s/%20/ /g; s/%09/ /g'
}

normalize_pins() {
	printf '%s\n' "$1" |
		tr ',;' '  ' |
		tr -s '[:space:]' ' ' |
		sed 's/^ //; s/ $//'
}

valid_pins() {
	pins="$1"
	[ -n "$pins" ] || return 1
	for pin in $pins; do
		case "$pin" in
			''|*[!0-9]*)
				return 1
				;;
		esac
		[ "$pin" -le 255 ] 2>/dev/null || return 1
	done
	return 0
}

leader_pin() {
	set -- $1
	printf '%s\n' "${1:-}"
}

current_pins() {
	C120_CAMERA_LIGHT_PINS="12 13"
	[ -f "$CONF" ] && . "$CONF"
	printf '%s\n' "$C120_CAMERA_LIGHT_PINS"
}

current_poll() {
	C120_LIGHT_PINS_POLL="0.5"
	[ -f "$CONF" ] && . "$CONF"
	printf '%s\n' "$C120_LIGHT_PINS_POLL"
}

current_exclusive() {
	C120_LIGHT_PINS_RESPECT_EXCLUSIVE="1"
	[ -f "$CONF" ] && . "$CONF"
	printf '%s\n' "$C120_LIGHT_PINS_RESPECT_EXCLUSIVE"
}

write_config() {
	pins="$1"
	poll="$2"
	exclusive="$3"
	tmp="$CONF.$$"

	cat > "$tmp" <<EOF
# First pin is the Majestic camera-light/backlight leader.
# Remaining pins mirror the leader unless c120-lamps is in exclusive 850 or 940 mode.
C120_CAMERA_LIGHT_PINS="$pins"
C120_LIGHT_PINS_POLL="$poll"
C120_LIGHT_PINS_RESPECT_EXCLUSIVE="$exclusive"
C120_LIGHT_PINS_LOG="0"
EOF
	mv "$tmp" "$CONF"
	chmod 644 "$CONF"
}

header

message=""
error=""
pins="$(current_pins)"
poll="$(current_poll)"
exclusive="$(current_exclusive)"

if [ "$(param apply)" = "1" ]; then
	raw_pins="$(decode_pin_text "$(param pins)")"
	new_pins="$(normalize_pins "$raw_pins")"
	new_poll="$(decode_pin_text "$(param poll)")"
	new_exclusive="$(param exclusive)"
	[ -n "$new_poll" ] || new_poll="0.2"
	[ "$new_exclusive" = "1" ] || new_exclusive="0"

	case "$new_poll" in
		''|*[!0-9.]*)
			error="Invalid poll value"
			;;
	esac

	if [ -z "$error" ] && ! valid_pins "$new_pins"; then
		error="Invalid GPIO list"
	fi

	if [ -z "$error" ]; then
		write_config "$new_pins" "$new_poll" "$new_exclusive"
		leader="$(leader_pin "$new_pins")"
		cli -s .nightMode.backlightPin "$leader" >/dev/null 2>&1 || true
		if [ -x /etc/init.d/S46c120-light-pins ]; then
			/etc/init.d/S46c120-light-pins restart >/dev/null 2>&1 || true
		fi
		/usr/bin/c120-light-pinsd sync >/dev/null 2>&1 || true
		pins="$new_pins"
		poll="$new_poll"
		exclusive="$new_exclusive"
		message="Saved"
	fi
fi

pins_html="$(printf '%s' "$pins" | html_escape)"
poll_html="$(printf '%s' "$poll" | html_escape)"

cat <<HTML
<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>C120 Light Pins</title>
<style>
body{font-family:sans-serif;margin:24px;max-width:560px;background:#25292c;color:#f4f7fb}
a{color:#58a6ff}
label{display:block;margin:16px 0 6px}
input{font:inherit;width:100%;box-sizing:border-box;padding:10px;background:#202428;color:#f4f7fb;border:1px solid #4b5560;border-radius:6px}
button{font:inherit;margin-top:18px;padding:10px 14px;background:#0d6efd;color:#fff;border:0;border-radius:6px}
.ok{padding:10px;background:#173f2a;border:1px solid #2ea043}
.err{padding:10px;background:#4a1f1f;border:1px solid #d14}
pre{background:#343a40;padding:12px;overflow:auto}
</style>
</head>
<body>
<h1>C120 Light Pins</h1>
HTML

[ -n "$message" ] && printf '<p class="ok">%s</p>\n' "$(printf '%s' "$message" | html_escape)"
[ -n "$error" ] && printf '<p class="err">%s</p>\n' "$(printf '%s' "$error" | html_escape)"

cat <<HTML
<form method="get" action="/cgi-bin/c120-light-pins.cgi">
<input type="hidden" name="apply" value="1">
<label for="pins">Camera light GPIO pins</label>
<input id="pins" name="pins" value="$pins_html" inputmode="text" pattern="[0-9,; ]+" required>
<label for="poll">Mirror poll seconds</label>
<input id="poll" name="poll" value="$poll_html" inputmode="decimal">
<label><input type="checkbox" name="exclusive" value="1" style="width:auto" $( [ "$exclusive" = "1" ] && echo checked )> Keep single-wavelength modes exclusive</label>
<button type="submit">Save Changes</button>
</form>
<h2>Status</h2>
<pre>
HTML

/usr/bin/c120-light-pinsd status 2>/dev/null | html_escape || true

cat <<HTML
</pre>
<p><a href="/cgi-bin/preview.cgi">Back to preview</a></p>
</body>
</html>
HTML
