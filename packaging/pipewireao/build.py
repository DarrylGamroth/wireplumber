#!/usr/bin/env python3
"""Build upstream WirePlumber against an isolated PipeWireAO SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

RECIPE = Path(__file__).resolve().parent
REPOSITORY = RECIPE.parent.parent
OPTIONS = ["--libdir=lib", "-Dsystemd=disabled", "-Delogind=disabled",
           "-Dsystemd-user-service=false", "-Dsystemd-system-service=false",
           "-Ddoc=disabled", "-Dintrospection=disabled", "-Dtests=true",
           "-Ddbus-tests=true"]


def run(command, env, cwd=None):
    result = subprocess.run(command, env=env, cwd=cwd, capture_output=True)
    if result.returncode:
        # Every caller uses the restricted environment constructed below.
        detail = result.stderr.decode(errors="replace")[-4000:]
        raise RuntimeError(f"{command[0]} {command[1]} failed ({result.returncode})\n{detail}")
    return result.stdout.decode().strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_digest(path):
    result = hashlib.sha256()
    for item in sorted(path.rglob("*")):
        if item.is_file():
            result.update(str(item.relative_to(path)).encode() + b"\0")
            result.update(bytes.fromhex(digest(item)))
    return result.hexdigest()


def environment(sdk, build=None):
    # Do not forward tokens, session endpoints, or caller build overrides.
    env = {"PATH": os.defpath, "HOME": str(Path.home()), "LANG": "C.UTF-8"}
    pcdirs = sorted(sdk.glob("lib*/pkgconfig"))
    pcdirs += sorted(sdk.glob("lib/*/pkgconfig"))
    env["PKG_CONFIG_PATH"] = os.pathsep.join(map(str, pcdirs))
    def variable(package, name):
        return run(["pkg-config", f"--variable={name}", package], env)
    libdir = variable("libpipewire-ao-0.3", "libdir")
    if not Path(os.path.abspath(libdir)).is_relative_to(sdk) or not Path(libdir).is_dir():
        raise RuntimeError("PipeWireAO libdir must identify a directory in the SDK")
    paths = [libdir]
    if build:
        paths.insert(0, str(build / "lib/wp"))
    env["LD_LIBRARY_PATH"] = os.pathsep.join(paths)
    env["PIPEWIREAO_MODULE_DIR"] = variable("libpipewire-ao-0.3", "moduledir")
    env["PIPEWIREAO_SPA_PLUGIN_DIR"] = variable("libspa-ao-0.2", "plugindir")
    env["PIPEWIREAO_CONFIG_DIR"] = (variable("libpipewire-ao-0.3", "confdatadir")
                                    or str(sdk / "share/pipewire-ao"))
    for name in ("PIPEWIREAO_MODULE_DIR", "PIPEWIREAO_SPA_PLUGIN_DIR",
                 "PIPEWIREAO_CONFIG_DIR"):
        # SDK assets may deliberately be symlinks into an artifact store.
        path = Path(os.path.abspath(env[name]))
        if not path.is_relative_to(sdk) or not path.is_dir():
            raise RuntimeError(f"{name} must identify a directory in the SDK")
    return env


def save(work, manifest):
    (work / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    build_parser = sub.add_parser("build")
    build_parser.add_argument("--sdk", required=True, type=Path)
    build_parser.add_argument("--prefix", required=True, type=Path)
    build_parser.add_argument("--lua-source", type=Path,
                              help="prepared Lua 5.5.0 source including Meson files")
    build_parser.add_argument("--dynamic-pod-filter", action="store_true")
    build_parser.add_argument("--event-properties", action="store_true")
    for command in (build_parser, sub.add_parser("test"), sub.add_parser("install")):
        command.add_argument("--workdir", required=True, type=Path)
        if command.prog.endswith(("build", "test")):
            command.add_argument("--jobs", type=int, default=2, choices=range(1, 17))
    args = parser.parse_args()
    work = args.workdir.resolve()
    build = work / "build"
    if args.action == "build":
        if not args.prefix.is_absolute():
            parser.error("--prefix must be an absolute isolated path")
        prefix, sdk = args.prefix.resolve(), args.sdk.resolve()
        lua = args.lua_source.resolve() if args.lua_source else None
        if work.exists() or prefix.exists():
            parser.error("workdir and prefix must both be fresh, nonexistent paths")
        if any(a.is_relative_to(b) or b.is_relative_to(a)
               for a, b in ((work, prefix), (prefix, sdk), (work, sdk))):
            parser.error("workdir, prefix, and SDK must not overlap")
        if lua and (not (lua / "meson.build").is_file() or not (lua / "src/lua.h").is_file()):
            parser.error("--lua-source must contain prepared Lua sources and Meson files")
        env = environment(sdk)
        revision = (RECIPE / "upstream-revision").read_text().strip()
        patches = ["dependencies.patch"]
        if args.dynamic_pod_filter:
            patches.append("dynamic-pod-filter.patch")
        if args.event_properties:
            patches.append("event-properties.patch")
        manifest = {"upstream_url": "https://gitlab.freedesktop.org/pipewire/wireplumber.git",
                    "upstream_revision": revision, "sdk": str(sdk), "prefix": str(prefix),
                    "meson_options": [f"--prefix={prefix}"] + OPTIONS +
                                     (["--wrap-mode=nodownload"] if lua else []),
                    "jobs": args.jobs, "lua_source": str(lua) if lua else None,
                    "lua_source_sha256": tree_digest(lua) if lua else None,
                    "patches": [{"name": p, "sha256": digest(RECIPE / p)} for p in patches],
                    "tools": {tool: run([tool, "--version"], env)
                              for tool in ("git", "meson", "ninja", "pkg-config", "cc")}}
        work.mkdir(parents=True)
        source = work / "source"
        source.mkdir()
        archive = work / "upstream.tar"
        run(["git", "archive", "--format=tar", f"--output={archive}", revision],
            env, REPOSITORY)
        manifest["upstream_archive_sha256"] = digest(archive)
        run(["tar", "-xf", str(archive), "-C", str(source)], env)
        archive.unlink()
        for patch in patches:
            run(["git", "apply", "--check", str(RECIPE / patch)], env, source)
            run(["git", "apply", str(RECIPE / patch)], env, source)
        if lua:
            shutil.copytree(lua, source / "subprojects/lua-5.5.0")
        save(work, manifest)
        run(["meson", "setup", str(build), str(source)] + manifest["meson_options"], env)
        print("Configured pinned upstream source; compiling", flush=True)
        run(["meson", "compile", "-C", str(build), "-j", str(args.jobs)], env)
        manifest["build"] = "passed"
    else:
        manifest = json.loads((work / "manifest.json").read_text())
        env = environment(Path(manifest["sdk"]), build)
        if args.action == "test":
            # Upstream sets PIPEWIRE_* itself. AO uses these separate names.
            runtime = work / "test-runtime"
            runtime.mkdir(mode=0o700, exist_ok=True)
            env["PIPEWIREAO_RUNTIME_DIR"] = str(runtime)
            command = ["meson", "test", "-C", str(build), "--no-rebuild",
                       "--num-processes", str(args.jobs)]
            result = subprocess.run(command, env=env, capture_output=True)
            results = [json.loads(line) for line in
                       (build / "meson-logs/testlog.json").read_text().splitlines()]
            manifest["tests"] = [{"name": item["name"], "result": item["result"]}
                                 for item in results]
            save(work, manifest)
            for item in manifest["tests"]:
                print(item["result"], item["name"])
            if result.returncode:
                raise RuntimeError("tests failed; inspect local Meson logs")
        else:
            prefix = Path(manifest["prefix"])
            if prefix.exists():
                raise RuntimeError("installation requires the recorded fresh prefix; refusing replacement")
            run(["meson", "install", "-C", str(build), "--no-rebuild"], env)
            manifest["install"] = "passed"
    save(work, manifest)
    print(f"{args.action} passed; manifest: {work / 'manifest.json'}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError) as error:
        sys.exit(str(error))
