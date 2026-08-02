#!/usr/bin/env python3

import argparse
import socket
import subprocess
import time
import uuid


class Client:
    def __init__(self, port: int):
        self.sock = socket.create_connection(
            ("127.0.0.1", port),
            timeout=3.0,
        )
        self.sock.settimeout(0.25)
        self.read_for(0.2)

    def read_for(self, seconds: float) -> str:
        deadline = time.time() + seconds
        chunks = []

        while time.time() < deadline:
            try:
                data = self.sock.recv(4096)
                if not data:
                    break
                chunks.append(data.decode())
            except socket.timeout:
                pass

        return "".join(chunks)

    def command(self, value: str) -> str:
        self.sock.sendall((value + "\n").encode())
        return self.read_for(0.5)

    def close(self) -> None:
        self.sock.close()


def start_server(
    executable: str,
    port: int,
    config: str,
) -> subprocess.Popen:
    process = subprocess.Popen(
        [executable, str(port), config],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    time.sleep(0.8)

    if process.poll() is not None:
        output = process.stdout.read()
        raise RuntimeError(
            "server failed to start:\n" + output
        )

    return process


def stop_server(process: subprocess.Popen) -> None:
    process.terminate()

    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--port", type=int, default=19322)
    args = parser.parse_args()

    suffix = uuid.uuid4().hex[:10]
    username = "user_" + suffix
    password = "Pass_" + suffix

    first_server = start_server(
        args.server,
        args.port,
        args.config,
    )

    try:
        client = Client(args.port)
        response = client.command(
            f"REGISTER {username} {password}"
        )
        client.close()

        assert (
            "registration successful" in response
        ), response
    finally:
        stop_server(first_server)

    second_server = start_server(
        args.server,
        args.port,
        args.config,
    )

    try:
        client = Client(args.port)

        response = client.command(
            f"LOGIN {username} {password}"
        )
        assert "login successful" in response, response

        response = client.command(
            f"LOGIN {username} wrong-password"
        )
        assert (
            "already logged in" in response
        ), response

        client.command("LOGOUT")

        response = client.command(
            f"LOGIN {username} wrong-password"
        )
        assert (
            "invalid username or password" in response
        ), response

        client.close()
    finally:
        stop_server(second_server)

    print("MySQL account persistence: PASS")
    print("correct password after restart: PASS")
    print("wrong password rejected: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
