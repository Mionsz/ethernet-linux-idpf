import os
from pathlib import Path
import secrets
import select
import socket
import subprocess
import sys


def listen(address, source):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
        receiver.bind((address, 0))
        receiver.settimeout(5)
        print(receiver.getsockname()[1], flush=True)
        for _ in range(100):
            payload, sender = receiver.recvfrom(2048)
            if sender[0] != source or len(payload) != 1400:
                raise RuntimeError("unexpected UDP sender or payload size")
            receiver.sendto(payload, sender)


def transfer(peer, source, command):
    server = subprocess.Popen(
        command + [sys.executable, str(Path(__file__).resolve()), "--listen", peer, source],
        stdout=subprocess.PIPE,
        text=True,
    )
    try:
        if not select.select([server.stdout], [], [], 5)[0]:
            raise TimeoutError("peer UDP receiver did not become ready")
        port = int(server.stdout.readline().strip())
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            sender.bind((source, 0))
            sender.connect((peer, port))
            sender.settimeout(2)
            token = secrets.token_bytes(16)
            for sequence in range(100):
                payload = token + sequence.to_bytes(4, "big") + b"x" * 1380
                sender.send(payload)
                if sender.recv(2048) != payload:
                    raise RuntimeError("UDP payload mismatch")
        if server.wait(timeout=5) != 0:
            raise RuntimeError("peer UDP receiver failed")
        print("PASS: 100 UDP payloads of 1400 bytes received and echoed intact")
    finally:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
        if server.stdout is not None:
            server.stdout.close()


if __name__ == "__main__":
    try:
        if len(sys.argv) == 4 and sys.argv[1] == "--listen":
            listen(sys.argv[2], sys.argv[3])
        elif sys.argv[1:] == ["--self-test"]:
            transfer("127.0.0.1", "127.0.0.1", [])
        elif len(sys.argv) == 1:
            partner = str(Path(__file__).with_name("link-partner.sh"))
            subprocess.run(["sh", partner, "confirm"], check=True)
            transfer(
                os.environ.get("TESTPEER", "192.168.211.2"),
                os.environ.get("TESTIP", "192.168.211.1/24").split("/")[0],
                ["sh", partner, "exec"],
            )
        else:
            raise ValueError("usage: peer-udp.py [--self-test]")
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL: UDP transfer: {error}", file=sys.stderr)
        sys.exit(1)