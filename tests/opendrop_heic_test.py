"""Check the /Discover answer of the HEIC-patched OpenDrop without its optional dependencies."""
import ast
import io
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILT = ROOT / "work/fold5-airdrop-build/rootfs/usr/lib/python3.12/site-packages/opendrop/server.py"
PATCH = ROOT / "tools/fold5-airdrop/opendrop-heic.patch"

# Same order as package.sh: the built server.py already has the stream patch,
# so only apply the HEIC patch to a temporary copy of it.
tmp = tempfile.mkdtemp()
(Path(tmp) / "opendrop").mkdir()
shutil.copy(BUILT, Path(tmp) / "opendrop/server.py")
if "AIRDROP_HEIC" not in (Path(tmp) / "opendrop/server.py").read_text():
    subprocess.run(["git", "apply", str(PATCH)], cwd=tmp, check=True, env={**os.environ, "GIT_DIR": "/nonexistent"})
SOURCE = Path(tmp) / "opendrop/server.py"
tree = ast.parse(SOURCE.read_text())
func = next(
    n for c in ast.walk(tree) if isinstance(c, ast.ClassDef) for n in c.body
    if isinstance(n, ast.FunctionDef) and n.name == "handle_discover"
)
namespace = {"json": json, "os": os, "plistlib": plistlib, "AirDropUtil": types.SimpleNamespace(write_debug=lambda *a, **k: None)}
exec(compile(ast.Module(body=[func], type_ignores=[]), str(SOURCE), "exec"), namespace)
handle_discover = namespace["handle_discover"]


class FakeHandler:
    def __init__(self):
        self.config = types.SimpleNamespace(computer_name="n", computer_model="m", record_data=None)
        self.wfile = io.BytesIO()
        self.length = None

    def _read_request_body(self):
        return b""

    def _set_response(self, length):
        self.length = length


def discover(heic):
    old = os.environ.pop("AIRDROP_HEIC", None)
    if heic is not None:
        os.environ["AIRDROP_HEIC"] = heic
    try:
        h = FakeHandler()
        handle_discover(h)
    finally:
        os.environ.pop("AIRDROP_HEIC", None)
        if old is not None:
            os.environ["AIRDROP_HEIC"] = old
    data = h.wfile.getvalue()
    assert data.startswith(b"bplist00") and h.length == len(data)
    return json.loads(plistlib.loads(data)["ReceiverMediaCapabilities"])


class HeicTest(unittest.TestCase):
    def test_default_and_one_declare_heif(self):
        for value in (None, "1"):
            caps = discover(value)
            self.assertEqual(
                caps["ContainerFormats"],
                {"public.heif-standard": {"HeifSubtypes": ["public.avci", "public.heic", "public.heif"]}},
            )
            self.assertEqual(caps["Version"], 1)
            # HEIC stills are hvc1 in HEIF: both must be declared or iOS sends JPEG
            self.assertEqual(caps["Codecs"]["hvc1"]["Profiles"]["VTSupportedProfiles"], [1, 2, 3])
            # Vendor names the OS, but no LivePhotoFormatVersion: Live Photos must not bring their MOV
            self.assertEqual(caps["Vendor"]["com.apple"]["OSVersion"], [10, 13, 3])
            self.assertNotIn("LivePhotoFormatVersion", caps["Vendor"]["com.apple"])

    def test_zero_keeps_legacy(self):
        self.assertEqual(discover("0"), {"Version": 1})


if __name__ == "__main__":
    unittest.main()
