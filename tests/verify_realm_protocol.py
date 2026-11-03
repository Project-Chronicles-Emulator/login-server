"""Verify the actual login executable returns the researched binary fingerprint."""

import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

from google.protobuf import message_factory

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from protocol_schema import load_descriptor_pool


def reserve_ports():
    with socket.socket() as public, socket.socket() as private:
        public.bind(("127.0.0.1", 0))
        private.bind(("127.0.0.1", 0))
        return public.getsockname()[1], private.getsockname()[1]


def verify(executable):
    pool = load_descriptor_pool(ROOT / "protocol/descriptors.normalized.pb")
    response_type = message_factory.GetMessageClass(
        pool.FindMessageTypeByName("x5.pa2c.WorldListRes")
    )
    port, authority_port = reserve_ports()
    origin = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory(prefix="chronicles-login-protocol-") as directory:
        key = Path(directory) / "service.key"
        key.write_text(secrets.token_hex(32), encoding="ascii")
        environment = os.environ.copy()
        for name in ["CHRONICLES_DATABASE_URL", "CHRONICLES_BOOTSTRAP_ACCOUNT",
                     "CHRONICLES_BOOTSTRAP_PASSWORD"]:
            environment.pop(name, None)
        command = [
            str(executable.resolve()), "--storage", "memory", "--port", str(port),
            "--authority-port", str(authority_port), "--public-address", origin,
            "--auth-service-key-file", str(key),
        ]
        process = subprocess.Popen(
            command, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        try:
            deadline = time.monotonic() + 15
            while True:
                if process.poll() is not None:
                    raise RuntimeError("Login executable exited before becoming healthy")
                try:
                    with urllib.request.urlopen(origin + "/health", timeout=1) as response:
                        health = json.load(response)
                    if health.get("status") == "ok":
                        break
                except (OSError, urllib.error.URLError):
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError("Login executable did not become healthy")
                time.sleep(0.1)

            with urllib.request.urlopen(origin + "/api/auth/WorldListReq", timeout=3) as response:
                worlds = response_type.FromString(response.read())
            if len(worlds.worlds) != 1:
                raise AssertionError("Expected one researched realm")
            world = worlds.worlds[0]
            expected = bytes.fromhex(
                "b0fa233f08edc242480aa097e76541d4e256bd7641c856c424907eb2ecd91e15"
            )
            if world.version != "hotfix-26.10.18.734" or world.protocol != expected:
                raise AssertionError(
                    f"Incorrect client identity: version={world.version}; "
                    f"protocol length={len(world.protocol)}; expected 32 raw bytes"
                )
            print("PASS: actual login response preserves the supported version and binary fingerprint")
        finally:
            process.terminate()
            process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path,
                        default=ROOT / "out/build/windows/bin/Release/chronicles-login-server.exe")
    args = parser.parse_args()
    verify(args.executable)


if __name__ == "__main__":
    main()
