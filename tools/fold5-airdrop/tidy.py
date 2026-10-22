#!/usr/bin/env python3
"""Move what an iPhone AirDrops into the Android gallery folder.

iOS wraps every transfer in an NSIRD_AirDrop_* directory with AppleDouble
("._") files. Flatten that: move the real files from STAGING into DEST, never
overwrite (IMG_1.JPG -> IMG_1-1.JPG), and drop the wrapper.

    tidy.py STAGING DEST [--watch SECONDS]

DEST is a directory, or "mediaprovider:RELPATH" for RELPATH under the phone's
shared storage (/storage/emulated/0). Root's mount namespace on the Fold5 does
not always have a live /storage, so that form goes through MediaProvider's own
namespace via /proc/<pid>/root, looked up again on every pass. If it cannot be
resolved, files stay in STAGING and are retried.
"""
import os
import shutil
import sys
import time

SETTLE_SECONDS = 5  # leave files alone while opendrop may still be writing


MEDIAPROVIDER = (b"com.google.android.providers.media.module", b"com.android.providers.media.module")


def resolve_dest(spec):
    if not spec.startswith("mediaprovider:"):
        return spec
    rel = spec.split(":", 1)[1].strip("/")
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                name = f.read().split(b"\0", 1)[0]
        except OSError:
            continue
        if name in MEDIAPROVIDER:
            root = f"/proc/{pid}/root/storage/emulated/0"
            if os.path.isdir(root):
                return os.path.join(root, rel)
    return None


def unique_path(dest_dir, name):
    base, ext = os.path.splitext(name)
    path = os.path.join(dest_dir, name)
    n = 1
    while os.path.exists(path):
        path = os.path.join(dest_dir, f"{base}-{n}{ext}")
        n += 1
    return path


def settled(path):
    """True once nothing under path has changed for SETTLE_SECONDS."""
    if os.path.isfile(path):
        newest = os.path.getmtime(path)
    else:
        newest = os.path.getmtime(path)  # a just-created, still empty dir counts as busy
        for root, _dirs, files in os.walk(path):
            for f in files:
                try:
                    newest = max(newest, os.path.getmtime(os.path.join(root, f)))
                except OSError:
                    pass
    return time.time() - newest > SETTLE_SECONDS


def is_transfer_artifact(name):
    # Raw containers stay open across long radio pauses. Age is not evidence
    # that an upload has finished; OpenDrop still needs this pathname to extract.
    return (name.startswith("airdrop-received-") and name.endswith((".dvzip", ".cpio"))) or name.endswith(".partial")


def tidy_once(staging, dest_spec):
    moved = []
    dest = resolve_dest(dest_spec)
    if dest is None:
        print("shared storage not reachable; keeping files in staging", file=sys.stderr, flush=True)
        return moved
    try:
        os.makedirs(dest, exist_ok=True)
    except OSError as e:
        print(f"cannot create {dest}: {e}", file=sys.stderr, flush=True)
        return moved
    for entry in sorted(os.listdir(staging)):
        src = os.path.join(staging, entry)
        if is_transfer_artifact(entry):
            continue
        if not settled(src):
            continue
        if os.path.isdir(src):
            for root, _dirs, files in os.walk(src):
                for f in files:
                    if is_transfer_artifact(f):
                        continue
                    if f.startswith("._") or f == ".DS_Store":
                        os.remove(os.path.join(root, f))
                        continue
                    target = unique_path(dest, f)
                    shutil.move(os.path.join(root, f), target)
                    moved.append(target)
            # Keep incomplete members in staging for inspection/recovery.
            for root, _dirs, _files in os.walk(src, topdown=False):
                try:
                    os.rmdir(root)
                except OSError:
                    pass
        elif entry.startswith("._") or entry == ".DS_Store":
            os.remove(src)
        elif not entry.startswith("."):
            target = unique_path(dest, entry)
            shutil.move(src, target)
            moved.append(target)
    for m in moved:
        print(f"received: {os.path.basename(m)}", flush=True)
    return moved


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    staging, dest = sys.argv[1], sys.argv[2]
    if len(sys.argv) >= 5 and sys.argv[3] == "--watch":
        interval = float(sys.argv[4])
        while True:
            try:
                tidy_once(staging, dest)
            except OSError as e:  # e.g. MediaProvider restarted mid-move; retry next pass
                print(f"tidy: {e}", file=sys.stderr, flush=True)
            time.sleep(interval)
    tidy_once(staging, dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
