"""Real MQTT 5 broker plus synthetic camera APIs; no camera or credentials needed."""
import copy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import socket
import subprocess
import tempfile
import threading
import time

BASE = Path(__file__).resolve().parent


def main():
    native = {'motionDetect': {'enabled': True, 'sensitivity': 3},
              'video0': {'size': '2560x1440', 'fps': 30, 'bitrate': 6000}}
    original_video = copy.deepcopy(native['video0'])
    ai_cfg = {'enabled': True, 'confidence': .6, 'motionRegions': True, 'soundEnabled': True,
              'soundSensitivity': 1, 'recording': {'enabled': False, 'seconds': 30, 'categories': ['person', 'pet']},
              'notifications': {'url': 'https://private.example/webhook', 'token': 'keep-private'}}
    posts, notes = [], queue.Queue()
    counter, night, white = 0, 0, 0

    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        for path in ['etc', 'run', 'sys/class/gpio/gpio14']:
            (root / path).mkdir(parents=True)
        # The daemon owns its PID lock; init must neither precreate it nor overlap a failed stop.
        init = root/'mqtt-init'
        init.write_text((BASE/'files/etc/init.d/S98openipc-mqtt').read_text().replace(
            '/run/c120-setup-ap.active', str(root/'run/c120-setup-ap.active')))
        init.chmod(0o755)
        stub = root/'start-stop-daemon'
        stub.write_text('#!/bin/sh\ncase " $* " in *" -t "*) exit "${STILL_RUNNING:-1}" ;; esac\nprintf "%s\\n" "$*" >> "$SSD_LOG"\n')
        stub.chmod(0o755)
        log = root/'service.log'
        env = dict(os.environ, PATH=str(root)+':'+os.environ['PATH'], SSD_LOG=str(log))
        ap = root/'run/c120-setup-ap.active'
        ap.touch()
        subprocess.run(['sh', str(init), 'start'], env=env, check=True)
        assert not log.exists(), 'Recovery AP must not start MQTT'
        ap.unlink()
        subprocess.run(['sh', str(init), 'restart'], env=env, check=True)
        calls = log.read_text().splitlines()
        assert len(calls)==2 and calls[0].startswith('-K ') and calls[1].startswith('-S ')
        assert ' -m ' not in calls[1], 'Only the daemon may create/lock its PID file'
        log.unlink()
        subprocess.run(['sh', str(init), 'restart'], env=dict(env, STILL_RUNNING='0'), check=False)
        assert len(log.read_text().splitlines())==1, 'A failed stop must not launch another daemon'
        def save(path, value):
            target = root / path
            replacement = target.with_suffix('.tmp')
            replacement.write_text(json.dumps(value))
            replacement.replace(target)
        state = {'monotonic': time.monotonic(), 'bootId': 'test-boot', 'lastEventId': 1,
                 'running': True, 'soundRunning': True, 'objects': [], 'sounds': [], 'events': [],
                 'status': 'Detecting', 'recording': {'active': False, 'status': 'Disabled'}}
        save('run/c120-ai-state.json', state)
        save('etc/c120-ai.json', ai_cfg)
        (root / 'sys/class/gpio/gpio14/value').write_text('0')
        done = threading.Event()
        def refresh():
            while not done.wait(.2):
                state['monotonic'] = time.monotonic()
                save('run/c120-ai-state.json', state)
        refresh_thread = threading.Thread(target=refresh, daemon=True)
        refresh_thread.start()

        class Camera(BaseHTTPRequestHandler):
            def reply(self, value):
                data = value.encode() if isinstance(value, str) else json.dumps(value).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            def do_GET(self):
                nonlocal night, white
                if self.path == '/api/v1/config.json': self.reply(native)
                elif self.path == '/metrics/motion': self.reply(f'md_rects_acc_total {counter}\n')
                elif self.path == '/metrics/night': self.reply(f'night_enabled {night}\n')
                elif self.path == '/cgi-bin/c120-ai-api.cgi': self.reply({'csrf': 'a'*64, 'config': ai_cfg})
                elif self.path.startswith('/night/'):
                    night = int(self.path.endswith('/on')); self.reply({})
                elif self.path.startswith('/cgi-bin/c120-floodlight.cgi'):
                    white = int(self.path.endswith('=on'))
                    (root / 'sys/class/gpio/gpio14/value').write_text(str(white)); self.reply({})
                else: self.send_error(404)
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                posts.append((self.path, body))
                if self.path == '/api/v1/config':
                    for group, fields in body.items(): native[group].update(fields)
                else:
                    assert body.pop('csrf') == 'a'*64
                    if 'recordClipSeconds' not in body:
                        ai_cfg.clear(); ai_cfg.update(body); save('etc/c120-ai.json', ai_cfg)
                self.reply('')  # Majestic configuration POST can return an empty body.
            def log_message(self, *args): pass

        server = ThreadingHTTPServer(('127.0.0.1', 0), Camera)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        cfg = dict(enabled=True, host='127.0.0.2', port=1883, id='testcamera', name='Test camera',
                   username='test', password='super-secret', url='https://camera.example')
        save('etc/openipc-mqtt.json', cfg)
        (root / 'etc/openipc-mqtt.json').chmod(0o600)
        executable = root / 'openipc-mqtt'
        subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', '-Os', f'-DMQTT_ROOT="{root}"',
                        f'-DCAMERA_ORIGIN="http://127.0.0.1:{server.server_port}"', str(BASE/'openipc-mqtt.c'),
                        '-lmosquitto', '-ljson-c', '-lcurl', '-lm', '-o', str(executable)], check=True)
        discovery = json.loads(subprocess.check_output([str(executable), '--discovery']))
        assert len(discovery['components']) == 27, len(discovery['components'])
        assert 'super-secret' not in json.dumps(discovery) and 'private.example' not in json.dumps(discovery)
        assert discovery['components']['motion']['value_template'] == "{{ 'ON' if value_json.motion else 'OFF' }}"
        assert discovery['components']['record_clip']['payload_press'] == 'PRESS'
        assert set(discovery)=={'state_topic','device','origin','components'}, 'device discovery cannot use the single-entity ~ shortcut'
        assert discovery['state_topic']=='openipc/testcamera/state'
        assert discovery['components']['record_clip']['command_topic']=='openipc/testcamera/set/record_clip'
        broker_cfg = root / 'broker.conf'
        broker_cfg.write_text('listener 1883 127.0.0.2\nallow_anonymous true\npersistence false\n')
        broker = subprocess.Popen(['mosquitto', '-c', str(broker_cfg)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subscriber = daemon = None
        messages = []
        def wait(predicate, timeout=12):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                try:
                    topic, payload = notes.get(timeout=.1)
                    messages.append((topic, payload))
                    if predicate(topic, payload): return payload
                except queue.Empty: pass
                assert daemon is None or daemon.poll() is None, 'MQTT helper exited'
            raise AssertionError(('MQTT wait timed out', messages[-10:]))
        def pub(key, value, retained=False):
            args = ['mosquitto_pub', '-h', '127.0.0.2', '-V', '5', '-t', 'openipc/testcamera/'+key, '-m', value]
            subprocess.run(args + (['-r'] if retained else []), check=True)
        def state_is(key, value):
            return lambda t, p: t.endswith('/state') and json.loads(p).get(key) == value
        def result(key, accepted):
            return lambda t, p: t.endswith('/result') and json.loads(p) == {'command':key, 'accepted':accepted} if accepted else t.endswith('/result') and json.loads(p).get('command') == key and not json.loads(p)['accepted']
        try:
            for _ in range(50):
                try:
                    with socket.create_connection(('127.0.0.2',1883), timeout=.1): break
                except OSError: time.sleep(.1)
            assert broker.poll() is None, 'Test broker port is in use'
            subscriber = subprocess.Popen(['mosquitto_sub', '-h', '127.0.0.2', '-V', '5', '-t', 'openipc/testcamera/#',
                                           '-t', 'homeassistant/device/openipc_testcamera/config', '-F', '%t %p'], stdout=subprocess.PIPE, text=True, bufsize=1)
            def collect():
                for line in subscriber.stdout:
                    topic, payload = line.rstrip('\n').split(' ', 1); notes.put((topic, payload))
            threading.Thread(target=collect, daemon=True).start()
            time.sleep(.2)
            pub('set/motion_enabled', 'OFF', True)
            daemon = subprocess.Popen([str(executable)], stderr=subprocess.PIPE)
            wait(lambda t,p: t.endswith('/config'))
            wait(state_is('motion_enabled', True))
            assert not posts, 'Stored retained command was replayed'
            counter = 4
            wait(state_is('motion', True)); wait(state_is('motion', False))
            state['objects'] = [{'label':'person', 'confidence':.9}]
            state['lastEventId'] = 2
            state['events'] = [{'label':'person', 'id':2, 'time':int(time.time()), 'bootId':'test-boot', 'confidence':.9}]
            wait(lambda t,p: t.endswith('/event/detection') and json.loads(p)['event_type']=='person')
            wait(state_is('person', True))
            pub('set/ai_enabled', 'OFF', True); wait(result('ai_enabled', False))
            assert ai_cfg['enabled']
            for key, payload in [('motion_sensitivity','9'), ('ai_confidence','"65"'), ('record_seconds','0'), ('shell','reboot'), ('ai_enabled','true')]:
                pub('set/'+key, payload); wait(result(key, False))
            assert not posts
            for key, payload in [('ai_confidence','65'), ('motion_sensitivity','5'), ('ai_enabled','OFF'), ('record_seconds','4')]:
                pub('set/'+key, payload); wait(result(key, True))
            wait(state_is('record_seconds', 4))
            assert native['video0'] == original_video
            assert ai_cfg['notifications']['token'] == 'keep-private'
            assert ai_cfg['recording']['categories'] == ['person','pet'] and not ai_cfg['recording']['enabled']
            pub('set/record_categories','Birds'); wait(result('record_categories', True))
            wait(state_is('record_categories','Birds'))
            assert ai_cfg['recording']['categories']==['bird']
            pub('set/record_categories','People and pets'); wait(result('record_categories', True))
            assert ai_cfg['recording']['categories']==['person','pet']
            pub('set/record_clip','PRESS'); wait(result('record_clip', True))
            assert posts[-1][1] == {'recordClipSeconds':4}
            pub('set/floodlight','ON'); wait(result('floodlight', True)); wait(state_is('floodlight', True))
            pub('set/floodlight','OFF'); wait(result('floodlight', True)); wait(state_is('floodlight', False))
            save('run/c120-recording-latest.json', {'id':'clip-1', 'source':'manual', 'videoUrl':'/recording.mp4'})
            wait(lambda t,p: t.endswith('/event/recording') and json.loads(p)['id']=='clip-1')
            (root/'run/c120-setup-ap.active').touch()
            wait(lambda t,p: t.endswith('/ai_available') and p=='offline')
            pub('set/record_clip','PRESS'); wait(result('record_clip', False))
            (root/'run/c120-setup-ap.active').unlink()
            wait(lambda t,p: t.endswith('/ai_available') and p=='online')
            daemon.kill(); daemon.wait(); daemon=None
            wait(lambda t,p: t.endswith('/availability') and p=='offline')
            before_events = sum(t.endswith('/event/detection') for t,p in messages)
            daemon = subprocess.Popen([str(executable)], stderr=subprocess.PIPE)
            wait(lambda t,p: t.endswith('/availability') and p=='online')
            wait(state_is('person', True))
            assert sum(t.endswith('/event/detection') for t,p in messages)==before_events
            assert all('keep-private' not in p for t,p in messages)
            daemon.terminate(); daemon.wait(timeout=5); daemon=None
            wait(lambda t,p: t.endswith('/availability') and p=='offline')
        finally:
            done.set(); refresh_thread.join(timeout=2)
            for p in (daemon, subscriber, broker):
                if p is not None and p.poll() is None:
                    p.terminate(); p.wait(timeout=5)
            server.shutdown(); server.server_close()
    print('PASS: MQTT 5 discovery, motion/AI events, settings, manual clip, lights, AP guard, LWT, reconnect baseline, retained-command rejection, no leaked secrets or video changes')


if __name__ == '__main__': main()
