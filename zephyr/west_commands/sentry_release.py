"""
`west sentry-release`: scripts/zephyr_release.py as a west command.

All of the logic is in that script, which also runs on its own as
`python scripts/zephyr_release.py ...`. West discovers extension commands only in projects
its manifest lists, and an application that pulls this module in through
ZEPHYR_EXTRA_MODULES is invisible to it, so the script cannot depend on running under west.
This wrapper only hands it the command line.

It loads the script by path rather than importing Zephyr's own west command modules, so it
works with whatever Zephyr tree the workspace has.
"""

import importlib.util
import sys
from pathlib import Path

from west.commands import WestCommand

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "zephyr_release.py"


class SentryRelease(WestCommand):
    def __init__(self):
        super().__init__(
            "sentry-release",
            "build, stamp and upload a release so Sentry symbolicates its crashes",
            "Build a Zephyr application with its build-id and image geometry compiled in, "
            "stamp the build-id into an upload copy of the ELF and upload it to Sentry. "
            "Takes the same arguments as scripts/zephyr_release.py; run "
            "`west sentry-release -h` for them.",
            accepts_unknown_args=True,
        )

    def do_add_parser(self, parser_adder):
        # No options of its own and no -h: every argument, -h included, goes to the script,
        # whose help is the complete one.
        return parser_adder.add_parser(self.name, help=self.help,
                                       description=self.description, add_help=False)

    def do_run(self, args, unknown):
        # The raw command line, not argparse's leftovers: those lose the "--" that separates
        # CMake arguments, and with it the difference between a CMake flag and an option.
        try:
            argv = sys.argv[sys.argv.index(self.name) + 1:]
        except ValueError:
            argv = unknown  # Invoked through a west alias; the best that is available.

        spec = importlib.util.spec_from_file_location("sentry_micro_zephyr_release", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        exit_code = module.main(argv)
        if exit_code:
            self.die(f"sentry-release failed (exit {exit_code})", exit_code=exit_code)
