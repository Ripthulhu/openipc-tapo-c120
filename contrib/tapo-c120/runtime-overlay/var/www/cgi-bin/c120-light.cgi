#!/usr/bin/haserl
<%
echo "HTTP/1.1 200 OK
Content-type: application/json
Cache-Control: no-store
Pragma: no-cache
"

mode=${GET_mode:-status}

case "$mode" in
	off|850|940|both|ir|850940|white|status)
		/usr/bin/c120-lamps "$mode"
		;;
	*)
		/usr/bin/c120-lamps status
		;;
esac
%>
