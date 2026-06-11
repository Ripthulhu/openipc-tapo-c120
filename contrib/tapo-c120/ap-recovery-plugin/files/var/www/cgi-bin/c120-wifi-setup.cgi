#!/bin/sh

decode() {
	printf '%b' "$(printf '%s' "$1" | sed 's/+/ /g; s/%/\\x/g')"
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
				printf '%s' "${part#"$name"}"
				return 0
				;;
		esac
	done
}

apply="$(param apply)"
ssid=
psk=

if [ "$apply" = "1" ]; then
	ssid="$(decode "$(param ssid)")"
	psk="$(decode "$(param psk)")"
fi

echo "HTTP/1.1 200 OK
Content-type: text/html
Cache-Control: no-store
Pragma: no-cache
"

cat <<'HTML'
<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>C120 Wi-Fi Setup</title>
<style>
body{font-family:sans-serif;margin:24px;max-width:520px}
label{display:block;margin:14px 0 6px}
input,button{font:inherit;width:100%;box-sizing:border-box;padding:10px}
button{margin-top:18px}
.ok{padding:12px;background:#e7ffe7;border:1px solid #8fd38f}
</style>
</head>
<body>
<h1>C120 Wi-Fi Setup</h1>
HTML

if [ "$apply" = "1" ] && [ -n "$ssid" ]; then
	fw_setenv wlanssid "$ssid"
	[ -n "$psk" ] && fw_setenv wlanpass "$psk"
	cat <<HTML
<p class="ok">Saved Wi-Fi settings for <strong>$(printf '%s' "$ssid" | sed 's/[<>&]/_/g')</strong>. The camera will leave setup AP mode and try to join that network.</p>
HTML
	(nohup sh -c 'sleep 3; /usr/bin/c120-setup-ap stop' >/tmp/c120-wifi-setup-stop.log 2>&1 &)
else
	cat <<'HTML'
<form method="get" action="/cgi-bin/c120-wifi-setup.cgi">
<input type="hidden" name="apply" value="1">
<label for="ssid">Wi-Fi SSID</label>
<input id="ssid" name="ssid" required>
<label for="psk">Wi-Fi password</label>
<input id="psk" name="psk" type="password" minlength="8">
<button type="submit">Save And Reconnect</button>
</form>
HTML
fi

cat <<'HTML'
<p>Setup AP address: <code>192.168.4.1</code></p>
<p>SSH is also available at <code>root@192.168.4.1</code>.</p>
</body>
</html>
HTML
