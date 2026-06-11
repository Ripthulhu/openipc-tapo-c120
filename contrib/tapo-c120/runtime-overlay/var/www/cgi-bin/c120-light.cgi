#!/bin/sh
echo "HTTP/1.1 200 OK
Content-type: application/json
Cache-Control: no-store
Pragma: no-cache
"

mode=status
old_ifs="$IFS"
IFS='&'
set -- ${QUERY_STRING:-}
IFS="$old_ifs"
for part do
	case "$part" in
		mode=*)
			mode="${part#mode=}"
			break
			;;
	esac
done

case "$mode" in
	off|850|940|both|ir|850940|white|status)
		/usr/bin/c120-lamps "$mode"
		;;
	*)
		/usr/bin/c120-lamps status
		;;
esac
