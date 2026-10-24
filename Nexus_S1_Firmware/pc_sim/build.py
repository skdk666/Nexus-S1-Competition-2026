"""Build current shared UI/LVGL with SDL2; products stay in .pio/pc_sim.

Python 3.9+, MinGW GCC and SDL2 development files are required. No downloads.
Use PATH or NEXUS_HOST_CXX_DIR for GCC; SDL2_ROOT can select another MinGW SDK.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

SIM = Path(__file__).resolve().parent
PROJECT = SIM.parent
BUILD = PROJECT / ".pio" / "pc_sim"
LVGL = PROJECT / ".pio" / "libdeps" / "esp32-s3-devkitc-1" / "lvgl"


def run(command, env):
    result = subprocess.run([str(arg) for arg in command], env=env,
                            capture_output=True, text=True, errors="replace")
    if result.returncode:
        raise RuntimeError("Command failed: " + subprocess.list2cmdline([str(x) for x in command])
                           + "\n" + result.stdout + result.stderr)
    return result.stdout + result.stderr


def toolchain():
    override = os.environ.get("NEXUS_HOST_CXX_DIR")
    gcc = Path(override) / "gcc.exe" if override else shutil.which("gcc")
    if not gcc or not Path(gcc).is_file():
        raise RuntimeError("GCC not found. Put MinGW bin on PATH or set NEXUS_HOST_CXX_DIR.")
    compiler = Path(gcc).resolve().parent
    for name in ("gcc.exe", "g++.exe", "objdump.exe"):
        if not (compiler / name).is_file():
            raise RuntimeError(f"Missing {name} in {compiler}")
    roots = [Path(os.environ["SDL2_ROOT"])] if os.environ.get("SDL2_ROOT") else []
    roots.append(compiler.parent)
    pkg = shutil.which("pkg-config")
    if pkg:
        probe = subprocess.run([pkg, "--variable=prefix", "sdl2"], capture_output=True, text=True)
        if probe.returncode == 0 and probe.stdout.strip():
            roots.append(Path(probe.stdout.strip()))
    for root in roots:
        if ((root / "include/SDL2/SDL.h").is_file()
                and (root / "lib/libSDL2.dll.a").is_file()
                and (root / "bin/SDL2.dll").is_file()):
            return compiler, root.resolve()
    raise RuntimeError("SDL2 headers/import library/DLL not found. Set SDL2_ROOT to its MinGW SDK prefix.")


def copy_runtime(exe, compiler, sdl, env):
    """Copy non-system DLL imports recursively for direct executable launch."""
    pending, seen, copied = [exe], set(), []
    while pending:
        binary = pending.pop()
        imports = re.findall(r"DLL Name:\s*(\S+)", run([compiler / "objdump.exe", "-p", binary], env))
        for name in imports:
            if name.casefold() in seen:
                continue
            seen.add(name.casefold())
            source = next((root / name for root in (sdl / "bin", compiler)
                           if (root / name).is_file()), None)
            if source:
                destination = BUILD / name
                shutil.copy2(source, destination)
                copied.append(name)
                pending.append(destination)
            elif not name.lower().startswith(("api-ms-", "ext-ms-")):
                system = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / name
                if not system.is_file():
                    raise RuntimeError(f"Unresolved runtime DLL: {name}")
    return sorted(copied)


def build():
    compiler, sdl = toolchain()
    if not (LVGL / "lvgl.h").is_file():
        raise RuntimeError("Current LVGL dependency is missing; install the firmware's PlatformIO dependencies first.")
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / "obj").mkdir(exist_ok=True)
    (BUILD / "artifacts").mkdir(exist_ok=True)
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join((str(compiler), str(sdl / "bin"), env.get("PATH", "")))
    flags = ["-O0", "-g", "-DLV_CONF_INCLUDE_SIMPLE", "-I" + str(PROJECT / "include"),
             "-I" + str(PROJECT / "src"), "-I" + str(LVGL), "-I" + str(sdl / "include")]
    sources = sorted((LVGL / "src").rglob("*.c"))
    if not sources:
        raise RuntimeError("No LVGL sources found")

    def compile_c(source):
        target = BUILD / "obj" / ("lvgl_" + "_".join(source.relative_to(LVGL).parts) + ".o")
        output = run([compiler / "gcc.exe", "-std=c99", *flags, "-c", source, "-o", target], env)
        return target, output

    # Always rebuild: never reuse tracked src/ui.o or objects in the dependency tree.
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        compiled = list(pool.map(compile_c, sources))
    print(f"Compiled {len(compiled)} current LVGL C sources", flush=True)
    log = [output for _, output in compiled]
    objects = [target for target, _ in compiled]
    for source, name in ((SIM / "main.cpp", "sim_main.o"), (PROJECT / "src/ui.cpp", "shared_ui.o"),
                         (PROJECT / "src/ui_m4.cpp", "shared_ui_m4.o")):
        target = BUILD / "obj" / name
        log.append(run([compiler / "g++.exe", "-std=c++17", "-Wall", "-Wextra", *flags,
                        "-c", source, "-o", target], env))
        objects.append(target)
    exe = BUILD / "nexus_s1_sim.exe"
    log.append(run([compiler / "g++.exe", *objects, "-L" + str(sdl / "lib"), "-lSDL2", "-o", exe], env))
    dlls = copy_runtime(exe, compiler, sdl, env)
    (BUILD / "build.log").write_text("".join(log), encoding="utf-8")
    (BUILD / "build_info.json").write_text(json.dumps({
        "compiler": str(compiler), "sdl2": str(sdl), "shared_ui": str(PROJECT / "src/ui.cpp"),
        "lvgl_c_sources": len(sources), "runtime_dlls": dlls,
    }, indent=2), encoding="utf-8")
    for output in log:
        if output.strip():
            print(output, end="")
    print(f"BUILD PASS: {exe}\nGCC: {compiler}\nSDL2: {sdl}\nRuntime DLLs: {', '.join(dlls)}", flush=True)
    return exe


def main():
    global BUILD
    parser = argparse.ArgumentParser(description=__doc__)
    choice = parser.add_mutually_exclusive_group()
    choice.add_argument("--run", action="store_true", help="Build current sources, then open interactive window")
    choice.add_argument("--test", action="store_true", help="Build, then exercise SDL events, real UI and time")
    parser.add_argument("--duration", type=float, help="Close preview after N seconds (optional smoke test)")
    parser.add_argument("--build-name", default="pc_sim", help="Isolated directory under .pio: pc_sim or pc_sim_<name>")
    args = parser.parse_args()
    if not re.fullmatch(r"pc_sim(?:_[A-Za-z0-9_-]+)?", args.build_name):
        parser.error("--build-name must be pc_sim or pc_sim_<name> (letters, digits, underscores or hyphens)")
    BUILD = PROJECT / ".pio" / args.build_name
    if args.duration is not None and (not 0 < args.duration < float("inf")):
        parser.error("--duration must be finite and positive")
    try:
        exe = build()
        if args.run or args.test:
            command = [str(exe), "--artifacts", str(BUILD / "artifacts")]
            if args.test:
                command.append("--self-test")
            if args.duration is not None:
                command += ["--duration", str(args.duration)]
            env = os.environ.copy()
            system = Path(os.environ.get("SystemRoot", r"C:\Windows"))
            env["PATH"] = os.pathsep.join((str(system / "System32"), str(system)))
            return subprocess.run(command, cwd=BUILD, env=env).returncode
    except (RuntimeError, OSError) as error:
        print(f"BUILD/RUN FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
