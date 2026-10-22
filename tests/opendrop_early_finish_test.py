"""Run the early-finish OpenDrop /Upload loop over real sockets.

handle_upload is lifted out of the patched server.py and run against a fake
handler whose connection is a real localhost TCP (optionally TLS) socket, so
the quiet wait, the buffered-data check and the shortened blocking read are the
shipped code. libarchive is replaced by a tiny ODC extractor.
"""
import ast
import email.message
import io
import logging
import os
from pathlib import Path
import shutil
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import types
import unittest
import zlib
import mmap
import select

ROOT = Path(__file__).resolve().parents[1]
BUILT = ROOT / "work/fold5-airdrop-build/rootfs/usr/lib/python3.12/site-packages/opendrop/server.py"
PATCHES = [ROOT / "tools/fold5-airdrop/opendrop-heic.patch", ROOT / "tools/fold5-airdrop/opendrop-early-finish.patch"]

# Same order as package.sh: the built server.py predates the HEIC and
# early-finish patches, so apply whichever are missing to a temporary copy.
tmp = tempfile.mkdtemp()
(Path(tmp) / "opendrop").mkdir()
shutil.copy(BUILT, Path(tmp) / "opendrop/server.py")
for marker, patch in (("AIRDROP_HEIC", PATCHES[0]), ("AIRDROP_DONE_QUIET", PATCHES[1])):
    if marker not in (Path(tmp) / "opendrop/server.py").read_text():
        subprocess.run(["git", "apply", str(patch)], cwd=tmp, check=True, env={**os.environ, "GIT_DIR": "/nonexistent"})
SOURCE = Path(tmp) / "opendrop/server.py"
tree = ast.parse(SOURCE.read_text())
func = next(
    n for c in ast.walk(tree) if isinstance(c, ast.ClassDef) for n in c.body
    if isinstance(n, ast.FunctionDef) and n.name == "handle_upload"
)

LOG = []
logger = logging.getLogger("early_finish_test")
logger.setLevel(logging.DEBUG)
logger.addHandler(type("H", (logging.Handler,), {"emit": lambda self, r: LOG.append(r.getMessage())})())


def odc(name, data=b"", mode=0o100644):
    n = name.encode() + b"\0"
    head = b"070707" + b"000000" * 2 + b"%06o" % mode + b"000000" * 3 + b"000000" + b"00000000000" \
        + b"%06o" % len(n) + b"%011o" % len(data)
    assert len(head) == 76
    return head + n + data


def fake_extract(cpio):
    off = 0
    while off + 76 <= len(cpio) and cpio[off:off + 6] == b"070707":
        mode = int(cpio[off + 18:off + 24], 8)
        ns, fs = int(cpio[off + 59:off + 65], 8), int(cpio[off + 65:off + 76], 8)
        name = cpio[off + 76:off + 76 + ns - 1].decode()
        if name == "TRAILER!!!":
            break
        data = cpio[off + 76 + ns:off + 76 + ns + fs]
        if mode & 0o170000 == 0o100000:
            # like libarchive: a cut member is padded out to its declared size
            Path(name).parent.mkdir(parents=True, exist_ok=True)
            Path(name).write_bytes(data.ljust(fs, b"\0"))
        off += 76 + ns + fs
    if off + 76 > len(cpio):
        raise ValueError("truncated archive")


def dvzip(cpio):
    out = b""
    for i in range(0, len(cpio), 131072):
        z = zlib.compress(cpio[i:i + 131072])
        out += struct.pack(">I", len(z)) + z
    return out


libarchive = types.SimpleNamespace(
    extract_memory=fake_extract,
    read=types.SimpleNamespace(stream_reader=lambda s: types.SimpleNamespace(
        __enter__=lambda: s, __exit__=lambda *a: False)),
    extract=types.SimpleNamespace(extract_entries=None),
)


class Handler:
    UPLOAD_CONTENT_TYPES = ("application/x-cpio", "application/x-dvzip")

    def __init__(self, conn, headers):
        self.connection = conn
        self.rfile = conn.makefile("rb")   # same as socketserver: BufferedReader
        self.headers = headers
        self.status = None

    def send_response(self, code):
        self.status = code

    def send_header(self, *a):
        pass

    def end_headers(self):
        pass


ns = {
    "io": io, "os": os, "socket": socket, "ssl": ssl, "select": select, "struct": struct, "time": time,
    "zlib": zlib, "mmap": mmap, "logger": logger, "libarchive": libarchive,
}
exec(compile(ast.Module(body=[func], type_ignores=[]), str(SOURCE), "exec"), ns)
handle_upload = ns["handle_upload"]


def make_tls_contexts():
    d = tempfile.mkdtemp()
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{d}/k.pem",
                    "-out", f"{d}/c.pem", "-subj", "/CN=t", "-days", "2"], check=True, capture_output=True)
    srv = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    srv.load_cert_chain(f"{d}/c.pem", f"{d}/k.pem")
    cli = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    cli.check_hostname = False
    cli.verify_mode = ssl.CERT_NONE
    return srv, cli


def run_upload(pieces, total, quiet="1", stall="4", tls=None, hold=8.0):
    """pieces: byte strings sent one after another with a short pause; then the
    sender goes silent (socket open) for `hold` seconds unless pieces end in a
    terminator. Returns (elapsed, handler, cwd)."""
    old_cwd = os.getcwd()
    cwd = tempfile.mkdtemp()
    os.chdir(cwd)
    env = {"AIRDROP_DONE_QUIET": quiet, "AIRDROP_STALL_TIMEOUT": stall, "AIRDROP_STALL_FLOOR": stall}
    saved = {k: os.environ.get(k) for k in env}
    os.environ.update(env)
    LOG.clear()
    lst = socket.socket()
    lst.bind(("127.0.0.1", 0))
    lst.listen(1)
    release = threading.Event()

    def sender():
        c = socket.create_connection(lst.getsockname())
        if tls:
            c = tls[1].wrap_socket(c, server_hostname="t")
        for p in pieces:
            c.sendall(p)
            time.sleep(0.05)
        release.wait(hold)
        c.close()

    t = threading.Thread(target=sender, daemon=True)
    t.start()
    conn, _ = lst.accept()
    if tls:
        conn = tls[0].wrap_socket(conn, server_side=True)
    h = Handler(conn, email.message.Message())
    h.headers["content-type"] = "application/x-dvzip"
    h.headers["transfer-encoding"] = "chunked"
    if total is not None:
        h.headers["TotalBytes"] = str(total)
    start = time.monotonic()
    try:
        handle_upload(h)
    finally:
        elapsed = time.monotonic() - start
        release.set()
        t.join()
        conn.close()
        lst.close()
        os.chdir(old_cwd)
        for k, v in saved.items():
            os.environ.pop(k, None) if v is None else os.environ.__setitem__(k, v)
    return elapsed, h, Path(cwd)


def chunk(b):
    return b"%x\r\n" % len(b) + b + b"\r\n"


PHOTO = os.urandom(200_000)
OTHER = os.urandom(120_000)


def archive(*files, trailer=False):
    cpio = odc("wrap", mode=0o040755)
    for name, data in files:
        cpio += odc("wrap/" + name, data)
    if trailer:
        cpio += odc("TRAILER!!!") + b"\0" * 8
    return dvzip(cpio)


class EarlyFinishTest(unittest.TestCase):
    def check_complete_without_terminator(self, tls=None):
        wire = chunk(archive(("a.jpg", PHOTO), ("b.jpg", OTHER)))
        elapsed, h, cwd = run_upload([wire], len(PHOTO) + len(OTHER), tls=tls)
        self.assertEqual(h.status, 200)
        self.assertTrue(any("upload complete by content: 2 files" in m for m in LOG), LOG)
        self.assertGreaterEqual(elapsed, 1.0)
        self.assertLess(elapsed, 2.5)   # not the 4 s stall timeout
        self.assertEqual((cwd / "wrap/a.jpg").read_bytes(), PHOTO)
        self.assertEqual((cwd / "wrap/b.jpg").read_bytes(), OTHER)
        self.assertEqual(list(cwd.glob("**/*.partial")), [])
        self.assertEqual(list(cwd.glob("airdrop-received-*")), [])   # raw removed: extraction succeeded

    def test_a_all_files_then_silence_finishes_early(self):
        self.check_complete_without_terminator()

    def test_a_tls(self):
        self.check_complete_without_terminator(make_tls_contexts())

    def test_b_silence_midway_waits_for_the_normal_timeout(self):
        full = archive(("a.jpg", PHOTO), ("b.jpg", OTHER))
        cut = full[:-60_000]   # the second file never fully arrives
        elapsed, h, cwd = run_upload([b"%x\r\n" % len(cut) + cut], len(PHOTO) + len(OTHER))
        self.assertFalse(any("complete by content" in m for m in LOG), LOG)
        self.assertTrue(any("went quiet" in m for m in LOG), LOG)
        self.assertGreaterEqual(elapsed, 3.5)   # the old timeout, not shortened by the check
        self.assertLess(elapsed, 5.0)           # ... and not stretched by done_quiet either
        self.assertEqual((cwd / "wrap/a.jpg").read_bytes(), PHOTO)
        self.assertFalse((cwd / "wrap/b.jpg").exists())
        self.assertTrue((cwd / "wrap/b.jpg.partial").exists())

    def test_b_no_or_bad_total_bytes_never_finishes_early(self):
        wire = chunk(archive(("a.jpg", PHOTO)))
        for total in (None, "junk"):
            elapsed, h, cwd = run_upload([wire], None, stall="2")
            self.assertGreaterEqual(elapsed, 1.8)
            self.assertFalse(any("complete by content" in m for m in LOG), LOG)

    def test_c_normal_end_is_not_delayed(self):
        for tail in ([b"0\r\n\r\n"], []):
            body = chunk(archive(("a.jpg", PHOTO), trailer=True))
            # second variant: the terminator rides in the same TCP segment as the last data
            pieces = [body, b"0\r\n\r\n"] if tail else [body + b"0\r\n\r\n"]
            elapsed, h, cwd = run_upload(pieces, len(PHOTO), quiet="2", stall="6")
            self.assertLess(elapsed, 1.0)   # no quiet wait even though select() sees nothing
            self.assertFalse(any("complete by content" in m for m in LOG), LOG)
            self.assertEqual((cwd / "wrap/a.jpg").read_bytes(), PHOTO)


if __name__ == "__main__":
    unittest.main()
