#!/usr/bin/haserl
<%
fail() {
	printf 'HTTP/1.1 %s\nContent-Type: application/json\nCache-Control: no-store\n\n' "$1"
	printf '{"error":"%s"}\n' "$2"
	exit 0
}

case "${REQUEST_METHOD:-GET}" in
	GET) action=${GET_action:-toggle} ;;
	POST) action=${POST_action:-toggle} ;;
	*) fail '405 Method Not Allowed' 'Use GET or POST.' ;;
esac
case "$action" in
	toggle|on|off|status) ;;
	*) fail '400 Bad Request' 'Use toggle, on, off or status.' ;;
esac
# Direct authenticated automation requests have no Referer; reject browser cross-origin writes.
if [ "$action" != status ]; then
	case "${HTTP_REFERER:-}" in
		''|"http://${HTTP_HOST:-}/"*|"https://${HTTP_HOST:-}/"*) ;;
		*) fail '403 Forbidden' 'The request must come from this camera.' ;;
	esac
	case "${HTTP_ORIGIN:-}" in
		''|"http://${HTTP_HOST:-}"|"https://${HTTP_HOST:-}") ;;
		*) fail '403 Forbidden' 'The request must come from this camera.' ;;
	esac
fi
result=$(/usr/bin/c120-eventd floodlight "$action") ||
	fail '503 Service Unavailable' 'Floodlight control is busy or unavailable. Try again.'
printf 'HTTP/1.1 200 OK\nContent-Type: application/json\nCache-Control: no-store\n\n%s\n' "$result"
%>
