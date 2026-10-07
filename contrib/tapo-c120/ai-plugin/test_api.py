"""On-device integration check. Requires C120_SSH_PASSWORD, no saved credentials.

python test_api.py https://camera.example --exercise
--exercise briefly disables AI, then restores the original settings in finally.
"""
import argparse
import json
import os
import time
from urllib.error import HTTPError
from urllib.request import (Request, build_opener, HTTPBasicAuthHandler,
                            HTTPDigestAuthHandler, HTTPPasswordMgrWithDefaultRealm,
                            ProxyHandler)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('url')
    parser.add_argument('--exercise', action='store_true')
    parser.add_argument('--soak-seconds', type=int, default=0)
    args = parser.parse_args()
    assert 0 <= args.soak_seconds <= 3600
    base = args.url.rstrip('/')
    url = base + '/cgi-bin/c120-ai-api.cgi'
    passwords = HTTPPasswordMgrWithDefaultRealm()
    passwords.add_password(None, base, 'root', os.environ['C120_SSH_PASSWORD'])
    client = build_opener(ProxyHandler({}), HTTPDigestAuthHandler(passwords), HTTPBasicAuthHandler(passwords))

    def request(path=url, payload=None, status=200, authenticated=True):
        req = Request(path, data=None if payload is None else json.dumps(payload).encode(),
                      headers={'Content-Type': 'application/json'} if payload is not None else {})
        opener = client if authenticated else build_opener(ProxyHandler({}))
        try:
            response = opener.open(req, timeout=10)
        except HTTPError as error:
            response = error
        with response:
            assert response.status == status, (path, response.status, status)
            data = response.read()
            return json.loads(data) if response.status != 401 else None

    request(authenticated=False, status=401)
    initial = request()
    assert initial.get('soundSource') == 'opus', 'Install the Opus-input plugin before enabling sound'
    cfg, token = initial['config'], initial['csrf']
    assert 'notifications' in cfg and 'recording' in cfg
    assert initial['schemaVersion'] == 2 and 'model' in cfg and 'nms' in cfg
    assert any(item['id'] == 'stock' for item in initial['models']['items'])
    majestic = request(base + '/api/v1/config.json')

    def diagnostic(value):
        # Action settings contain private destinations/tokens; do not print them.
        keys = ('status', 'running', 'frames', 'soundStatus', 'soundRunning',
                'soundFrames', 'soundReconnects', 'availableKiB', 'rssKiB')
        return {key: value.get(key) for key in keys}
    for changes, status in [({'csrf': 'invalid'}, 403), ({'intervalMs': 499}, 400),
                            ({'enabled': 1}, 400), ({'confidence': 1.01}, 400),
                            ({'soundEnabled': 1}, 400), ({'soundSensitivity': 3}, 400),
                            ({'soundGainDb': -1}, 400), ({'soundGainDb': 25}, 400),
                            ({'soundClasses': ['bird']}, 400), ({'soundClasses': ['bark','bark']}, 400),
                            ({'soundEnabled': True, 'soundClasses': []}, 400),
                            ({'extra': True}, 400), ({'csrf': token+'\0suffix'}, 403),
                            ({'model': '../stock'}, 400), ({'model': 'stock\0suffix'}, 400),
                            ({'model': 'unavailable-api-test'}, 400),
                            ({'nms': 0}, 400), ({'nms': 1}, 400)]:
        request(payload={**cfg, 'csrf': token, **changes}, status=status)
        assert request()['config'] == cfg

    request(payload={'csrf': 'invalid', 'modelAction': 'remove', 'id': 'stock'}, status=403)
    request(payload={'csrf': token, 'modelAction': 'remove', 'id': 'stock'}, status=400)
    request(payload={'csrf': token, 'modelAction': 'unknown'}, status=400)
    assert request()['config'] == cfg

    def save(value):
        current = request()
        if current['config'] == value:
            return current
        return request(payload={**value, 'csrf': current['csrf']})

    for changes in ({'cooldownSeconds': 4294967356}, {'url': 'file:///etc/passwd'},
                    {'url': 'http://user:password@localhost/'}, {'categories': ['pet','pet']},
                    {'token': 'bad\r\nheader'}, {'enabled': True, 'url': ''}):
        request(payload={**cfg, 'csrf': token, 'notifications': {**cfg['notifications'], **changes}}, status=400)
        assert request()['config'] == cfg
    for changes in ({'seconds': 0}, {'seconds': 601}, {'categories': ['cat']}, {'enabled': True, 'categories': []}):
        request(payload={**cfg, 'csrf': token, 'recording': {**cfg['recording'], **changes}}, status=400)
        assert request()['config'] == cfg

    def wait_for(predicate):
        for _ in range(30):
            value = request()
            if predicate(value):
                return value
            time.sleep(.5)
        raise AssertionError(diagnostic(value))

    if args.exercise:
        try:
            save({**cfg, 'enabled': False, 'soundEnabled': False})
            wait_for(lambda s: s['status'] == 'Disabled' and not s['running'] and not s['objects'] and not s['soundRunning'])
            save({**cfg, 'enabled': False, 'soundEnabled': True, 'soundClasses': ['bark','meow','cry','glass']})
            active = wait_for(lambda s: not s['running'] and s['soundRunning'] and s['soundStatus']=='Listening')
            wait_for(lambda s: s['soundFrames'] > active['soundFrames'])
            save({**cfg, 'enabled': True, 'soundEnabled': False})
            active = wait_for(lambda s: s['running'] and s['status'] == 'Detecting')
            wait_for(lambda s: s['frames'] > active['frames'])
            save({**cfg, 'enabled': True, 'soundEnabled': True, 'soundClasses': ['bark','meow','cry','glass']})
            active = wait_for(lambda s: s['running'] and s['soundRunning'] and s['soundStatus']=='Listening')
            wait_for(lambda s: s['frames'] > active['frames'] and s['soundFrames'] > active['soundFrames'])
        finally:
            save(cfg)
        wait_for(lambda s: s['running'] == cfg['enabled'] and s['soundRunning'] == cfg['soundEnabled'])
    else:
        save(cfg)
    assert request()['config'] == cfg
    assert request(base + '/api/v1/config.json') == majestic
    if args.soak_seconds:
        # A POST returns before the daemon reloads. Require fresh inference before
        # taking the stability baseline, not the previous connection's status.
        settled = request()
        initial = wait_for(lambda s: s['running'] == cfg['enabled'] and
                           (not cfg['enabled'] or s['frames'] > settled['frames'] + 2) and
                           (not cfg['soundEnabled'] or (s['soundStatus'] == 'Listening' and
                            s['soundFrames'] > settled['soundFrames'] + 8)))
        deadline = time.monotonic() + args.soak_seconds
        while time.monotonic() < deadline:
            current = request()
            assert current['config'] == cfg
            assert current['running'] == cfg['enabled']
            assert current['soundRunning'] == cfg['soundEnabled'], diagnostic(current)
            assert current['soundReconnects'] == initial['soundReconnects'], diagnostic(current)
            time.sleep(.5)
        if cfg['enabled']:
            assert current['frames'] > initial['frames']
        if cfg['soundEnabled']:
            assert current['soundFrames'] > initial['soundFrames']
        assert request(base + '/api/v1/config.json') == majestic
        print('PASS: uninterrupted inference for', args.soak_seconds, 'seconds')
    print('PASS: authentication, CSRF, validation, persistence, stream settings' +
          (', disable/re-enable and advancing inference' if args.exercise else ''))


if __name__ == '__main__':
    main()
