#!/usr/bin/env python3
"""
Build a Zephyr application and make its backtraces readable in Sentry, in one command.

    .venv/bin/python scripts/zephyr_release.py -b esp32s3_devkitc/esp32s3/procpu \\
        -d build-wifi examples/zephyr_basic -- -DEXTRA_CONF_FILE=wifi.conf
    west sentry-release -b esp32s3_devkitc/esp32s3/procpu -d build-wifi examples/zephyr_basic

The Zephyr counterpart of scripts/release.sh, with the same steps, which have to agree with
each other and are easy to get wrong by hand:

  1. Choose a build-id, deterministically, from the release name and the board.
  2. Build with that id, the image's load address and its size compiled in, rebuilding until
     the measured address and size stop moving.
  3. Copy the ELF to zephyr.sentry.elf, sort its PT_LOAD headers (elf_sort_loads.py says why
     Zephyr's ESP32 ELF needs that) and stamp the build-id into the copy as a non-ALLOC note.
  4. Read the copy back with sentry-cli to prove Sentry will index it under that id.
  5. Upload the copy and register the release.

The device runs the zephyr.bin the last build pass produced, which carries the same id and
geometry; only the upload copy is touched afterwards. `west flash -d <build-dir>` after this
is correct: it rebuilds before flashing, and with the same sources and environment that
rebuild produces the same image.

Everything after a `--` is passed to CMake, exactly as `west build` passes it.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from urllib.parse import urlparse

SCRIPTS = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))

import elf_sort_loads  # noqa: E402  (found through the sys.path entry above)

# Environment variables the build reads at configure time, here and in zephyr/CMakeLists.txt.
# The image geometry is left out of the first pass on purpose: there is nothing measured yet,
# and a stale value from an earlier shell would make that pass describe some other build.
GEOMETRY = ("SENTRY_MICRO_IMAGE_ADDR", "SENTRY_MICRO_IMAGE_SIZE")


class ReleaseError(Exception):
    """A step failed; the message says what to do about it."""


def say(line=""):
    print(line, flush=True)


def tool_next_to_python(name):
    """A tool installed in the same virtualenv as the running Python, else from PATH."""
    candidate = Path(sys.executable).parent / name
    if candidate.is_file() and os.access(candidate, os.X_OK):
        return str(candidate)
    return shutil.which(name)


def git_describe(path):
    try:
        out = subprocess.run(["git", "-C", str(path), "describe", "--always", "--dirty"],
                             capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return out.stdout.strip() or None


def is_shallow(path):
    try:
        out = subprocess.run(["git", "-C", str(path), "rev-parse", "--is-shallow-repository"],
                             capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return False
    return out.stdout.strip() == "true"


def read_cmake_cache(build_dir, key):
    """A value from <build-dir>/CMakeCache.txt, or None."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return None
    pattern = re.compile(rf"^{re.escape(key)}:[A-Z]+=(.*)$")
    for line in cache.read_text(errors="replace").splitlines():
        match = pattern.match(line)
        if match:
            return match.group(1)
    return None


def kernel_name(build_dir):
    """CONFIG_KERNEL_BIN_NAME, which names zephyr.elf and zephyr.bin; "zephyr" by default."""
    config = build_dir / "zephyr" / ".config"
    if config.is_file():
        for line in config.read_text(errors="replace").splitlines():
            if line.startswith("CONFIG_KERNEL_BIN_NAME="):
                return line.split("=", 1)[1].strip().strip('"')
    return "zephyr"


def measure(elf):
    """(image_addr, image_size) as the "0x..." strings elf_info.py --export prints.

    Taken from its --export output rather than recomputed here, so the strings compiled into
    the firmware are formatted by the one script that defines them, for both build systems.
    """
    out = subprocess.run([sys.executable, str(SCRIPTS / "elf_info.py"), str(elf), "--export"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise ReleaseError(f"could not measure {elf}:\n{out.stderr.strip()}")
    values = dict(re.findall(r'^export (\w+)="([^"]*)"$', out.stdout, re.MULTILINE))
    try:
        return values["SENTRY_MICRO_IMAGE_ADDR"], values["SENTRY_MICRO_IMAGE_SIZE"]
    except KeyError:
        raise ReleaseError(f"unexpected output from elf_info.py:\n{out.stdout}") from None


def west_build(args, env, number, purpose):
    """One pristine build pass. Quiet unless it fails, like release.sh's `pio run`."""
    say(f"→ building (pass {number}: {purpose})")
    # -p always on every pass, not just the first. The sentry-micro module and the
    # application read these values from the environment when CMake configures, and
    # an incremental build does not reconfigure because an environment variable
    # changed, so it would quietly compile in the previous pass's numbers. A pristine
    # build costs a few minutes per pass; a stale one costs every frame.
    command = [args.west, "build", "-p", "always", "-b", args.board,
               "-d", str(args.build_dir), str(args.source_dir)]
    if args.cmake_args:
        command += ["--", *args.cmake_args]
    result = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    # Written after the pass rather than during it: -p always empties the build directory
    # first, and this is the one place the log is both kept and out of git's way.
    log = args.build_dir / "sentry-release-build.log"
    if args.build_dir.is_dir():
        log.write_text(result.stdout)
    if result.returncode != 0:
        tail = "\n".join(result.stdout.splitlines()[-40:])
        raise ReleaseError(f"west build failed on pass {number} (exit {result.returncode}).\n"
                           f"{tail}\n       full log: {log}")


def parse_args(argv):
    # Split on the first "--" ourselves: argparse would swallow it, and everything after it
    # belongs to CMake, not to this script.
    if "--" in argv:
        split = argv.index("--")
        own, cmake_args = argv[:split], argv[split + 1:]
    else:
        own, cmake_args = argv, []

    parser = argparse.ArgumentParser(
        prog="zephyr_release.py",
        description=__doc__.split("\n")[1],
        epilog="Everything after -- is passed to CMake, as with west build.")
    parser.add_argument("source_dir", nargs="?", default=".", type=Path,
                        help="application source directory (default: the current one)")
    parser.add_argument("-b", "--board", required=True,
                        help="board, as west build takes it, e.g. esp32s3_devkitc/esp32s3/procpu")
    parser.add_argument("-d", "--build-dir", default="build", type=Path,
                        help="build directory (default: build)")
    parser.add_argument("-r", "--release",
                        help="release identifier, e.g. my-firmware@1.2.3 (default: "
                             "$SENTRY_MICRO_RELEASE, else <app-dir>@<git describe>)")
    parser.add_argument("--build-id",
                        help="20-byte build-id as hex (default: sha1 of '<release>/<board>')")
    parser.add_argument("--image-name",
                        help="module name shown beside every frame (default: <board>.elf, "
                             "with / as _)")
    parser.add_argument("--zephyr-base", type=Path,
                        help="the zephyr checkout (default: $ZEPHYR_BASE, which west sets)")
    parser.add_argument("--objcopy",
                        help="objcopy for this target (default: CMAKE_OBJCOPY from the "
                             "build's CMakeCache.txt)")
    parser.add_argument("--west", help="west executable (default: next to this Python, "
                                       "else on PATH)")
    parser.add_argument("--sentry-cli", help="sentry-cli executable (default: next to this "
                                             "Python, else on PATH)")
    parser.add_argument("--max-passes", type=int, default=6,
                        help="give up if the image has not settled after this many builds "
                             "(default: 6)")
    parser.add_argument("--no-upload", action="store_true",
                        help="build, stamp and check, but do not talk to Sentry")
    parser.add_argument("--no-wait", action="store_true",
                        help="return as soon as the upload is accepted, without waiting for "
                             "Sentry to finish processing it")
    parser.add_argument("--no-sources", action="store_true",
                        help="upload debug info only, without embedding the source text")
    parser.add_argument("--no-commits", action="store_true",
                        help="do not tell Sentry which commits went into this build")
    parser.add_argument("--json-summary", type=Path,
                        help="write {environment, release, code_id, debug_id, ...} to this file")
    args = parser.parse_args(own)
    args.cmake_args = cmake_args
    if args.max_passes < 2:
        parser.error("--max-passes must be at least 2: one to measure, one to bake it in")
    return args


def run(args):
    # Kept as typed for the flash hint at the end, which is run from the same directory.
    build_dir_as_given = args.build_dir
    args.source_dir = args.source_dir.resolve()
    args.build_dir = args.build_dir.resolve()
    if not (args.source_dir / "CMakeLists.txt").is_file():
        raise ReleaseError(f"no CMakeLists.txt in {args.source_dir}")

    zephyr_base = args.zephyr_base or os.environ.get("ZEPHYR_BASE")
    if not zephyr_base:
        raise ReleaseError("ZEPHYR_BASE is not set. Export it, pass --zephyr-base, or run "
                           "this as `west sentry-release`, which sets it.")
    args.west = args.west or tool_next_to_python("west")
    if not args.west:
        raise ReleaseError("west not found; install it next to this Python or pass --west")

    upload = not args.no_upload
    args.sentry_cli = args.sentry_cli or tool_next_to_python("sentry-cli")

    # Checked before building, as release.sh does: a doomed run should cost seconds, not
    # several pristine Zephyr builds, and it should not leave a half-populated release.
    project = org = None
    if upload:
        if not args.sentry_cli:
            raise ReleaseError("sentry-cli not found; install it, or pass --no-upload.\n"
                               "       brew install getsentry/tools/sentry-cli")
        if not os.environ.get("SENTRY_AUTH_TOKEN") and not (Path.home() / ".sentryclirc").is_file():
            raise ReleaseError("no Sentry credentials.\n"
                               "       export SENTRY_AUTH_TOKEN=... (Settings -> Auth Tokens),\n"
                               "       or run 'sentry-cli login', or pass --no-upload.")
        # The project comes from the DSN, which already carries it. The org deliberately does
        # not: an organization auth token names its own org and sentry-cli warns about, then
        # ignores, any other. It is passed only when the user set SENTRY_ORG, which is what a
        # personal token needs. Same rule as release.sh.
        project = os.environ.get("SENTRY_PROJECT")
        dsn = os.environ.get("SENTRY_MICRO_DSN")
        if not project and dsn:
            project = (urlparse(dsn).path or "").strip("/").split("/")[-1] or None
        if not project:
            raise ReleaseError("could not determine the project.\n"
                               "       set SENTRY_MICRO_DSN, or export SENTRY_PROJECT.")
        org = os.environ.get("SENTRY_ORG") or None
        # A shallow clone yields exactly one commit, which reads as "this build contains one
        # change" when it contains a hundred, and renders identically to the truth.
        if not args.no_commits and is_shallow(args.source_dir):
            raise ReleaseError(
                f"{args.source_dir} is a shallow clone, so only one commit would be\n"
                "       associated with this release, which would be wrong.\n"
                "       Run 'git fetch --unshallow', or pass --no-commits to ship without them.")

    release = args.release or os.environ.get("SENTRY_MICRO_RELEASE")
    if not release:
        release = f"{args.source_dir.name}@{git_describe(args.source_dir) or 'dev'}"

    # Derived from the release and the board, so the same source at the same release always
    # gets the same id and two boards built from it never share one. Sharing one would have
    # Sentry resolve one board's addresses against the other's ELF: confidently wrong frames.
    build_id = args.build_id or hashlib.sha1(f"{release}/{args.board}".encode()).hexdigest()
    if not re.fullmatch(r"[0-9a-fA-F]{40}", build_id):
        raise ReleaseError(f"--build-id must be 40 hex characters, got {build_id!r}")
    build_id = build_id.lower()
    image_name = args.image_name or args.board.replace("/", "_") + ".elf"

    say("── sentry-micro release (Zephyr) ─────────────────")
    say(f"  board     {args.board}")
    say(f"  release   {release}")
    say(f"  build-id  {build_id}")
    say(f"  build dir {args.build_dir}")
    say()

    # The values the firmware reports, compiled in by zephyr/CMakeLists.txt (build-id, image
    # geometry and name) and by the application (release), so the device claims exactly the
    # id that gets stamped and exactly the release that gets registered.
    env = dict(os.environ)
    env["ZEPHYR_BASE"] = str(Path(zephyr_base).resolve())
    env["SENTRY_MICRO_BUILD_ID"] = build_id
    env["SENTRY_MICRO_RELEASE"] = release
    env["SENTRY_MICRO_IMAGE_NAME"] = image_name
    for name in GEOMETRY:
        env.pop(name, None)

    # Build until the measurements stop moving, as release.sh does and for the same reason:
    # compiling the address and size in changes the size (a small constant is a two-byte
    # movi.n on Xtensa, a large one a four-byte literal-pool entry), so the numbers are only
    # right once a pass produces an ELF whose measurements match the ones it was built with.
    # Failing to settle means something else in the build is not deterministic.
    west_build(args, env, 1, "measure the image")
    elf = args.build_dir / "zephyr" / f"{kernel_name(args.build_dir)}.elf"
    if not elf.is_file():
        raise ReleaseError(f"no ELF at {elf}")
    addr, size = measure(elf)
    say(f"  image_addr {addr}")
    say(f"  image_size {size}")

    passes, settled = 1, False
    while passes < args.max_passes:
        passes += 1
        env["SENTRY_MICRO_IMAGE_ADDR"], env["SENTRY_MICRO_IMAGE_SIZE"] = addr, size
        say()
        west_build(args, env, passes, "bake it in")
        measured = measure(elf)
        if measured == (addr, size):
            say(f"  settled: {addr} +{size}")
            settled = True
            break
        say(f"  moved:   {addr} +{size} -> {measured[0]} +{measured[1]}")
        addr, size = measured

    if not settled:
        raise ReleaseError(
            f"the image never stopped moving after {args.max_passes} passes.\n"
            f"       last: compiled in {env['SENTRY_MICRO_IMAGE_ADDR']} "
            f"+{env['SENTRY_MICRO_IMAGE_SIZE']}, measured {addr} +{size}\n"
            "       Shipping this would upload debug files that do not describe the binary.\n"
            "       The build is not reproducible; investigate before releasing.")

    # The geometry settles just as well when nothing was compiled in at all, for instance with
    # a copy of the module whose CMake predates these variables. The build-id string in the
    # image is the proof that the firmware really carries what is about to be uploaded.
    image = elf.with_suffix(".bin")
    if not image.is_file():
        image = elf
    if build_id.encode() not in image.read_bytes():
        raise ReleaseError(
            f"{image.name} does not contain the build-id {build_id}.\n"
            "       The firmware would not report it, so nothing could match the upload.\n"
            "       Is CONFIG_SENTRY_MICRO=y, and is this the sentry-micro module whose\n"
            "       zephyr/CMakeLists.txt reads SENTRY_MICRO_BUILD_ID?")

    objcopy = args.objcopy or read_cmake_cache(args.build_dir, "CMAKE_OBJCOPY")
    if not objcopy:
        raise ReleaseError("could not find objcopy in the build's CMakeCache.txt; pass --objcopy")

    say()
    say("→ making the upload copy: sorting PT_LOAD, stamping the build-id")
    # A copy, because `west flash` rebuilds before it flashes and would overwrite anything
    # done to zephyr.elf itself; the copy is the upload artefact and nothing rebuilds it.
    debug_elf = elf.with_name(f"{elf.stem}.sentry.elf")
    shutil.copyfile(elf, debug_elf)
    if elf_sort_loads.main([str(debug_elf), "--in-place"]) != 0:
        raise ReleaseError(f"could not sort the program headers of {debug_elf}")
    stamp = subprocess.run([sys.executable, str(SCRIPTS / "stamp_build_id.py"), str(debug_elf),
                            "--build-id", build_id, "--objcopy", objcopy, "--print-debug-id"],
                           capture_output=True, text=True)
    if stamp.returncode != 0:
        raise ReleaseError(f"stamping failed:\n{stamp.stdout}{stamp.stderr}".rstrip())
    debug_id = stamp.stdout.strip()
    say(f"  code_id  {build_id}")
    say(f"  debug_id {debug_id}")

    # Symbolic rebases everything against the first PT_LOAD, and the firmware reports the
    # lowest. They must be the same number in the file that is uploaded, after objcopy has
    # rewritten it, or every frame is offset and resolves to the wrong function, silently.
    first = elf_sort_loads.first_load_vaddr(debug_elf.read_bytes())
    if first is None or first != int(addr, 16) or measure(debug_elf) != (addr, size):
        shown = "nowhere" if first is None else f"0x{first:x}"
        raise ReleaseError(f"{debug_elf.name} loads at {shown} as Sentry reads it, but the "
                           f"firmware reports {addr} +{size}.")
    say(f"  load address {addr}, as Sentry reads it and as the firmware reports it")

    say()
    say("→ verifying the debug file")
    # objcopy exits 0 whether or not the note ended up where Sentry looks for it, so read it
    # back with the same library Sentry uses. Only optional when not uploading.
    check = [sys.executable, str(SCRIPTS / "check_debug_file.py"), str(debug_elf),
             "--expect-debug-id", debug_id]
    check += ["--sentry-cli", args.sentry_cli or "sentry-cli"]
    if not upload:
        check.append("--optional")
    if subprocess.run(check).returncode != 0:
        raise ReleaseError("the upload copy is not one Sentry can use; see above")

    if upload:
        org_args = ["--org", org] if org else []
        say(f"  project   {project}" + (f" (org {org})" if org else ""))
        say()
        say("→ uploading debug files to Sentry")
        # --id + --require-all turn "nothing matched, uploaded 0 files", which sentry-cli
        # otherwise reports as success, into an error; --wait makes the server's verdict
        # visible instead of returning as soon as the bytes are accepted.
        upload_cmd = [args.sentry_cli, "debug-files", "upload"]
        if not args.no_wait:
            upload_cmd.append("--wait")
        if not args.no_sources:
            upload_cmd.append("--include-sources")
        upload_cmd += ["--id", debug_id, "--require-all", *org_args,
                       "--project", project, str(debug_elf)]
        if subprocess.run(upload_cmd).returncode != 0:
            raise ReleaseError("the debug file upload failed; see above")

        say()
        say("→ registering the release")
        # None of this is fatal: the debug files are what symbolication needs, and a release
        # that failed to register still produces readable backtraces.
        cli = [args.sentry_cli, "releases"]
        quiet = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
        if subprocess.run([*cli, "new", *org_args, "--project", project, release],
                          **quiet).returncode != 0:
            say("  (could not register the release; debug files are uploaded)")
        else:
            if not args.no_commits:
                # From the application's own repository, so the commits are the adopter's.
                # --auto prefers a repository configured in Sentry's integrations and falls
                # back to the local tree; --ignore-missing tolerates a previous release whose
                # commit is not in this clone.
                commits = subprocess.run([*cli, "set-commits", "--auto", "--ignore-missing",
                                          *org_args, "--project", project, release],
                                         cwd=args.source_dir)
                if commits.returncode != 0:
                    say(f"  (no commits associated; is {args.source_dir} inside a git "
                        "repository?)")
            if subprocess.run([*cli, "finalize", *org_args, "--project", project, release],
                              **quiet).returncode != 0:
                say("  (could not finalize the release)")

    # Same keys as release.sh's, so summarize_release.py reads either; `environment` is the
    # board here, and `board` and `bin` are extra.
    if args.json_summary:
        summary = {
            "environment": args.board, "release": release, "code_id": build_id,
            "debug_id": debug_id, "image_addr": addr, "image_size": size,
            "elf": str(debug_elf), "uploaded": upload,
            "board": args.board, "bin": str(elf.with_suffix(".bin")),
        }
        args.json_summary.write_text(json.dumps(summary, indent=2) + "\n")

    say()
    say(f"done after {passes} build passes. Events from this build carry:")
    say(f"  release    {release}")
    say(f"  code_id    {build_id}")
    say(f"  debug_id   {debug_id}")
    say(f"  image_addr {addr}")
    say(f"  image_size {size}")
    say()
    say(f"  upload copy  {debug_elf}" + ("" if upload else "  (not uploaded: --no-upload)"))
    say(f"  device image {elf.with_suffix('.bin')}")
    say("  flash it with the same environment and arguments, which rebuild it unchanged:")
    say(f"    west flash -d {build_dir_as_given}")

    # The code mapping and stack trace rules Sentry cannot infer for a native project, read
    # from this ELF's build paths. Advice, not part of the release, so never fatal.
    if upload:
        say()
        readelf = re.sub(r"objcopy$", "readelf", objcopy)
        config = [sys.executable, str(SCRIPTS / "sentry_config.py"), str(debug_elf),
                  "--project-dir", str(args.source_dir)]
        if readelf != objcopy:
            config += ["--readelf", readelf]
        if subprocess.run(config).returncode != 0:
            say("  (could not derive the Sentry settings from this ELF)")


def main(argv=None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else list(argv))
    try:
        run(args)
    except ReleaseError as error:
        print(f"\nerror: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
