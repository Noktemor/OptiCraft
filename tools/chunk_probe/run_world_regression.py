#!/usr/bin/env python3
"""Run the headless chunk/fluid regression using an existing Ninja build.

The probe links the same objects and libraries as OptiCraft, except for main.cpp.
Its world is created in a temporary directory and discarded after the run.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="configured Ninja build directory")
    parser.add_argument("--probe", choices=("world", "biome"), default="world")
    args = parser.parse_args()
    build = args.build.resolve()
    root = Path(__file__).resolve().parents[2]
    subprocess.run(["cmake", "--build", str(build), "--target", "OptiCraft"], check=True)
    database = json.loads((build / "compile_commands.json").read_text())
    game = next(entry for entry in database if entry["file"].endswith("/src/core/game/Game.cpp"))
    commands = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", "OptiCraft"], text=True)
    link_line = commands.splitlines()[-1]
    # Ninja's CMake link rule includes a trailing asset-copy command.
    link = shlex.split(link_line.split(" && ")[1])
    link.remove("CMakeFiles/OptiCraft.dir/main.cpp.o")

    with tempfile.TemporaryDirectory(prefix="opticraft-world-regression-") as temp:
        temp = Path(temp)
        obj = temp / "probe.o"
        exe = temp / "probe"
        compile_cmd = shlex.split(game["command"])
        compile_cmd[compile_cmd.index("-o") + 1] = str(obj)
        compile_cmd[compile_cmd.index("-c") + 1] = str(root / f"tools/chunk_probe/{args.probe}_regression.cpp")
        subprocess.run(compile_cmd, cwd=build, check=True)
        link[link.index("-o") + 1] = str(exe)
        link.append(str(obj))
        subprocess.run(link, cwd=build, check=True)
        subprocess.run([str(exe), str(temp / "world")], cwd=root, check=True)


if __name__ == "__main__":
    main()
