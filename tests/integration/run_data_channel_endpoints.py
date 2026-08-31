#!/usr/bin/env python3
"""Run local M19 HTTP(S), FTP, and SFTP fixtures against the C++ probe."""

from __future__ import annotations

import argparse
import contextlib
import http.server
import os
import pathlib
import pwd
import shutil
import socket
import socketserver
import ssl
import subprocess
import tempfile
import threading
import time


PAYLOAD = b'{"meter":"m19","value":0}\n'


def run(*arguments: str, capture: bool = False) -> str:
    result = subprocess.run(
        arguments,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return result.stdout if capture else ""


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as value:
        value.bind(("127.0.0.1", 0))
        return int(value.getsockname()[1])


class PostHandler(http.server.BaseHTTPRequestHandler):
    server_version = "M19Integration/1"

    def do_POST(self) -> None:  # noqa: N802 - stdlib callback name
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        self.server.requests.append((self.path, dict(self.headers), body))
        if self.path == "/drop":
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        status = 201
        if self.path == "/retry":
            status = 503
        elif self.path == "/blocked":
            status = 401
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *_: object) -> None:
        return


class ThreadedHttpServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

    def __init__(self, address: tuple[str, int]):
        super().__init__(address, PostHandler)
        self.requests: list[tuple[str, dict[str, str], bytes]] = []


class FtpHandler(socketserver.StreamRequestHandler):
    def setup(self) -> None:
        super().setup()
        self.authenticated = False
        self.pending_user = ""
        self.passive: socket.socket | None = None
        self.rename_from = ""
        self.cwd = ""

    def finish(self) -> None:
        if self.passive is not None:
            self.passive.close()
        super().finish()

    def reply(self, text: str) -> None:
        self.wfile.write((text + "\r\n").encode("ascii"))
        self.wfile.flush()

    def passive_listener(self) -> int:
        if self.passive is not None:
            self.passive.close()
        self.passive = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.passive.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.passive.bind(("127.0.0.1", 0))
        self.passive.listen(1)
        self.passive.settimeout(10)
        return int(self.passive.getsockname()[1])

    def receive_file(self, path: str) -> None:
        if not self.authenticated or self.passive is None:
            self.reply("425 Use passive mode first")
            return
        self.reply("150 Opening binary data connection")
        connection, _ = self.passive.accept()
        with connection:
            chunks = []
            while True:
                chunk = connection.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
        self.server.files[self.normalized(path)] = b"".join(chunks)
        self.passive.close()
        self.passive = None
        self.reply("226 Transfer complete")

    def normalized(self, path: str) -> str:
        if path.startswith("/"):
            return path.lstrip("/")
        return "/".join(part for part in (self.cwd, path) if part)

    def handle(self) -> None:
        self.reply("220 M19 integration FTP ready")
        while line := self.rfile.readline():
            command_line = line.decode("utf-8", "replace").rstrip("\r\n")
            self.server.commands.append(command_line)
            command, _, argument = command_line.partition(" ")
            command = command.upper()
            if command == "USER":
                self.pending_user = argument
                self.reply("331 Password required")
            elif command == "PASS":
                self.authenticated = (
                    self.pending_user == "meter" and argument == "integration-secret"
                )
                self.reply("230 Login successful" if self.authenticated else "530 Login incorrect")
            elif command == "SYST":
                self.reply("215 UNIX Type: L8")
            elif command == "FEAT":
                self.reply("211 No extensions")
            elif command == "PWD":
                self.reply('257 "/" is the current directory')
            elif command == "CWD":
                self.cwd = argument.strip("/")
                self.reply("250 Directory changed")
            elif command == "TYPE":
                self.reply("200 Type set")
            elif command == "EPSV":
                self.reply(f"229 Entering Extended Passive Mode (|||{self.passive_listener()}|)")
            elif command == "PASV":
                port = self.passive_listener()
                self.reply(f"227 Entering Passive Mode (127,0,0,1,{port // 256},{port % 256})")
            elif command == "STOR":
                self.receive_file(argument)
            elif command == "RNFR":
                source = self.normalized(argument)
                if source in self.server.files:
                    self.rename_from = source
                    self.reply("350 Ready for destination name")
                else:
                    self.reply("550 Source does not exist")
            elif command == "RNTO":
                destination = self.normalized(argument)
                if "rename-fail" in argument:
                    self.reply("550 Integration rename rejection")
                elif self.rename_from in self.server.files:
                    self.server.files[destination] = self.server.files.pop(self.rename_from)
                    self.rename_from = ""
                    self.reply("250 Rename successful")
                else:
                    self.reply("550 Rename source missing")
            elif command == "DELE":
                self.server.files.pop(self.normalized(argument), None)
                self.reply("250 Delete successful")
            elif command == "NOOP":
                self.reply("200 OK")
            elif command == "QUIT":
                self.reply("221 Goodbye")
                return
            else:
                self.reply("502 Command not implemented")


class FtpServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address: tuple[str, int]):
        super().__init__(address, FtpHandler)
        self.files: dict[str, bytes] = {}
        self.commands: list[str] = []


def create_certificates(root: pathlib.Path) -> dict[str, pathlib.Path]:
    files = {name: root / name for name in (
        "ca.key", "ca.pem", "server.key", "server.csr", "server.pem",
        "client.key", "client.csr", "client.pem", "wrong-ca.key",
        "wrong-ca.pem", "server.ext", "client.ext",
    )}
    files["server.ext"].write_text(
        "subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n",
        encoding="utf-8",
    )
    files["client.ext"].write_text("extendedKeyUsage=clientAuth\n", encoding="utf-8")
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-subj", "/CN=M19 Integration CA", "-keyout", str(files["ca.key"]),
        "-out", str(files["ca.pem"]), "-days", "1")
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes",
        "-subj", "/CN=localhost", "-keyout", str(files["server.key"]),
        "-out", str(files["server.csr"]))
    run("openssl", "x509", "-req", "-in", str(files["server.csr"]),
        "-CA", str(files["ca.pem"]), "-CAkey", str(files["ca.key"]),
        "-CAcreateserial", "-out", str(files["server.pem"]), "-days", "1",
        "-sha256", "-extfile", str(files["server.ext"]))
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes",
        "-subj", "/CN=m19-client", "-keyout", str(files["client.key"]),
        "-out", str(files["client.csr"]))
    run("openssl", "x509", "-req", "-in", str(files["client.csr"]),
        "-CA", str(files["ca.pem"]), "-CAkey", str(files["ca.key"]),
        "-CAcreateserial", "-out", str(files["client.pem"]), "-days", "1",
        "-sha256", "-extfile", str(files["client.ext"]))
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-subj", "/CN=Wrong M19 CA", "-keyout", str(files["wrong-ca.key"]),
        "-out", str(files["wrong-ca.pem"]), "-days", "1")
    return files


def start_sshd(root: pathlib.Path, port: int) -> tuple[subprocess.Popen[bytes], dict[str, pathlib.Path]]:
    files = {name: root / name for name in (
        "host-key", "client-key", "wrong-client-key", "wrong-host-key",
        "authorized-keys", "known-hosts", "wrong-known-hosts", "sshd.conf",
        "sshd.log", "sshd.pid",
    )}
    for key in ("host-key", "client-key", "wrong-client-key", "wrong-host-key"):
        run("ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(files[key]))
    shutil.copyfile(str(files["client-key"]) + ".pub", files["authorized-keys"])
    user = pwd.getpwuid(os.getuid()).pw_name
    files["sshd.conf"].write_text(
        "\n".join((
            f"Port {port}",
            "ListenAddress 127.0.0.1",
            f"HostKey {files['host-key']}",
            f"PidFile {files['sshd.pid']}",
            f"AuthorizedKeysFile {files['authorized-keys']}",
            "StrictModes no",
            "PasswordAuthentication no",
            "KbdInteractiveAuthentication no",
            "ChallengeResponseAuthentication no",
            "PubkeyAuthentication yes",
            "UsePAM no",
            "PrintMotd no",
            "LogLevel ERROR",
            f"AllowUsers {user}",
            "Subsystem sftp internal-sftp",
            "",
        )),
        encoding="utf-8",
    )
    log = files["sshd.log"].open("wb")
    process = subprocess.Popen(
        ("/usr/sbin/sshd", "-D", "-e", "-f", str(files["sshd.conf"])),
        stdout=log,
        stderr=log,
    )
    for _ in range(100):
        if process.poll() is not None:
            log.close()
            raise RuntimeError(files["sshd.log"].read_text(encoding="utf-8"))
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                break
        except OSError:
            time.sleep(0.02)
    else:
        process.terminate()
        log.close()
        raise RuntimeError("local sshd did not start")
    public = run("ssh-keygen", "-y", "-f", str(files["host-key"]), capture=True).strip()
    wrong_public = run(
        "ssh-keygen", "-y", "-f", str(files["wrong-host-key"]), capture=True
    ).strip()
    prefix = f"[127.0.0.1]:{port} "
    files["known-hosts"].write_text(prefix + public + "\n", encoding="utf-8")
    files["wrong-known-hosts"].write_text(prefix + wrong_public + "\n", encoding="utf-8")
    process._m19_log = log
    return process, files


@contextlib.contextmanager
def running(server: socketserver.BaseServer):
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("probe", type=pathlib.Path)
    arguments = parser.parse_args()
    probe = arguments.probe.resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="m19-data-channel-") as temporary:
        root = pathlib.Path(temporary)
        certificates = create_certificates(root)
        http_server = ThreadedHttpServer(("127.0.0.1", free_port()))
        https_server = ThreadedHttpServer(("127.0.0.1", free_port()))
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(certificates["server.pem"], certificates["server.key"])
        tls.load_verify_locations(certificates["ca.pem"])
        tls.verify_mode = ssl.CERT_REQUIRED
        https_server.socket = tls.wrap_socket(https_server.socket, server_side=True)
        ftp_server = FtpServer(("127.0.0.1", free_port()))
        sftp_port = free_port()
        sftp_root = root / "sftp"
        sftp_root.mkdir()
        sshd, ssh_files = start_sshd(root, sftp_port)
        try:
            environment = os.environ.copy()
            environment.update({
                "M19_HTTP_PORT": str(http_server.server_address[1]),
                "M19_HTTPS_PORT": str(https_server.server_address[1]),
                "M19_FTP_PORT": str(ftp_server.server_address[1]),
                "M19_SFTP_PORT": str(sftp_port),
                "M19_CA_FILE": str(certificates["ca.pem"]),
                "M19_WRONG_CA_FILE": str(certificates["wrong-ca.pem"]),
                "M19_CLIENT_CERT_FILE": str(certificates["client.pem"]),
                "M19_CLIENT_KEY_FILE": str(certificates["client.key"]),
                "M19_SFTP_USER": pwd.getpwuid(os.getuid()).pw_name,
                "M19_SFTP_REMOTE_DIRECTORY": str(sftp_root),
                "M19_SFTP_PRIVATE_KEY_FILE": str(ssh_files["client-key"]),
                "M19_SFTP_WRONG_PRIVATE_KEY_FILE": str(ssh_files["wrong-client-key"]),
                "M19_SFTP_KNOWN_HOSTS_FILE": str(ssh_files["known-hosts"]),
                "M19_SFTP_WRONG_KNOWN_HOSTS_FILE": str(ssh_files["wrong-known-hosts"]),
            })
            with running(http_server), running(https_server), running(ftp_server):
                try:
                    subprocess.run((str(probe),), check=True, env=environment)
                except subprocess.CalledProcessError:
                    print("FTP control transcript:", ftp_server.commands)
                    raise

            accepted = [entry for entry in http_server.requests if entry[0] == "/accepted"]
            if len(accepted) != 1 or accepted[0][2] != PAYLOAD:
                raise RuntimeError("HTTP fixture did not receive the exact artifact body")
            headers = {name.lower(): value for name, value in accepted[0][1].items()}
            if headers.get("idempotency-key") != "integration-artifact":
                raise RuntimeError("HTTP fixture did not receive the stable idempotency key")
            if ftp_server.files.get("incoming/integration-artifact.json") != PAYLOAD:
                raise RuntimeError("FTP fixture did not retain the atomically renamed payload")
            if "incoming/.integration-artifact.json.part" in ftp_server.files:
                raise RuntimeError("FTP fixture retained a successful temporary filename")
            if (sftp_root / "integration-artifact.json").read_bytes() != PAYLOAD:
                raise RuntimeError("SFTP fixture did not retain the atomically renamed payload")
            if (sftp_root / ".integration-artifact.json.part").exists():
                raise RuntimeError("SFTP fixture retained a successful temporary filename")
        finally:
            sshd.terminate()
            try:
                sshd.wait(timeout=3)
            except subprocess.TimeoutExpired:
                sshd.kill()
                sshd.wait(timeout=3)
            sshd._m19_log.close()

    print("M19 real local endpoint matrix passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
