#!/usr/bin/env python3
"""Send debug-only test-control commands to a Wii U Ship of Harkinian build."""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
import uuid
from pathlib import Path
from typing import Any


DEFAULT_PORT = 43385
MAX_PACKET_SIZE = 64 * 1024
PROTOCOL_SCHEMA = 1


class ControlError(RuntimeError):
    pass


class TestControlClient:
    def __init__(self, host: str, port: int, timeout: float) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self.socket: socket.socket | None = None
        self.received = bytearray()

    def __enter__(self) -> "TestControlClient":
        try:
            self.socket = socket.create_connection((self.host, self.port), timeout=self.timeout)
        except OSError as error:
            raise ControlError(f"could not connect to {self.host}:{self.port}: {error}") from error
        try:
            hello = self.receive_message()
            if hello.get("type") != "hello" or hello.get("service") != "soh-wiiu-test-control":
                raise ControlError(f"unexpected greeting from {self.host}:{self.port}")
            if hello.get("schema") != PROTOCOL_SCHEMA:
                raise ControlError(f"unsupported test-control schema: {hello.get('schema')!r}")
        except Exception:
            self.socket.close()
            self.socket = None
            raise
        return self

    def __exit__(self, *_: object) -> None:
        if self.socket is not None:
            self.socket.close()
            self.socket = None

    def receive_message(self) -> dict[str, Any]:
        if self.socket is None:
            raise ControlError("not connected")

        deadline = time.monotonic() + self.timeout
        while b"\0" not in self.received:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ControlError("timed out waiting for the Wii U")
            self.socket.settimeout(remaining)
            try:
                chunk = self.socket.recv(64 * 1024)
            except socket.timeout as error:
                raise ControlError("timed out waiting for the Wii U") from error
            except OSError as error:
                raise ControlError(f"connection failed while receiving: {error}") from error
            if not chunk:
                raise ControlError("the Wii U closed the connection")
            self.received.extend(chunk)
            if len(self.received) > MAX_PACKET_SIZE:
                raise ControlError("the Wii U response exceeded the packet limit")

        encoded, _, remainder = self.received.partition(b"\0")
        self.received = bytearray(remainder)
        try:
            message = json.loads(encoded.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ControlError(f"the Wii U returned invalid JSON: {error}") from error
        if not isinstance(message, dict):
            raise ControlError("the Wii U returned a non-object JSON message")
        return message

    def request(self, operation: str, **arguments: Any) -> dict[str, Any]:
        if self.socket is None:
            raise ControlError("not connected")

        request_id = uuid.uuid4().hex
        request = {
            "schema": PROTOCOL_SCHEMA,
            "id": request_id,
            "operation": operation,
            **arguments,
        }
        try:
            encoded = json.dumps(request, separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\0"
        except (TypeError, ValueError) as error:
            raise ControlError(f"request is not valid JSON: {error}") from error
        if len(encoded) > MAX_PACKET_SIZE:
            raise ControlError("request exceeded the packet limit")
        try:
            self.socket.sendall(encoded)
        except OSError as error:
            raise ControlError(f"connection failed while sending: {error}") from error

        while True:
            response = self.receive_message()
            if response.get("id") != request_id:
                continue
            if response.get("status") == "failure":
                detail = response.get("error") or response.get("result") or "unknown error"
                raise ControlError(f"{operation} failed: {detail}")
            if response.get("status") == "transitioning":
                continue
            if response.get("type") == "event" and response.get("event") != "state_applied":
                continue
            return response


def load_state(path: Path) -> dict[str, Any]:
    try:
        loaded = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise ControlError(f"could not read {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise ControlError(f"{path} is not valid JSON: {error}") from error

    if isinstance(loaded, dict) and isinstance(loaded.get("state"), dict):
        loaded = loaded["state"]
    if not isinstance(loaded, dict) or not ({"save", "runtime"} & loaded.keys()):
        raise ControlError(f"{path} must contain a captured state or a get_state response")
    return loaded


def write_json(payload: Any, output: Path | None) -> None:
    encoded = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if output is None:
        sys.stdout.write(encoded)
        return
    try:
        output.write_text(encoded, encoding="utf-8")
    except OSError as error:
        raise ControlError(f"could not write {output}: {error}") from error
    print(f"Wrote {output}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="Wii U IPv4 address")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"TCP port (default: {DEFAULT_PORT})")
    parser.add_argument("--timeout", type=float, default=15.0, help="response timeout in seconds (default: 15)")

    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("ping", help="check the control service and report its build")

    get_parser = commands.add_parser("get", help="capture the current test state as JSON")
    get_parser.add_argument("--output", "-o", type=Path, help="write the state to this file")

    apply_parser = commands.add_parser("apply", help="apply a captured or hand-edited state")
    apply_parser.add_argument("state_file", type=Path)

    for name in ("checkpoint", "restore"):
        slot_parser = commands.add_parser(name, help=f"{name} an in-memory state slot")
        slot_parser.add_argument("slot", type=int, choices=range(3), metavar="{0,1,2}")

    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    return args


def main() -> int:
    args = parse_args()
    try:
        with TestControlClient(args.host, args.port, args.timeout) as client:
            if args.command == "ping":
                write_json(client.request("ping"), None)
            elif args.command == "get":
                response = client.request("get_state")
                write_json(response["state"], args.output)
            elif args.command == "apply":
                response = client.request("apply_state", state=load_state(args.state_file))
                write_json(response, None)
            elif args.command in ("checkpoint", "restore"):
                write_json(client.request(args.command, slot=args.slot), None)
            else:
                raise ControlError(f"unsupported command: {args.command}")
    except ControlError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
