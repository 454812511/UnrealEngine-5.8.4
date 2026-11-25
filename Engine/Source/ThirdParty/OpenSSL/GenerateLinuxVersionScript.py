#!/usr/bin/env python3
# Copyright Epic Games, Inc. All Rights Reserved.
"""
Generate the UE_OpenSSL.ver linker version scripts for the Linux/Unix OpenSSL builds.

WHY THIS EXISTS
---------------
UE whole-archives libcrypto.a/libssl.a into its module .so files. With default visibility
those OpenSSL symbols are exported unversioned, so a host process' own system libcrypto/libssl
can interpose UE's copy (and vice versa) and crash. LinuxToolChain.cs passes the sibling
UE_OpenSSL.ver to the linker, putting every OpenSSL symbol under a private version node: the
symbols stay shared among UE's .so files but no longer bind to an outside libcrypto/libssl.

WHEN TO RUN IT
--------------
Whenever the bundled OpenSSL is updated (new version dir, or refreshed .a archives):

  1. Drop the new archives in:
       Engine/Source/ThirdParty/OpenSSL/<version>/lib/Unix/<arch>/lib{crypto,ssl}.a
  2. Regenerate (run from anywhere; the script locates itself):
       python GenerateLinuxVersionScript.py                 # one version dir present
       python GenerateLinuxVersionScript.py --openssl-version <version>
  3. p4 add / p4 edit the regenerated .../lib/Unix/<arch>/UE_OpenSSL.ver files.

CI / PRESUBMIT
--------------
  python GenerateLinuxVersionScript.py --check
exits non-zero (listing what differs) if any UE_OpenSSL.ver is missing or stale vs its archives.

NOTES
-----
* No external tools (nm/objdump): the symbol list is read from the GNU 'ar' archive symbol
  index, so it runs the same on Windows and Linux.
* The version-node name is internal; nothing references it by name. The "UE_OPENSSL_" prefix
  just keeps it distinct from the system OpenSSL's node.
"""

import argparse
import os
import sys
from pathlib import Path

# Engine/Source/ThirdParty/OpenSSL  -- this script lives directly in here.
OPENSSL_ROOT = Path(__file__).resolve().parent
VER_FILENAME = "UE_OpenSSL.ver"
ARCHIVES = ("libcrypto.a", "libssl.a")
AR_MAGIC = b"!<arch>\n"


def fail(msg):
    """Print a clear error and abort. Every failure path routes through here."""
    sys.stderr.write(f"ERROR: {msg}\n")
    sys.exit(1)


def info(msg):
    sys.stdout.write(msg + "\n")


# --------------------------------------------------------------------------------------
# GNU 'ar' archive symbol-index reader (the externally-visible defined symbols of an .a).
# --------------------------------------------------------------------------------------
def read_archive_exported_symbols(archive_path):
    try:
        data = archive_path.read_bytes()
    except OSError as e:
        fail(f"cannot read archive '{archive_path}': {e}")

    if data[:8] != AR_MAGIC:
        fail(f"'{archive_path}' is not a GNU ar archive "
             f"(magic was {data[:8]!r}, expected {AR_MAGIC!r}).")

    if len(data) < 8 + 60:
        fail(f"'{archive_path}' is truncated: no archive members found.")

    # First member is the symbol index: '/' (32-bit offsets) or '/SYM64/' (64-bit offsets).
    header = data[8:8 + 60]
    if header[58:60] != b"`\n":
        fail(f"'{archive_path}' has a malformed first member header "
             f"(bad terminator {header[58:60]!r}).")
    name = header[0:16].decode("ascii", "replace").rstrip()
    try:
        size = int(header[48:58].decode("ascii").strip())
    except ValueError:
        fail(f"'{archive_path}' has a malformed member size field "
             f"({header[48:58]!r}).")

    body = data[8 + 60: 8 + 60 + size]
    if len(body) < size:
        fail(f"'{archive_path}' is truncated: symbol index claims {size} bytes "
             f"but only {len(body)} are present.")

    if name == "/":
        wordsize = 4
    elif name == "/SYM64/":
        wordsize = 8
    else:
        fail(f"'{archive_path}' has no symbol index (first member is '{name}'). "
             f"Run 'ranlib' / 'llvm-ranlib' on the archive and try again.")

    if len(body) < wordsize:
        fail(f"'{archive_path}' symbol index is too small to hold a symbol count.")
    count = int.from_bytes(body[:wordsize], "big")
    names_off = wordsize + count * wordsize
    if names_off > len(body):
        fail(f"'{archive_path}' symbol index is inconsistent "
             f"(count={count} overruns the {len(body)}-byte index).")

    raw_names = body[names_off:].split(b"\x00")
    symbols = [n.decode("ascii", "replace") for n in raw_names if n]
    if len(symbols) < count:
        # Not fatal, but worth surfacing -- something truncated the name blob.
        sys.stderr.write(f"WARNING: '{archive_path}' index lists {count} symbols but only "
                         f"{len(symbols)} names were parsed.\n")
    if not symbols:
        fail(f"'{archive_path}' exported zero symbols -- the archive looks empty or stripped.")
    return symbols


# --------------------------------------------------------------------------------------
# Version-script content.
# --------------------------------------------------------------------------------------
def version_node_name(openssl_version):
    sanitized = "".join(c if (c.isalnum()) else "_" for c in openssl_version)
    return f"UE_OPENSSL_{sanitized}"


def build_version_script(openssl_version, symbols):
    node = version_node_name(openssl_version)
    lines = [
        "# UE's bundled OpenSSL, version-namespaced so a host process' system libcrypto/libssl",
        "# can't interpose it (and vice versa). Generated by GenerateLinuxVersionScript.py -- do not edit.",
        f"{node} {{",
        f"    global:",
    ]
    lines += [f"        {s};" for s in sorted(set(symbols))]
    lines.append("};")
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------------------
# Locating the OpenSSL version directory and its per-arch lib folders.
# --------------------------------------------------------------------------------------
def resolve_version_dir(requested):
    if requested:
        d = OPENSSL_ROOT / requested
        if not d.is_dir():
            fail(f"OpenSSL version directory not found: '{d}'. "
                 f"Available: {', '.join(_available_versions()) or '(none)'}.")
        if not (d / "lib" / "Unix").is_dir():
            fail(f"'{d}' has no lib/Unix folder -- is this a Unix-capable OpenSSL drop?")
        return d

    candidates = _available_versions()
    if not candidates:
        fail(f"No OpenSSL version directory with a lib/Unix folder under '{OPENSSL_ROOT}'. "
             f"Pass --openssl-version explicitly.")
    if len(candidates) > 1:
        fail(f"Multiple OpenSSL version dirs found ({', '.join(candidates)}). "
             f"Pass --openssl-version to choose one.")
    return OPENSSL_ROOT / candidates[0]


def _available_versions():
    if not OPENSSL_ROOT.is_dir():
        return []
    return sorted(p.name for p in OPENSSL_ROOT.iterdir()
                  if p.is_dir() and (p / "lib" / "Unix").is_dir())


def find_arch_dirs(version_dir):
    unix = version_dir / "lib" / "Unix"
    arch_dirs = sorted(p for p in unix.iterdir()
                       if p.is_dir() and (p / "libcrypto.a").is_file())
    if not arch_dirs:
        fail(f"No arch folders containing libcrypto.a under '{unix}'.")
    return arch_dirs


# --------------------------------------------------------------------------------------
def gather_symbols(arch_dir):
    symbols = set()
    for arch_name in ARCHIVES:
        archive = arch_dir / arch_name
        if not archive.is_file():
            fail(f"expected archive missing: '{archive}'. Both {ARCHIVES} must be present.")
        symbols.update(read_archive_exported_symbols(archive))
    return symbols


def main():
    parser = argparse.ArgumentParser(
        description="(Re)generate UE_OpenSSL.ver linker version scripts for Linux OpenSSL.",
        epilog="See the module docstring at the top of this file for full instructions.")
    parser.add_argument("--openssl-version", metavar="VER",
                        help="OpenSSL version dir name (e.g. 1.1.1t). "
                             "Auto-detected if exactly one exists.")
    parser.add_argument("--check", action="store_true",
                        help="Do not write; exit non-zero if any UE_OpenSSL.ver is missing "
                             "or stale vs its archives (for CI).")
    args = parser.parse_args()

    version_dir = resolve_version_dir(args.openssl_version)
    openssl_version = version_dir.name
    info(f"OpenSSL version : {openssl_version}")
    info(f"Version node    : {version_node_name(openssl_version)}")

    arch_dirs = find_arch_dirs(version_dir)
    stale = []
    for arch_dir in arch_dirs:
        symbols = gather_symbols(arch_dir)
        content = build_version_script(openssl_version, symbols)
        ver_path = arch_dir / VER_FILENAME

        if args.check:
            existing = ver_path.read_text() if ver_path.is_file() else None
            if existing != content:
                why = "missing" if existing is None else "stale"
                stale.append(ver_path)
                sys.stderr.write(f"  [{why}] {ver_path}  ({len(symbols)} symbols expected)\n")
            else:
                info(f"  [ok]    {ver_path}  ({len(symbols)} symbols)")
            continue

        try:
            ver_path.write_text(content)
        except OSError as e:
            fail(f"cannot write '{ver_path}': {e} "
                 f"(is it checked out / writable? run 'p4 edit' or 'p4 add').")
        info(f"  wrote {ver_path}  ({len(symbols)} symbols)")

    if args.check and stale:
        fail(f"{len(stale)} version script(s) out of date. "
             f"Re-run: python {Path(__file__).name} --openssl-version {openssl_version}")

    info("OK (check passed)." if args.check else "Done.")


if __name__ == "__main__":
    main()
