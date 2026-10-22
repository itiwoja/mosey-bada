#!/usr/bin/env python3
"""Build the read-only probe using an existing Android NDK; no downloads."""
import argparse
import os
from pathlib import Path
import platform
import subprocess


def windows_path(path):
    return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tool", choices=["radio-probe", "monitor-session", "binder-probe", "mosey-adapter", "mosey-start-probe", "awdl-bridge"], default="radio-probe")
    args = parser.parse_args()
    prebuilt = args.ndk.resolve() / "toolchains/llvm/prebuilt"
    if os.name == "nt":
        host = "windows-x86_64"
    elif platform.system() == "Darwin":
        host = "darwin-x86_64"
    elif (prebuilt / "linux-x86_64/bin/clang").is_file():
        host = "linux-x86_64"
    elif "microsoft" in platform.release().lower():
        host = "windows-x86_64"
    else:
        parser.error("No compatible compiler found in this NDK")
    root = prebuilt / host
    compiler = root / "bin" / ("clang.exe" if host.startswith("windows") else "clang")
    if not compiler.is_file():
        parser.error(f"Compiler missing: {compiler}")
    name = {"radio-probe": "radio_probe.c", "monitor-session": "monitor_session.c",
            "binder-probe": "binder_probe.c", "mosey-adapter": "mosey_path_redirect.c",
            "mosey-start-probe": "mosey_start_probe.c", "awdl-bridge": "awdl_bridge.c"}[args.tool]
    source = Path(__file__).resolve().parents[1] / "src/fold5" / name
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    convert = windows_path if host.startswith("windows") and os.name != "nt" else str
    link = ["-shared", "-fPIC"] if args.tool == "mosey-adapter" else ["-fPIE", "-pie"]
    libs = ["-ldl", "-llog"] if args.tool == "mosey-adapter" else (["-ldl"] if args.tool == "binder-probe" else [])
    if args.tool == "mosey-start-probe":
        libs = ["-lbinder_ndk", "-ldl", "-llog", "-pthread"]
    subprocess.run([str(compiler), "--target=aarch64-linux-android36",
                    "--sysroot=" + convert(root / "sysroot"),
                    "-O2", "-Wall", "-Wextra", "-Werror", *link,
                    convert(source), "-o", convert(output), *libs], check=True)
    print(f"Built {args.tool}: {output}")


if __name__ == "__main__":
    main()
