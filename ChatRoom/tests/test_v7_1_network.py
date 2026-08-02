#!/usr/bin/env python3

import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path


class Client:
    def __init__(self, port: int):
        self.sock = socket.create_connection(
            ("127.0.0.1", port),
            timeout=2.0,
        )
        self.sock.settimeout(0.2)
        self.buffer = ""
        self.read_for(0.15)

    def send(self, command: str) -> None:
        self.sock.sendall((command + "\n").encode())

    def read_for(self, seconds: float = 0.25) -> str:
        deadline = time.time() + seconds
        chunks = []

        while time.time() < deadline:
            try:
                data = self.sock.recv(4096)
                if not data:
                    break
                chunks.append(data.decode())
            except socket.timeout:
                continue

        text = "".join(chunks)
        self.buffer += text
        return text

    def command(
        self,
        command: str,
        seconds: float = 0.25,
    ) -> str:
        self.send(command)
        return self.read_for(seconds)

    def clear(self) -> None:
        self.buffer = ""
        self.read_for(0.05)
        self.buffer = ""

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def expect(
    condition: bool,
    name: str,
    details: str = "",
) -> None:
    if condition:
        print(f"{name}: PASS")
        return

    print(f"{name}: FAIL")
    if details:
        print(details)
    raise AssertionError(name)


def register_and_login(
    client: Client,
    username: str,
    password: str,
) -> None:
    response = client.command(
        f"REGISTER {username} {password}"
    )
    expect(
        "registration successful" in response,
        f"register {username}",
        response,
    )

    response = client.command(
        f"LOGIN {username} {password}"
    )
    expect(
        "login successful" in response,
        f"login {username}",
        response,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--port", type=int, default=19321)
    args = parser.parse_args()

    server_path = Path(args.server)

    process = subprocess.Popen(
        [str(server_path), str(args.port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    clients = []

    try:
        time.sleep(0.2)

        alice = Client(args.port)
        bob = Client(args.port)
        charlie = Client(args.port)
        guest = Client(args.port)
        clients.extend([alice, bob, charlie, guest])

        register_and_login(alice, "alice", "pass123")
        register_and_login(bob, "bob", "pass456")
        register_and_login(
            charlie,
            "charlie",
            "pass789",
        )

        for client in [alice, bob, charlie, guest]:
            client.clear()

        alice.command("ADD_FRIEND bob")
        bob.read_for(0.2)
        response = bob.command("ACCEPT_FRIEND alice")
        expect(
            "are now friends" in response,
            "accept friendship",
            response,
        )

        for client in [alice, bob, charlie, guest]:
            client.clear()

        alice.command("SAY public-one")
        bob.command("SAY public-two")

        for client in [alice, bob, charlie]:
            client.read_for(0.2)
            client.clear()

        response = alice.command("HISTORY_PUBLIC 1")
        expect(
            "[history public] showing 1 message(s)" in response,
            "public history count",
            response,
        )
        expect(
            "public-two" in response,
            "public history latest message",
            response,
        )
        expect(
            "public-one" not in response,
            "public history excludes older message",
            response,
        )

        response = guest.command("HISTORY_PUBLIC")
        expect(
            "must LOGIN" in response,
            "guest public history rejected",
            response,
        )

        alice.clear()
        bob.clear()
        charlie.clear()

        alice.command("MSG bob private-one")
        bob.read_for(0.2)
        bob.command("MSG alice private-two")
        alice.read_for(0.2)

        alice.clear()
        bob.clear()
        charlie.clear()

        response = alice.command(
            "HISTORY_PRIVATE bob 2",
            0.35,
        )
        expect(
            "[history private with bob] showing 2 message(s)"
            in response,
            "private history count",
            response,
        )
        expect(
            "alice -> bob: private-one" in response,
            "private history first direction",
            response,
        )
        expect(
            "bob -> alice: private-two" in response,
            "private history reverse direction",
            response,
        )
        expect(
            response.find("private-one")
            < response.find("private-two"),
            "private history chronological order",
            response,
        )

        response = bob.command(
            "HISTORY_PRIVATE alice 1",
            0.35,
        )
        expect(
            "private-two" in response,
            "private history latest only",
            response,
        )
        expect(
            "private-one" not in response,
            "private history older excluded",
            response,
        )

        response = charlie.command(
            "HISTORY_PRIVATE bob 10",
            0.35,
        )
        expect(
            "showing 0 message(s)" in response,
            "third party cannot see another conversation",
            response,
        )
        expect(
            "private-one" not in response
            and "private-two" not in response,
            "third party private content isolated",
            response,
        )

        response = alice.command("HISTORY_PUBLIC 0")
        expect(
            "count is 1-100" in response,
            "public history validates zero",
            response,
        )

        response = alice.command(
            "HISTORY_PRIVATE bob 101"
        )
        expect(
            "count is 1-100" in response,
            "private history validates maximum",
            response,
        )

        response = alice.command(
            "HISTORY_PRIVATE nobody 5"
        )
        expect(
            "does not exist" in response,
            "private history validates account",
            response,
        )

        response = alice.command("REMOVE_FRIEND bob")
        expect(
            "removed bob" in response,
            "remove friend",
            response,
        )

        response = alice.command(
            "HISTORY_PRIVATE bob 2",
            0.35,
        )
        expect(
            "private-one" in response
            and "private-two" in response,
            "history remains after friendship removal",
            response,
        )

        print("all v7.1 network regression tests passed")
        return 0

    finally:
        for client in clients:
            client.close()

        process.terminate()

        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)

        if process.stdout is not None:
            output = process.stdout.read()
            if output:
                print("--- server output ---")
                print(output)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"test failed: {exc}", file=sys.stderr)
        raise
