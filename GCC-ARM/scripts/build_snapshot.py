"""Compile an existing Coffee2 Ninja graph without Ninja's job scheduler.

Diagnostic fallback only: does not configure, clean, download or flash.
Uses generated compile/link commands, recompiles EVERY translation unit,
and refuses a changed source snapshot. Normal build.ps1 remains unchanged.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

SHELL = Path(__file__).resolve().parents[1]
ROOT = SHELL.parent
BUILD = SHELL / "build/Coffee2-Debug"
BIN = SHELL / ".tools/arm-gnu-toolchain/bin"
NINJA = SHELL / ".tools/python/bin/ninja.exe"


def snapshot():
    paths = []
    for directory in (ROOT / "Application", ROOT / "CubeMX_Base", SHELL / "Platform",
                      SHELL / "linker", SHELL / "cmake"):
        paths.extend(p for p in directory.rglob("*")
                     if p.suffix.lower() in (".c", ".h", ".s", ".ld", ".cmake"))
    paths += [ROOT / "Application/CMakeLists.txt", SHELL / "CMakeLists.txt"]
    return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(set(paths))}


def main():
    commands = json.loads((BUILD / "compile_commands.json").read_text())
    cache = (BUILD / "CMakeCache.txt").read_text()
    if not re.search(r"^PRODUCT_NAME:.*=Coffee2$", cache, re.M):
        raise RuntimeError("Not a Coffee2 build tree")
    graph = (BUILD / "build.ninja").read_text()
    match = re.search(r"^build Coffee2Target\.elf: \S+ (.+)\n((?:  .*\n)*)", graph, re.M)
    if not match:
        raise RuntimeError("Executable rule missing")
    objects = match[1].split(" |", 1)[0]
    libraries = re.search(r"^  LINK_LIBRARIES = (.*)$", match[2], re.M)
    if not libraries:
        raise RuntimeError("Link libraries missing")
    link_commands = subprocess.check_output(
        [str(NINJA), "-t", "commands", "Coffee2Target.elf"], cwd=BUILD, text=True)
    link = link_commands.splitlines()[-1]
    if "@CMakeFiles\\Coffee2Target.rsp" not in link or "--gc-sections" not in link:
        raise RuntimeError("Unexpected link command")
    before = snapshot()
    started = time.time()
    log_path = BUILD / "snapshot_build.log"
    with log_path.open("w", encoding="utf-8") as log:
        def run(command):
            result = subprocess.run(command, cwd=BUILD, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True,
                                    errors="replace", timeout=120)
            log.write(str(command) + "\n" + result.stdout)
            log.flush()
            if result.returncode:
                print(result.stdout)
                raise RuntimeError(f"Build step failed: {result.returncode}")
            if result.stdout:
                print(result.stdout, end="", flush=True)

        for index, entry in enumerate(commands, 1):
            if Path(entry["directory"]).resolve() != BUILD.resolve():
                raise RuntimeError("Unexpected compile working directory")
            output = Path(entry["output"])
            if not output.is_relative_to(BUILD):
                raise RuntimeError("Object path outside build tree")
            output.parent.mkdir(parents=True, exist_ok=True)
            run(entry["command"])
            if index % 20 == 0 or index == len(commands):
                print(f"Compiled {index}/{len(commands)}", flush=True)

        archive = "nanomodbus/libnanomodbus.a"
        run([str(BIN / "arm-none-eabi-ar.exe"), "rcs", archive,
             "nanomodbus/CMakeFiles/nanomodbus.dir/Src/nanomodbus.c.obj"])
        members = subprocess.check_output(
            [str(BIN / "arm-none-eabi-ar.exe"), "t", archive], cwd=BUILD, text=True)
        if members.splitlines() != ["nanomodbus.c.obj"]:
            raise RuntimeError("Archive contains unexpected stale members")
        (BUILD / "CMakeFiles/Coffee2Target.rsp").write_text(
            objects + " " + libraries[1] + "\n", encoding="ascii")
        run(link)
        if before != snapshot():
            raise RuntimeError("Sources changed during compilation; do not use artifacts")
        for suffix in ("elf", "hex", "bin", "map"):
            artifact = BUILD / ("Coffee2Target." + suffix)
            if artifact.stat().st_mtime < started:
                raise RuntimeError("Stale artifact: " + str(artifact))
        report = {"result": "PASS", "method": "direct generated commands, not Ninja build",
                  "translation_units": len(commands), "source_hashes": before,
                  "artifact_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                      for p in BUILD.glob("Coffee2Target.*")
                                      if p.suffix in (".elf", ".hex", ".bin", ".map")}}
        (BUILD / "snapshot_manifest.json").write_text(
            json.dumps(report, indent=2), encoding="utf-8")
        print("PASS: fresh GCC snapshot; log=" + str(log_path), flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("FAILED: " + str(error), file=sys.stderr)
        sys.exit(1)
