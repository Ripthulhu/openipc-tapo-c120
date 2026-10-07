#!/usr/bin/haserl
<%
fail() {
	printf 'HTTP/1.1 %s\nContent-Type: application/json\nCache-Control: no-store\n\n' "$1"
	printf '{"error":"%s"}\n' "$2"
	exit 0
}

[ "${REQUEST_METHOD:-GET}" = GET ] || fail '405 Method Not Allowed' 'Use GET.'

hex_bytes() {
	value=${1#0x}
	printf '%s\n' "$value" | grep -Eq '^[0-9A-Fa-f]{1,8}$' || return 1
	printf '%d' "0x$value"
}

cmdline=$(cat /proc/cmdline 2>/dev/null) || fail '503 Service Unavailable' 'Memory map unavailable.'
physical_hex=
media_hex=
for arg in $cmdline; do
	case "$arg" in
		LX_MEM=*) physical_hex=${arg#LX_MEM=} ;;
		mma_heap=mma_heap_name0,*)
			old_ifs=$IFS
			IFS=,
			set -- ${arg#mma_heap=}
			IFS=$old_ifs
			for field do
				case "$field" in sz=*) media_hex=${field#sz=} ;; esac
			done
			;;
	esac
done
physical=$(hex_bytes "$physical_hex") || fail '503 Service Unavailable' 'Memory map unavailable.'
media=$(hex_bytes "$media_hex") || fail '503 Service Unavailable' 'Memory map unavailable.'
[ "$physical" -gt 0 ] && [ "$physical" -le 268435456 ] &&
	[ "$media" -gt 0 ] && [ "$media" -le "$physical" ] ||
	fail '503 Service Unavailable' 'Memory map unavailable.'

free=
heap_file=/proc/mi_modules/mi_sys_mma/mma_heap_name0
if [ -r "$heap_file" ]; then
	heap_line=$(grep '^[[:space:]]*mma_heap_name0[[:space:]]' "$heap_file" 2>/dev/null | head -n 1)
	if [ -n "$heap_line" ]; then
		set -- $heap_line
		if [ "$#" -ge 4 ]; then
			heap_size=$(hex_bytes "$3") || heap_size=
			heap_free=$(hex_bytes "$4") || heap_free=
			if [ "$heap_size" = "$media" ] && [ -n "$heap_free" ] && [ "$heap_free" -le "$media" ]; then
				free=$heap_free
			fi
		fi
	fi
fi

printf 'HTTP/1.1 200 OK\nContent-Type: application/json\nCache-Control: no-store\n\n'
printf '{"physicalBytes":%s,"mediaBytes":%s' "$physical" "$media"
[ -z "$free" ] || printf ',"mediaFreeBytes":%s' "$free"
printf '}\n'
%>
