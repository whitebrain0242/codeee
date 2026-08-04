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
                continue

        return "".join(chunks)

    def command(
        self,
        command: str,
        seconds: float = 0.5,
    ) -> str:
        self.sock.sendall(
            (command + "\n").encode()
        )
        return self.read_for(seconds)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


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
        output = (
            process.stdout.read()
            if process.stdout is not None
            else ""
        )
        raise RuntimeError(
            "server failed to start:\n" + output
        )

    return process


def stop_server(
    process: subprocess.Popen,
) -> None:
    process.terminate()

    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def register_and_login(
    client: Client,
    username: str,
    password: str,
) -> None:
    response = client.command(
        f"REGISTER {username} {password}"
    )
    assert (
        "registration successful" in response
    ), response

    response = client.command(
        f"LOGIN {username} {password}"
    )
    assert "login successful" in response, response


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument(
        "--port",
        type=int,
        default=19322,
    )
    args = parser.parse_args()

    suffix = uuid.uuid4().hex[:8]
    alice_name = "a_" + suffix
    bob_name = "b_" + suffix

    alice_password = "PassA_" + suffix
    bob_password = "PassB_" + suffix

    first_server = start_server(
        args.server,
        args.port,
        args.config,
    )

    try:
        alice = Client(args.port)
        bob = Client(args.port)

        register_and_login(
            alice,
            alice_name,
            alice_password,
        )

        register_and_login(
            bob,
            bob_name,
            bob_password,
        )

        response = alice.command(
            f"ADD_FRIEND {bob_name}"
        )
        assert "friend request sent" in response, response

        bob.read_for(0.2)

        response = bob.command(
            f"ACCEPT_FRIEND {alice_name}"
        )
        assert "are now friends" in response, response

        response = alice.command(
            "FRIEND_EVENTS 10"
        )
        assert (
            f"{alice_name} sent a friend request "
            f"to {bob_name}"
        ) in response, response

        assert (
            f"{bob_name} accepted "
            f"{alice_name}'s friend request"
        ) in response, response

        alice.close()
        bob.close()
    finally:
        stop_server(first_server)

    second_server = start_server(
        args.server,
        args.port,
        args.config,
    )

    try:
        alice = Client(args.port)
        bob = Client(args.port)

        response = alice.command(
            f"LOGIN {alice_name} {alice_password}"
        )
        assert "login successful" in response, response

        response = bob.command(
            f"LOGIN {bob_name} {bob_password}"
        )
        assert "login successful" in response, response

        response = alice.command("FRIENDS")
        assert (
            f"{bob_name} [online]" in response
        ), response

        response = bob.command("FRIENDS")
        assert (
            f"{alice_name} [online]" in response
        ), response

        response = alice.command(
            "FRIEND_EVENTS 10"
        )
        assert (
            "sent a friend request" in response
            and "accepted" in response
        ), response

        response = alice.command(
            f"REMOVE_FRIEND {bob_name}"
        )
        assert (
            f"removed {bob_name}" in response
        ), response

        alice.close()
        bob.close()
    finally:
        stop_server(second_server)

    third_server = start_server(
        args.server,
        args.port,
        args.config,
    )

    try:
        alice = Client(args.port)
        bob = Client(args.port)

        response = alice.command(
            f"LOGIN {alice_name} {alice_password}"
        )
        assert "login successful" in response, response

        response = bob.command(
            f"LOGIN {bob_name} {bob_password}"
        )
        assert "login successful" in response, response

        response = alice.command("FRIENDS")
        assert (
            f"{bob_name} [online]" not in response
        ), response

        response = alice.command(
            "FRIEND_EVENTS 1"
        )
        assert (
            f"{alice_name} removed {bob_name}"
            in response
        ), response

        alice.close()
        bob.close()
    finally:
        stop_server(third_server)

    print("account persistence: PASS")
    print("friend request persistence: PASS")
    print("friendship persistence: PASS")
    print("Protobuf friend event persistence: PASS")
    print("friend removal persistence: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
