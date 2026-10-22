"""Exercise the patched reader without importing OpenDrop's optional dependencies."""
import ast
import io
from pathlib import Path
import socket
import types
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "work/fold5-airdrop-build/rootfs/usr/lib/python3.12/site-packages/opendrop/server.py"
tree = ast.parse(SOURCE.read_text())
reader_node = next(n for n in ast.walk(tree) if isinstance(n, ast.ClassDef) and n.name == "HTTPChunkedReader")
namespace = {"io": io}
exec(compile(ast.Module(body=[reader_node], type_ignores=[]), str(SOURCE), "exec"), namespace)
Reader = namespace["HTTPChunkedReader"]


class BurstStream(io.BytesIO):
    def read(self, size=-1):
        raise AssertionError("blocking whole-chunk read")

    def read1(self, size=-1):
        return super().read(min(size, 1024))


class StreamTest(unittest.TestCase):
    def test_large_chunk_yields_first_burst_and_completes(self):
        payload = bytes(range(256)) * 24576  # 6 MiB in one HTTP chunk
        stream = BurstStream(b"600000;extension=yes\r\n" + payload + b"\r\n0\r\nX-Test: ok\r\n\r\nNEXT")
        reader = Reader(stream)
        first = reader.read(65536)
        self.assertEqual(first, payload[:1024])
        self.assertLess(stream.tell(), 2048)
        out = bytearray(first)
        while data := reader.read(65536):
            out.extend(data)
        self.assertEqual(out, payload)
        self.assertEqual(reader.total, len(payload))
        self.assertEqual(stream.read1(4), b"NEXT")
        self.assertEqual(reader.read(1), b"")

    def test_truncation_preserves_preceding_burst(self):
        reader = Reader(BurstStream(b"100000\r\n" + b"x" * 1024))
        self.assertEqual(reader.read(65536), b"x" * 1024)
        with self.assertRaises(ValueError):
            reader.read(65536)
        self.assertEqual(reader.total, 1024)

    def test_chunk_boundaries_and_invalid_framing(self):
        reader = Reader(BurstStream(b"1\r\na\r\n2\r\nbc\r\n0\r\n\r\n"))
        self.assertEqual(reader.read(9), b"a")
        self.assertEqual(reader.read(9), b"bc")
        self.assertEqual(reader.read(9), b"")
        for wire in [b"-1\r\n", b"xyz\r\n", b"2\nxx", b"1\r\naWRONG\r\n"]:
            reader = Reader(BurstStream(wire))
            with self.assertRaises(ValueError):
                while reader.read(10):
                    pass

    def test_receive_buffer_is_configured_on_server_socket(self):
        node = next(n for n in ast.walk(tree) if isinstance(n, ast.FunctionDef) and n.name == "_init_server")
        messages = []
        logger = types.SimpleNamespace(debug=messages.append, warning=messages.append)
        ns = {"socket": socket, "logger": logger, "platform": types.SimpleNamespace(system=lambda: "Linux")}
        exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), "exec"), ns)
        with socket.socket() as sock:
            server = types.SimpleNamespace(socket=sock)
            context = types.SimpleNamespace(wrap_socket=lambda sock, server_side: sock)
            obj = types.SimpleNamespace(ServerClass=lambda *args: server, serveraddress=("::", 0), Handler=None,
                                        config=types.SimpleNamespace(interface="awdl0", get_ssl_context=lambda: context))
            self.assertIs(ns["_init_server"](obj), server)
            self.assertTrue(any("SO_RCVBUF" in msg for msg in messages))
            self.assertGreater(sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF), 0)


if __name__ == "__main__":
    unittest.main()
