#!/bin/sh
set -eu
[ "$(id -u)" = 0 ] || exit 1
[ -f /var/www/cgi-bin/p/header.cgi ] && [ -r /var/www/cgi-bin/p/majestic.sh ] || {
    echo "The OpenIPC WebUI recording helper is missing" >&2; exit 1;
}
. /var/www/cgi-bin/p/majestic.sh
hook=$(mj_cfg records.onClose) || {
    rc=$?
    [ "$rc" -eq 1 ] || { echo "Cannot read recording close hook" >&2; exit 1; }
    hook=
}
[ "$hook" != /usr/bin/c120-recording-closed ] || [ -e /etc/c120-recording-hook.previous ] || {
    echo "Cannot restore the previous recording close hook" >&2; exit 1;
}
[ ! -x /etc/init.d/S97c120-ai ] || /etc/init.d/S97c120-ai stop
! pidof c120-aid >/dev/null || { echo "AI is still running" >&2; exit 1; }
if [ -f /usr/bin/c120-setup-ap ]; then
    sed -i '/stop_service_if_running S97c120-ai c120-aid \/run\/c120-ai.pid/d' /usr/bin/c120-setup-ap
fi
sed -i '/href="c120-ai.cgi"/d' /var/www/cgi-bin/p/header.cgi
if [ "$hook" = /usr/bin/c120-recording-closed ]; then
    if [ -s /etc/c120-recording-hook.previous ]; then
        previous=$(cat /etc/c120-recording-hook.previous)
        mj_set records.onClose "$previous" string || exit 1
    else
        mj_clear records.onClose || exit 1
    fi
fi
rm -f /usr/bin/c120-recording-closed /var/www/cgi-bin/c120-recordings-api.cgi /etc/c120-recording-hook.previous
rm -f /etc/init.d/S97c120-ai /usr/bin/c120-ai /var/www/cgi-bin/c120-ai.cgi /var/www/cgi-bin/c120-ai-api.cgi /var/www/a/c120-ai.js
rm -rf /usr/lib/c120-ai
rm -f /run/c120-ai.pid /run/c120-ai-state.json /run/c120-ai.token
echo "AI plugin removed. Settings preserved in /etc/c120-ai.json."
