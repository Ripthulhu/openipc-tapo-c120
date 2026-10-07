#!/usr/bin/env python3
"""Only the authenticated stock protocol used by the C120 installer."""
import base64
import hashlib
import http.client
import json
import secrets
import ssl
from urllib.parse import quote

from cryptography.hazmat.primitives import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


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


def sha256_hex(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest().upper()


def aes_encrypt_base64(text, key, iv):
    padder = padding.PKCS7(128).padder()
    data = padder.update(text.encode("utf-8")) + padder.finalize()
    encryptor = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
    return base64.b64encode(encryptor.update(data) + encryptor.finalize()).decode("ascii")


def aes_decrypt_base64(text, key, iv):
    decryptor = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
    data = decryptor.update(base64.b64decode(text, validate=True)) + decryptor.finalize()
    unpadder = padding.PKCS7(128).unpadder()
    return (unpadder.update(data) + unpadder.finalize()).decode("utf-8")


def http_request(host: str, path: str, method: str = "GET", body: bytes | None = None, headers: dict | None = None):
    context = ssl._create_unverified_context()
    conn = http.client.HTTPSConnection(host, timeout=15, context=context)
    conn.response_class = TapoHTTPResponse
    request_headers = dict(headers or {})
    if body is not None:
        request_headers.setdefault("Content-Length", str(len(body)))
    try:
        conn.request(method, path, body=body, headers=request_headers)
        response = conn.getresponse()
        return {"status": response.status, "body": response.read()}
    finally:
        conn.close()


def login_once(host: str, username: str, pwd_hash: str):
    cnonce = secrets.token_hex(8).upper()
    login1_body = json.dumps({"method": "login", "params": {"cnonce": cnonce, "encrypt_type": "3", "username": username}}).encode()
    login1 = json.loads(http_request(host, "/", "POST", login1_body, {"Content-Type": "application/json"})["body"].decode())
    server_nonce = login1.get("result", {}).get("data", {}).get("nonce")
    if not server_nonce:
        raise RuntimeError(f"Handshake failed before nonce: {login1}")

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
        raise RuntimeError(f"Login failed: {login2}")

    hashed_key = sha256_hex(cnonce + pwd_hash + server_nonce)
    key = bytes.fromhex(sha256_hex("lsk" + cnonce + server_nonce + hashed_key)[:32])
    iv = bytes.fromhex(sha256_hex("ivb" + cnonce + server_nonce + hashed_key)[:32])
    return {"stok": login2["result"]["stok"], "seq": int(login2["result"]["start_seq"]), "key": key, "iv": iv, "cnonce": cnonce}


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


def encode_traversal_path(path_name: str) -> str:
    return "%2f".join(quote(part, safe="") for part in path_name.split("/"))
