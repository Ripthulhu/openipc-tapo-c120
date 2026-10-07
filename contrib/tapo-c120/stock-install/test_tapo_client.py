"""Protocol compatibility and factory-mode cleanup, without contacting a camera."""
import base64
import io
import json
from unittest.mock import patch

import tapo_client as client
import tapo_c120_factory_exec as factory


key, iv = bytes(range(16)), bytes(range(16, 32))
plain = '{"method":"get"}'
# Captured from the retired implementation, including PKCS#7's full padding block.
cipher = 'RuzI0EgQ5287P7eabc6/G82DYxJRVa4lynDzmcKnO5w='
assert client.aes_encrypt_base64(plain, key, iv) == cipher
assert client.aes_decrypt_base64(cipher, key, iv) == plain
for data in ('!', base64.b64encode(b'bad').decode(), base64.b64encode(bytes(16)).decode()):
    try:
        client.aes_decrypt_base64(data, key, iv)
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid ciphertext accepted')


class Socket:
    def makefile(self, *args):
        return io.BytesIO(b'garbage\0HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}')


response = client.TapoHTTPResponse(Socket())
response.begin()
assert response.status == 200 and response.read() == b'{}'

password = client.sha256_hex('test-password')
cnonce, snonce = '0011223344556677', '8899AABBCCDDEEFF'
confirm = client.sha256_hex(cnonce + password + snonce) + snonce + cnonce
responses = [
    {'body': json.dumps({'result': {'data': {'nonce': snonce, 'device_confirm': confirm}}}).encode()},
    {'body': b'{"error_code":0,"result":{"stok":"test","start_seq":10}}'},
]
with patch.object(client.secrets, 'token_hex', return_value=cnonce), patch.object(client, 'http_request', side_effect=responses) as request:
    login = client.login_once('192.168.1.2', 'test@example.invalid', password)
    assert request.call_count == 2 and login['seq'] == 10
with patch.object(client, 'http_request', return_value={'body': b'{"error_code":-1}'}) as request:
    try:
        client.login_once('192.168.1.2', 'test@example.invalid', password)
    except RuntimeError:
        pass
    else:
        raise AssertionError('Failed login accepted')
    assert request.call_count == 1
session = client.TapoSession('192.168.1.2', password, login)
with patch.object(client, 'http_request', side_effect=OSError('disconnected')):
    try:
        session.secure_request({'method': 'get'})
    except OSError:
        pass
assert session.seq == 11


class Session:
    host = '192.168.1.2'
    stok = 'test'

    def __init__(self, fail=False):
        self.requests = []
        self.fail = fail

    def secure_request(self, request):
        self.requests.append(request)
        if len(request.get('params', {}).get('requests', [])) == 4:
            if self.fail:
                raise OSError('ambiguous batch response')
            return {'result': {'responses': [{'error_code': n} for n in (0, -40101, 0, 0)]}}
        return {}


session = Session()
with patch.object(factory, 'factory_state', side_effect=['0', '0']), \
        patch.object(factory, 'http_request', return_value={'status': 200, 'body': b'output\n0\n\0'}):
    assert factory.run(session, 'id') == (0, 'output')
assert session.requests[0]['params']['requests'][1]['params']['device_info']['set_info']['type'] == 14
session = Session(fail=True)
with patch.object(factory, 'factory_state', side_effect=['0', '1', '0']):
    try:
        factory.run(session, 'id')
    except OSError:
        pass
    else:
        raise AssertionError('Ambiguous command retried or accepted')
assert len(session.requests) == 2
assert session.requests[-1]['params']['requests'] == [factory.factory_request('0')]
print('PASS: legacy AES vector, malformed ciphertext, stock HTTP quirk, one-shot login, sequence and factory cleanup')
