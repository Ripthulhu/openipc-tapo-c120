#!/usr/bin/haserl
<%
CONF=/etc/c120-motion-light.conf

reply() {
	printf 'HTTP/1.1 %s\nContent-Type: application/json\nCache-Control: no-store\n\n' "$1"
}

fail() {
	reply "$1"
	printf '{"error":"%s"}\n' "$2"
	exit 0
}

case "${REQUEST_METHOD:-GET}" in
	GET) ;;
	POST)
		# Majestic forwards Referer/Host to CGI, but not arbitrary headers or Origin.
		case "${HTTP_REFERER:-}" in
			"http://${HTTP_HOST:-}/"*|"https://${HTTP_HOST:-}/"*) ;;
			*) fail '403 Forbidden' 'Open these settings from this camera and try again.' ;;
		esac
		case "${HTTP_ORIGIN:-}" in
			''|"http://${HTTP_HOST:-}"|"https://${HTTP_HOST:-}") ;;
			*) fail '403 Forbidden' 'The request must come from this camera.' ;;
		esac
		case "${POST_enabled:-}" in
			0|1) ;;
			*) fail '400 Bad Request' 'Invalid enabled setting.' ;;
		esac
		case "${POST_seconds:-}" in
			''|*[!0-9]*) fail '400 Bad Request' 'Duration must be a whole number from 1 to 600 seconds.' ;;
		esac
		[ "$POST_seconds" -ge 1 ] 2>/dev/null && [ "$POST_seconds" -le 600 ] 2>/dev/null ||
			fail '400 Bad Request' 'Duration must be a whole number from 1 to 600 seconds.'
		# Older callers omit the new field; retain the three-second default.
		POST_triggerSeconds=${POST_triggerSeconds-3}
		case "$POST_triggerSeconds" in
			''|*[!0-9]*) fail '400 Bad Request' 'Motion delay must be a whole number from 0 to 600 seconds.' ;;
		esac
		[ "$POST_triggerSeconds" -ge 0 ] 2>/dev/null && [ "$POST_triggerSeconds" -le 600 ] 2>/dev/null ||
			fail '400 Bad Request' 'Motion delay must be a whole number from 0 to 600 seconds.'
		# Preserve source selection when an older caller only updates the timer.
		source=$(sed -n 's/^C120_MOTION_LIGHT_SOURCE="\([a-z]*\)"$/\1/p' "$CONF" 2>/dev/null)
		mask=$(sed -n 's/^C120_MOTION_LIGHT_AI_MASK="\([0-9]\|1[0-5]\)"$/\1/p' "$CONF" 2>/dev/null)
		POST_source=${POST_source-${source:-motion}}
		POST_aiMask=${POST_aiMask-${mask:-1}}
		case "$POST_source" in motion|ai) ;; *) fail '400 Bad Request' 'Invalid floodlight trigger.' ;; esac
		case "$POST_aiMask" in [0-9]|1[0-5]) ;; *) fail '400 Bad Request' 'Invalid AI categories.' ;; esac
		[ "$POST_enabled:$POST_source:$POST_aiMask" != '1:ai:0' ] ||
			fail '400 Bad Request' 'Select at least one AI category.'
		/usr/bin/c120-eventd status | grep -q '^eventd=running$' ||
			fail '503 Service Unavailable' 'Camera helper is stopped. Restart the camera and try again.'
		umask 077
		tmp="$CONF.$$"
		trap 'rm -f "$tmp"' EXIT HUP INT TERM
		printf 'C120_MOTION_LIGHT_ENABLED="%s"\nC120_MOTION_LIGHT_SECONDS="%s"\nC120_MOTION_LIGHT_TRIGGER_SECONDS="%s"\nC120_MOTION_LIGHT_SOURCE="%s"\nC120_MOTION_LIGHT_AI_MASK="%s"\n' \
			"$POST_enabled" "$POST_seconds" "$POST_triggerSeconds" "$POST_source" "$POST_aiMask" > "$tmp" || fail '500 Internal Server Error' 'Could not save settings.'
		mv "$tmp" "$CONF" || fail '500 Internal Server Error' 'Could not save settings.'
		/usr/bin/c120-eventd reload || fail '503 Service Unavailable' 'Settings saved, but the camera helper did not reload.'
		;;
	*) fail '405 Method Not Allowed' 'Use GET or POST.' ;;
esac
reply '200 OK'
/usr/bin/c120-eventd motion-status
%>
