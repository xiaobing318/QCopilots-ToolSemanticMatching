#!/usr/bin/env python3
"""Pass CMake's UTF-8 response files to MSVC LINK/LIB without losing paths."""
import codecs
import contextlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile


@contextlib.contextmanager
def response_arguments(arguments):
    """Preserve argv/cwd and own only unique copies of unmarked UTF-8 files."""
    rewritten = list(arguments)
    temporary_files = []
    try:
        for index, argument in enumerate(rewritten[1:], start=1):
            if not argument.startswith("@"):
                continue
            source = Path(argument[1:])
            content = source.read_bytes()
            if content.startswith((codecs.BOM_UTF8, codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE)):
                continue
            # Ninja emits UTF-8 without a BOM. Do not tokenize its argument
            # text, guess an ANSI code page or silently replace invalid bytes.
            decoded = content.decode("utf-8")
            descriptor, filename = tempfile.mkstemp(
                prefix="qctsm-msvc-", suffix=".rsp", dir=source.parent)
            temporary = Path(filename)
            temporary_files.append(temporary)
            with os.fdopen(descriptor, "wb") as output:
                output.write(codecs.BOM_UTF16_LE + decoded.encode("utf-16-le"))
            replacement = temporary if source.is_absolute() else Path(os.path.relpath(temporary))
            rewritten[index] = "@" + str(replacement)
        yield rewritten
    finally:
        cleanup_errors = []
        for temporary in temporary_files:
            try:
                temporary.unlink(missing_ok=True)
            except OSError as error:
                cleanup_errors.append(error)
        if cleanup_errors:
            raise cleanup_errors[0]


def main(arguments=None):
    arguments = sys.argv[1:] if arguments is None else arguments
    if not arguments:
        raise ValueError("An MSVC linker or librarian command is required")
    with response_arguments(arguments) as command:
        # Keep inherited output streams and environment. The VS manifest
        # helper remains outside this launcher and receives the real exit code.
        return subprocess.call(command, shell=False)


if __name__ == "__main__":
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    try:
        sys.exit(main())
    except (OSError, UnicodeError, ValueError) as error:
        print(f"qctsm MSVC response file: {error}", file=sys.stderr)
        sys.exit(1)
