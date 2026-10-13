#!/usr/bin/env python3
r"""
prepare_atom_roms.py

Prepare and identify ROM files for ACORN ATOM+ V191 (including BBC BASIC).

The important point is that filenames are NOT trusted. Every ROM is identified
by the historical Acorn Atom ROM checksum, so users can download ROM packs
with arbitrary filenames and this script will tell them which files are useful.

Usage:
    python prepare_atom_roms.py
    python prepare_atom_roms.py "C:\Users\name\Downloads\AtomROMs"

What the script does
--------------------
1. Scan every file in the selected directory.
2. If a file is 16 KiB, treat it as a C+D+E+F image and split it into four
   4 KiB files named C_<original>, D_<original>, E_<original>, F_<original>.
   Existing split files are overwritten.
3. Calculate the historical Acorn Atom ROM checksum for every 4 KiB ROM.
4. Compare every signature with the stored ROM database below.
5. If it is one of the required Atom ROMs, copy it to data_ready/ using the
   canonical V191 filename.
6. If it is a known source ROM that can be converted:
       original Atom Kernel E386 -> patched V139 Kernel F0B9
       ASBK-I v1.00 DE24          -> ASBK-I v1.03 988C
   create an _OK file, verify its new signature, and copy it to data_ready/.
7. Print ALL recognised ROMs, even if they are not required by V191.
8. Print all unknown 4 KiB ROMs with their signatures so the database can be
   extended later.
9. Print the final V191 required-file list. Missing files begin with "X -".
10. Write ROM_REPORT.txt.

No third-party Python modules are required.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import shutil
import zipfile
import re
from pathlib import Path

BBC_BASIC_SHA256 = "a7310823149731872e326dbe6a0d56769e0435390465bc872b2e4e9aa0c05c73"
BBC_BASIC_NAME = "ATBASIC3"
ROM_SIZE = 4096
CDEF_SIZE = 16384
ATOM_PC_16_SIZE = 4112   # 16-byte header + 4096-byte ROM
ATOM_PC_22_SIZE = 4118   # 22-byte header + 4096-byte ROM
ATOM_ARCHIVE_4351_SIZE = 4351  # 16-byte header + 4096 ROM + 239 trailer
TAP_RECORD_SIZE = 4118   # 22-byte header + 4096-byte ROM
WORK_DIR_NAME = "_romscan_work"
OUTPUT_DIR = "data_ready"
REPORT_NAME = "ROM_REPORT.txt"


def atom_signature(data: bytes) -> int:
    """Historical Acorn Atom ROM checksum.

    This reproduces the Atom ROM signature routine used by the Atom community.

    Known validation values:
        Atom BASIC          D67D
        Floating Point ROM  AAA1
        original Kernel     E386
    """
    c = 0
    z = 0xFFFF
    y = 0x2D

    for a in data:
        for _ in range(8):
            c = c * 2 + (a & 1)
            a //= 2
            if c > z:
                c ^= y
                c &= z

    return c


def sh(value: int) -> str:
    return f"{value:04X}"


# =============================================================================
# ROM SIGNATURE DATABASE
# =============================================================================
#
# This is deliberately separate from the filenames used by V191.
#
# Add more known Atom ROM signatures here as we identify them.
#
# Format:
#     CRC: {
#         "name": human readable name,
#         "v139": canonical V191 filename or None,
#         "notes": optional notes
#     }
#
# Filenames downloaded from the Internet do NOT matter; checksum does.
#

ROM_DATABASE = {
    # -------------------------------------------------------------------------
    # Acorn system ROMs
    # -------------------------------------------------------------------------
    0xD67D: {
        "name": "Acorn Atom BASIC ROM",
        "v139": "Atom_Basic.rom",
        "notes": "V139 C-ROM",
    },
    0xAAA1: {
        "name": "Acorn Atom Floating Point ROM",
        "v139": "Atom_FloatingPoint.rom",
        "notes": "V139 D-ROM",
    },
    0x3353: {
        "name": "Floating Point Extension 2",
        "v139": None,
        "notes": "Convertible to V139 Floating Point ROM AAA1",
    },
    0xE386: {
        "name": "Original Acorn Atom Kernel ROM",
        "v139": None,
        "notes": "Can be patched for AtoMMC2 E000 -> F0B9",
    },
    0xF0B9: {
        "name": "AtoMMC2-patched Atom Kernel ROM",
        "v139": "Atom_Kernel.rom",
        "notes": "V139 F-ROM",
    },

    # -------------------------------------------------------------------------
    # AtoMMC2
    # -------------------------------------------------------------------------
    0x5072: {
        "name": "ATMMC3E ROM",
        "v139": "ATMMC3E.rom",
        "notes": "V139 E-ROM",
    },

    # -------------------------------------------------------------------------
    # A000 utility ROMs used by V139
    # -------------------------------------------------------------------------
    0x72FD: {
        "name": "P-Charme",
        "v139": "Atom_pcharme.rom",
        "notes": "V139 bank 0",
    },
    0xDE24: {
        "name": "ASBK-I v1.00",
        "v139": None,
        "notes": "Can be patched to ASBK-I v1.03 -> 988C",
    },
    0x988C: {
        "name": "ASBK-I v1.03",
        "v139": "ASBK-I_v1.03.rom",
        "notes": "V139 bank 1",
    },
    0x7049: {
        "name": "DISATOM",
        "v139": "DISATOM.rom",
        "notes": "V139 bank 2",
    },
    0x3626: {
        "name": "SALFAA 2.6",
        "v139": "SALFAA_2.6.rom",
        "notes": "V139 bank 3",
    },
    0x9F75: {
        "name": "SALFAA 2.6 (alternate image)",
        "v139": None,
        "notes": "Convertible to V139 SALFAA 2.6 checksum 3626",
    },
    0x19C1: {
        "name": "GAGS 2.3",
        "v139": "GAGS_2.3.rom",
        "notes": "V139 bank 4",
    },
    0x196E: {
        "name": "GAGS 2.3 (archive image)",
        "v139": None,
        "notes": "Convertible to V139 GAGS 2.3 checksum 19C1",
    },
    0x41CE: {
        "name": "PCBOX v1.02",
        "v139": "PCBOX_v1.02.rom",
        "notes": "V139 bank 5",
    },
    0x129F: {
        "name": "Psion EDIT (V139 filename TOOLBUG.rom)",
        "v139": "TOOLBUG.rom",
        "notes": "Exact V139 bank-6 image; historical archive identifies checksum 129F as Psion EDIT",
    },
    0xCCEB: {
        "name": "AXR-1",
        "v139": "AXR-1.rom",
        "notes": "V139 bank 7",
    },
    0xF545: {
        "name": "WE-ROM",
        "v139": None,
        "notes": "Identified from Wouter Ras ExtROM.Tap",
    },
    0x7261: {
        "name": "Utility Kit",
        "v139": None,
        "notes": "Identified from Wouter Ras ExtROM.Tap",
    },
    0x54D4: {
        "name": "COMBOX",
        "v139": None,
        "notes": "Identified from Wouter Ras ExtROM.Tap",
    },
    0x6D90: {
        "name": "Mouse-DOS Box",
        "v139": None,
        "notes": "Identified from Wouter Ras ExtROM.Tap",
    },
    0x0085: {
        "name": "Atomic Windows",
        "v139": None,
        "notes": "Identified from Wouter Ras ExtROM.Tap",
    },

    # -------------------------------------------------------------------------
    # Our resident ASBK-S V24 image is 1633 bytes, not a 4 KiB ROM.
    # It is handled separately below.
    # -------------------------------------------------------------------------
}


ASBKS_V24_SIGNATURE = 0x57CD
ASBKS_V24_SIZE = 1633


# =============================================================================
# PATCHES
# =============================================================================

# Floating Point Extension 2 (3353) -> V139 Floating Point ROM (AAA1)
#
# Exact 9-byte utility-interpreter patch recovered from the earlier V139
# system-ROM work.  ROM offsets are relative to $D000:
#
#   $04B0  04 -> 00
#   $04B1  E0 -> A0
#   $04B3  BF -> 40
#   $04B4  F0 -> D0
#   $04B7  00 -> 01
#   $04BA  40 -> BF
#   $04BC  83 -> 03
#   $04C1  05 -> 58
#   $04C2  E0 -> C5
#
FP2_TO_FP1_PATCH = {
    0x04B0: (0x04, 0x00),
    0x04B1: (0xE0, 0xA0),
    0x04B3: (0xBF, 0x40),
    0x04B4: (0xF0, 0xD0),
    0x04B7: (0x00, 0x01),
    0x04BA: (0x40, 0xBF),
    0x04BC: (0x83, 0x03),
    0x04C1: (0x05, 0x58),
    0x04C2: (0xE0, 0xC5),
}

# SALFAA 2.6 alternate image (9F75) -> V139 SALFAA 2.6 (3626)
# Exact byte differences derived from the uploaded 4096-byte ROM images.
SALFAA_9F75_TO_3626_PATCH = {
    0x0135: (0x06, 0x28),
    0x013A: (0x06, 0x28),
    0x0218: (0x06, 0x28),
    0x03C1: (0x06, 0x28),
    0x0534: (0x06, 0x28),
    0x0892: (0x06, 0x28),
    0x08FC: (0x06, 0x28),
    0x0904: (0x06, 0x28),
    0x091B: (0x06, 0x28),
    0x0A0D: (0xEA, 0x2C),
    0x0A0E: (0xEA, 0x01),
    0x0A0F: (0xEA, 0xB8),
    0x0A10: (0xEA, 0x10),
    0x0A11: (0xEA, 0x17),
    0x0A8D: (0xFB, 0x08),
    0x0A8E: (0xFE, 0xFF),
    0x0BA1: (0x06, 0x28),
    0x0CB4: (0x06, 0x28),
    0x0D4A: (0x06, 0x28),
    0x0E2F: (0x06, 0x28),
    0x0E52: (0x06, 0x28),
    0x0ED8: (0x06, 0x28),
    0x0FFF: (0x0C, 0xFF),
}

# GAGS 2.3 archive image (196E) -> V139 GAGS 2.3 (19C1)
# The ROMs are otherwise identical; only the unused final EPROM byte differs.
GAGS_196E_TO_19C1_PATCH = {
    0x0FFF: (0x0A, 0xFF),
}

# Original Atom Kernel E386 -> V139/AtoMMC2 Kernel F0B9
KERNEL_PATCH = {
    0x08EA: (0x0D, 0xCC),
    0x08EB: (0xE0, 0xEF),
    0x0F95: (0xB2, 0x00),
    0x0F96: (0xC2, 0xE0),
}

# ASBK-I v1.00 DE24 -> our tested ASBK-I v1.03 988C
ASBKI_100_TO_103_PATCH = {
    0x012D: (0x30, 0x33),
    0x0F5F: (0x15, 0x24),
    0x0F60: (0xC9, 0x15),
    0x0F61: (0x0C, 0x33),
    0x0F62: (0x30, 0x15),
    0x0F63: (0x07, 0x42),
    0x0F64: (0xA9, 0xD0),
    0x0F65: (0xF2, 0x08),
    0x0F66: (0x48, 0xB5),
    0x0F67: (0x48, 0x15),
    0x0F68: (0x6C, 0xC9),
    0x0F69: (0x02, 0x0C),
    0x0F6A: (0x02, 0x90),
    0x0F6B: (0xC9, 0x0C),
    0x0F6C: (0x00, 0xEA),
    0x0F6D: (0x10, 0xEA),
    0x0F6E: (0x07, 0xA9),
    0x0F6F: (0xA9, 0xF2),
    0x0F70: (0xF3, 0x48),
    0x0F72: (0x48, 0x6C),
    0x0F73: (0x6C, 0x02),
    0x0F75: (0x02, 0xEA),
}


REQUIRED = [
    ("Atom_Basic.rom",          4096, 0xD67D),
    ("Atom_FloatingPoint.rom",  4096, 0xAAA1),
    ("ATMMC3E.rom",             4096, 0x5072),
    ("Atom_Kernel.rom",         4096, 0xF0B9),
    ("Atom_pcharme.rom",        4096, 0x72FD),
    ("ASBK-I_v1.03.rom",        4096, 0x988C),
    ("DISATOM.rom",             4096, 0x7049),
    ("SALFAA_2.6.rom",          4096, 0x3626),
    ("GAGS_2.3.rom",            4096, 0x19C1),
    ("PCBOX_v1.02.rom",         4096, 0x41CE),
    ("TOOLBUG.rom",             4096, 0x129F),
    ("AXR-1.rom",               4096, 0xCCEB),
    ("ASBK-S_v24.bin",          1633, 0x57CD),
]


def ok_name(path: Path) -> Path:
    if path.suffix:
        return path.with_name(path.stem + "_OK" + path.suffix)
    return path.with_name(path.name + "_OK")


def apply_checked_patch(data: bytes, patch, description: str) -> bytes:
    out = bytearray(data)

    for offset, (expected, replacement) in patch.items():
        actual = out[offset]
        if actual != expected:
            raise ValueError(
                f"{description}: offset ${offset:04X} contains "
                f"${actual:02X}; expected ${expected:02X}"
            )
        out[offset] = replacement

    return bytes(out)





def is_generated_intermediate(path: Path) -> bool:
    """Ignore files created by earlier runs of this utility."""
    n = path.name.lower()
    if n.endswith("_raw.rom"):
        return True
    if n.endswith("_ok.rom"):
        return True
    if len(n) > 2 and n[1] == "_" and n[0] in "cdef":
        return True
    return False


def safe_name(text: str) -> str:
    """Make a short filesystem-safe name."""
    text = text.strip().strip("\x00")
    text = re.sub(r"[^A-Za-z0-9._+-]+", "_", text)
    return text.strip("._") or "ROM"


def atom_header_name(header: bytes) -> str:
    """Decode the 16-byte Atom/Wouter Ras filename field."""
    raw = header[:16].split(b"\x00", 1)[0]
    try:
        return raw.decode("ascii", errors="replace").strip()
    except Exception:
        return "ROM"


def write_raw_rom(data: bytes, dest: Path, report: list[str],
                  source_description: str) -> Path | None:
    """Write exactly one raw 4096-byte ROM and report its Atom checksum."""
    if len(data) != ROM_SIZE:
        report.append(
            f"ERROR: {source_description}: extracted {len(data)} bytes, expected 4096"
        )
        return None

    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)

    checksum = atom_signature(data)
    info = ROM_DATABASE.get(checksum)
    name = info["name"] if info else "UNKNOWN"

    report.append(
        f"RAW: {source_description:<40} -> "
        f"{dest.name:<28} checksum {sh(checksum)}  {name}"
    )
    return dest


def strip_4351_archive(path: Path, workdir: Path,
                       report: list[str]) -> Path | None:
    """Extract the 4096-byte ROM from a 4351-byte Atom archive file.

    Layout observed in the Atom archive:
        16-byte header
        4096-byte ROM
        239-byte trailer/padding
    """
    data = path.read_bytes()
    if len(data) != ATOM_ARCHIVE_4351_SIZE:
        return None

    rom = data[16:16 + ROM_SIZE]
    dest = workdir / f"{safe_name(path.stem)}_RAW.rom"
    return write_raw_rom(
        rom, dest, report,
        f"{path.name} [4351: strip 16-byte header, ignore trailer]"
    )


def split_tap(path: Path, workdir: Path,
              report: list[str]) -> list[Path]:
    """Split a multi-ROM Wouter Ras .TAP file.

    ExtROM.Tap uses consecutive fixed 4118-byte records:
        22-byte Atom/Wouter Ras header
        4096-byte ROM payload
    """
    data = path.read_bytes()
    result = []

    if len(data) == 0 or len(data) % TAP_RECORD_SIZE != 0:
        report.append(
            f"TAP: {path.name}: size {len(data)} is not a multiple of {TAP_RECORD_SIZE}; skipped"
        )
        return result

    count = len(data) // TAP_RECORD_SIZE
    report.append(f"TAP: {path.name}: {count} ROM record(s)")

    for index in range(count):
        rec = data[index * TAP_RECORD_SIZE:(index + 1) * TAP_RECORD_SIZE]
        header = rec[:22]
        rom = rec[22:]
        record_name = atom_header_name(header)
        dest = workdir / (
            f"{safe_name(path.stem)}_{index+1:02d}_{safe_name(record_name)}.rom"
        )
        raw = write_raw_rom(
            rom, dest, report,
            f"{path.name} record {index+1} '{record_name}'"
        )
        if raw is not None:
            result.append(raw)

    return result


def extract_zip(path: Path, workdir: Path,
                report: list[str]) -> list[Path]:
    """Extract files from a ZIP into the temporary scan area.

    Stored paths are flattened and prefixed with a sequence number so ZIP
    filenames cannot escape the work directory and duplicate basenames are safe.
    """
    result = []
    zip_dir = workdir / ("ZIP_" + safe_name(path.stem))
    zip_dir.mkdir(parents=True, exist_ok=True)

    try:
        with zipfile.ZipFile(path, "r") as zf:
            infos = [i for i in zf.infolist() if not i.is_dir()]
            report.append(f"ZIP: {path.name}: {len(infos)} file(s)")
            for index, info in enumerate(infos, 1):
                base = Path(info.filename).name
                if not base:
                    continue
                dest = zip_dir / f"{index:04d}_{safe_name(base)}"
                try:
                    dest.write_bytes(zf.read(info))
                    result.append(dest)
                except Exception as exc:
                    report.append(
                        f"ZIP: could not extract {info.filename}: {exc}"
                    )
    except (OSError, zipfile.BadZipFile) as exc:
        report.append(f"ZIP: {path.name}: {exc}")

    return result


def strip_atom_pc_header(path: Path, workdir: Path, report: list[str]) -> Path | None:
    """Strip known Atom-on-PC wrappers and write a raw 4096-byte ROM.

    Supported:
        4112 bytes = 16-byte header + 4096-byte ROM
        4118 bytes = 22-byte header + 4096-byte ROM

    The stripped file is written beside the source as:
        <stem>_RAW.rom

    Existing _RAW.rom files are overwritten.
    """
    data = path.read_bytes()
    size = len(data)

    if size == ATOM_PC_16_SIZE:
        header_len = 16
    elif size == ATOM_PC_22_SIZE:
        header_len = 22
    else:
        return None

    rom = data[header_len:]
    if len(rom) != ROM_SIZE:
        report.append(
            f"ERROR: {path.name}: stripped ROM is {len(rom)} bytes, expected 4096"
        )
        return None

    dest = workdir / (safe_name(path.stem) + "_RAW.rom")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(rom)

    crc = atom_signature(rom)
    info = ROM_DATABASE.get(crc)
    name = info["name"] if info else "UNKNOWN"

    report.append(
        f"HEADER: {path.name:<30} {size} bytes -> "
        f"{dest.name:<28} checksum {sh(crc)}  {name}"
    )
    return dest


def split_cdef(path: Path, workdir: Path, report: list[str]) -> list[Path]:
    data = path.read_bytes()
    result = []

    report.append("")
    report.append(f"16 KiB CDEF image: {path.name}")

    for index, letter in enumerate("CDEF"):
        chunk = data[index * ROM_SIZE:(index + 1) * ROM_SIZE]
        dest = workdir / f"{letter}_{safe_name(path.name)}"
        dest.write_bytes(chunk)  # overwrite intentionally

        crc = atom_signature(chunk)
        info = ROM_DATABASE.get(crc)

        if info:
            description = info["name"]
        else:
            description = "UNKNOWN"

        report.append(
            f"  {letter}: {dest.name:<32} checksum {sh(crc)}  {description}"
        )
        result.append(dest)

    return result


def copy_target(data: bytes, canonical: str, source: Path,
                outdir: Path, report: list[str]) -> None:
    dest = outdir / canonical
    dest.write_bytes(data)
    report.append(
        f"  -> data_ready/{canonical}  checksum {sh(atom_signature(data))}"
    )


def process_4k_rom(path: Path, outdir: Path, report: list[str],
                   recognised: list[tuple], unknown: list[tuple]) -> None:
    data = path.read_bytes()
    crc = atom_signature(data)

    report.append("")
    report.append(f"{path.name}")
    report.append(f"  Atom checksum {sh(crc)}")

    info = ROM_DATABASE.get(crc)

    if info:
        report.append(f"  IDENTIFIED: {info['name']}")
        if info.get("notes"):
            report.append(f"  {info['notes']}")

        recognised.append((path.name, crc, info["name"]))

        canonical = info.get("v139")
        if canonical:
            copy_target(data, canonical, path, outdir, report)
            return

        # Convertible GAGS 2.3 archive image.
        if crc == 0x196E:
            report.append("  NORMALISE: GAGS 2.3 196E -> 19C1")
            try:
                patched = apply_checked_patch(
                    data, GAGS_196E_TO_19C1_PATCH,
                    "GAGS 2.3 archive -> V139 normalisation"
                )
            except ValueError as exc:
                report.append(f"  PATCH FAILED: {exc}")
                return

            new_crc = atom_signature(patched)
            patched_file = ok_name(path)
            patched_file.write_bytes(patched)

            report.append(
                f"  NORMALISED: {patched_file.name} checksum {sh(new_crc)}"
            )

            if new_crc == 0x19C1:
                copy_target(
                    patched, "GAGS_2.3.rom",
                    patched_file, outdir, report
                )
            else:
                report.append(
                    f"  ERROR: expected 19C1, obtained {sh(new_crc)}"
                )
            return

        # Convertible SALFAA 2.6 alternate image.
        if crc == 0x9F75:
            report.append("  CONVERSION AVAILABLE: SALFAA 2.6 9F75 -> 3626")
            try:
                patched = apply_checked_patch(
                    data, SALFAA_9F75_TO_3626_PATCH,
                    "SALFAA 2.6 9F75 -> V139 3626 patch"
                )
            except ValueError as exc:
                report.append(f"  PATCH FAILED: {exc}")
                return

            new_crc = atom_signature(patched)
            patched_file = ok_name(path)
            patched_file.write_bytes(patched)

            report.append(
                f"  PATCHED: {patched_file.name} checksum {sh(new_crc)}"
            )

            if new_crc == 0x3626:
                copy_target(
                    patched, "SALFAA_2.6.rom",
                    patched_file, outdir, report
                )
            else:
                report.append(
                    f"  ERROR: expected 3626, obtained {sh(new_crc)}"
                )
            return

        # Convertible Floating Point Extension 2.
        if crc == 0x3353:
            report.append("  CONVERSION AVAILABLE: FP2 3353 -> FP AAA1")
            try:
                patched = apply_checked_patch(
                    data, FP2_TO_FP1_PATCH,
                    "Floating Point Extension 2 -> V139 Floating Point patch"
                )
            except ValueError as exc:
                report.append(f"  PATCH FAILED: {exc}")
                return

            new_crc = atom_signature(patched)
            patched_file = ok_name(path)
            patched_file.write_bytes(patched)

            report.append(
                f"  PATCHED: {patched_file.name} checksum {sh(new_crc)}"
            )

            if new_crc == 0xAAA1:
                copy_target(
                    patched, "Atom_FloatingPoint.rom",
                    patched_file, outdir, report
                )
            else:
                report.append(
                    f"  ERROR: expected AAA1, obtained {sh(new_crc)}"
                )
            return

        # Convertible original Kernel.
        if crc == 0xE386:
            report.append("  CONVERSION AVAILABLE: Kernel E386 -> F0B9")
            try:
                patched = apply_checked_patch(
                    data, KERNEL_PATCH, "AtoMMC2 Kernel patch"
                )
            except ValueError as exc:
                report.append(f"  PATCH FAILED: {exc}")
                return

            new_crc = atom_signature(patched)
            patched_file = ok_name(path)
            patched_file.write_bytes(patched)

            report.append(
                f"  PATCHED: {patched_file.name} checksum {sh(new_crc)}"
            )

            if new_crc == 0xF0B9:
                copy_target(
                    patched, "Atom_Kernel.rom",
                    patched_file, outdir, report
                )
            else:
                report.append(
                    f"  ERROR: expected F0B9, obtained {sh(new_crc)}"
                )
            return

        # Convertible ASBK-I v1.00.
        if crc == 0xDE24:
            report.append("  CONVERSION AVAILABLE: ASBK-I v1.00 -> v1.03")
            try:
                patched = apply_checked_patch(
                    data, ASBKI_100_TO_103_PATCH,
                    "ASBK-I v1.00 -> v1.03 patch"
                )
            except ValueError as exc:
                report.append(f"  PATCH FAILED: {exc}")
                return

            new_crc = atom_signature(patched)
            patched_file = ok_name(path)
            patched_file.write_bytes(patched)

            report.append(
                f"  PATCHED: {patched_file.name} checksum {sh(new_crc)}"
            )

            if new_crc == 0x988C:
                copy_target(
                    patched, "ASBK-I_v1.03.rom",
                    patched_file, outdir, report
                )
            else:
                report.append(
                    f"  ERROR: expected 988C, obtained {sh(new_crc)}"
                )
            return

        return

    report.append("  UNKNOWN ROM")
    unknown.append((path.name, crc))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Identify and prepare ACORN ATOM+ V191 ROMs (including BBC BASIC)."
    )
    parser.add_argument(
        "directory", nargs="?", default=".",
        help="Directory containing downloaded ROM files"
    )
    args = parser.parse_args()

    root = Path(args.directory).expanduser().resolve()
    if not root.is_dir():
        print(f"ERROR: directory not found: {root}")
        return 2

    outdir = root / OUTPUT_DIR
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    report = [
        "ACORN ATOM+ V191 ROM PREPARATION REPORT",
        "=" * 42,
        f"Input : {root}",
        f"Output: {outdir}",
        "",
        "ROM identification uses the historical Acorn Atom checksum.",
        "Filenames are ignored for identification.",
        "Supports ZIP archives, multi-ROM TAP files, 4112/4118-byte Atom-PC wrappers, and 4351-byte Atom archive files.",
        "",
        "Validation values:",
        "  D67D  Acorn Atom BASIC",
        "  AAA1  Floating Point ROM",
        "  E386  original Kernel ROM",
        "",
        "STEP 1 - SCAN ZIP/TAP / STRIP HEADERS / SPLIT CDEF",
        "-" * 42,
    ]

    workdir = root / WORK_DIR_NAME
    if workdir.exists():
        shutil.rmtree(workdir)
    workdir.mkdir(parents=True, exist_ok=True)

    initial_files = []
    for p in root.rglob("*"):
        if not p.is_file():
            continue

        # Never feed our own output or temporary extraction tree back in.
        try:
            rel = p.relative_to(root)
        except ValueError:
            continue

        if OUTPUT_DIR in rel.parts or WORK_DIR_NAME in rel.parts:
            continue
        if p.name == REPORT_NAME:
            continue
        if is_generated_intermediate(p):
            continue

        initial_files.append(p)

    # Queue lets ZIP contents themselves be TAP/headered/raw ROM files.
    queue = sorted(initial_files, key=lambda p: p.name.lower())
    rom_files = []
    project_bins = []
    bbc_candidates = []
    seen = set()

    while queue:
        path = queue.pop(0)
        try:
            resolved = str(path.resolve())
        except OSError:
            resolved = str(path)
        if resolved in seen:
            continue
        seen.add(resolved)

        if not path.is_file():
            continue

        suffix = path.suffix.lower()
        size = path.stat().st_size

        if suffix == ".zip":
            queue.extend(extract_zip(path, workdir, report))
            continue

        if suffix == ".tap":
            rom_files.extend(split_tap(path, workdir, report))
            continue

        if size == ATOM_ARCHIVE_4351_SIZE:
            raw = strip_4351_archive(path, workdir, report)
            if raw is not None:
                rom_files.append(raw)
            continue

        if size in (ATOM_PC_16_SIZE, ATOM_PC_22_SIZE):
            raw = strip_atom_pc_header(path, workdir, report)
            if raw is not None:
                rom_files.append(raw)
            continue

        if size == CDEF_SIZE:
            payload = path.read_bytes()
            if hashlib.sha256(payload).hexdigest() == BBC_BASIC_SHA256:
                bbc_candidates.append(path)
                report.append(f"BBC  : {path.name:<34} verified ATBASIC3 (SHA-256)")
                continue
            # Other 16 KiB files retain the original V10 C+D+E+F splitting.
            rom_files.extend(split_cdef(path, workdir, report))
            continue

        if size == ROM_SIZE:
            crc = atom_signature(path.read_bytes())
            info = ROM_DATABASE.get(crc)
            name = info["name"] if info else "UNKNOWN"
            report.append(
                f"4 KiB: {path.name:<34} checksum {sh(crc)}  {name}"
            )
            rom_files.append(path)
            continue

        if size == ASBKS_V24_SIZE:
            crc = atom_signature(path.read_bytes())
            name = "ASBK-S V24" if crc == ASBKS_V24_SIGNATURE else "UNKNOWN 1633-byte file"
            report.append(
                f"BIN  : {path.name:<34} checksum {sh(crc)}  {name}"
            )
            project_bins.append(path)
            continue

        report.append(f"skip : {path.name:<34} {size} bytes")

    report += [
        "",
        "STEP 2 - IDENTIFY ALL ROMS / BUILD data_ready",
        "-" * 46,
    ]

    recognised = []
    unknown = []

    seen_rom_payloads = set()

    for path in sorted(rom_files, key=lambda p: p.name.lower()):
        data = path.read_bytes()
        if data in seen_rom_payloads:
            report.append(
                f"\n{path.name}\n  DUPLICATE ROM - already processed"
            )
            continue
        seen_rom_payloads.add(data)
        process_4k_rom(
            path, outdir, report, recognised, unknown
        )

    for path in sorted(project_bins, key=lambda p: p.name.lower()):
        data = path.read_bytes()
        crc = atom_signature(data)

        report.append("")
        report.append(path.name)
        report.append(f"  Atom checksum {sh(crc)}")

        if crc == ASBKS_V24_SIGNATURE:
            report.append("  IDENTIFIED: ASBK-S V24")
            copy_target(
                data, "ASBK-S_v24.bin",
                path, outdir, report
            )
        else:
            report.append("  UNKNOWN 1633-byte file")

    # BBC BASIC is a 16 KiB image, not a bank of four Atom 4 KiB ROMs.
    # Accept it only when it matches the working V191 ATBASIC3 image.
    if not bbc_candidates:
        candidate = Path(__file__).resolve().parent / BBC_BASIC_NAME
        if candidate.is_file() and hashlib.sha256(candidate.read_bytes()).hexdigest() == BBC_BASIC_SHA256:
            bbc_candidates.append(candidate)
    if bbc_candidates:
        source = bbc_candidates[0]
        (outdir / BBC_BASIC_NAME).write_bytes(source.read_bytes())
        report.append(f"BBC BASIC: verified {source} -> data_ready/{BBC_BASIC_NAME}")
    else:
        report.append("BBC BASIC: ATBASIC3 not found (or does not match the V191 reference image)")

    # ASBK files are project-owned/project-supplied inputs for this build.
    # If they are not in the user's ROM-download directory, also look next
    # to this script and add them automatically when their Atom checksums match.
    script_dir = Path(__file__).resolve().parent

    asbki_src = script_dir / "ASBK-I_v1.03.rom"
    asbki_dst = outdir / "ASBK-I_v1.03.rom"
    if not asbki_dst.is_file() and asbki_src.is_file():
        data = asbki_src.read_bytes()
        if len(data) == ROM_SIZE and atom_signature(data) == 0x988C:
            copy_target(
                data, "ASBK-I_v1.03.rom",
                asbki_src, outdir, report
            )

    asbks_src = script_dir / "ASBK-S_v24.bin"
    asbks_dst = outdir / "ASBK-S_v24.bin"
    if not asbks_dst.is_file() and asbks_src.is_file():
        data = asbks_src.read_bytes()
        if len(data) == ASBKS_V24_SIZE and atom_signature(data) == 0x57CD:
            copy_target(
                data, "ASBK-S_v24.bin",
                asbks_src, outdir, report
            )

    report += [
        "",
        "STEP 3 - RECOGNISED ROMS FOUND",
        "-" * 34,
    ]

    if recognised:
        shown = set()
        for filename, crc, name in recognised:
            key = (crc, name)
            if key in shown:
                continue
            shown.add(key)
            report.append(
                f"{sh(crc)}  {filename:<34} {name}"
            )
    else:
        report.append("None")

    report += [
        "",
        "STEP 4 - UNKNOWN 4 KiB ROMS",
        "-" * 29,
    ]

    if unknown:
        shown = set()
        for filename, crc in unknown:
            if crc in shown:
                continue
            shown.add(crc)
            report.append(
                f"{sh(crc)}  {filename}"
            )
    else:
        report.append("None")

    report += [
        "",
        "STEP 5 - FILES REQUIRED FOR V191 data DIRECTORY",
        "-" * 48,
    ]

    complete = True
    report.append(f"Checking {len(REQUIRED) + 1} required V191 files:")

    for index, (filename, expected_size, expected_crc) in enumerate(REQUIRED, 1):
        path = outdir / filename

        if not path.is_file():
            report.append(
                f"{index:02d}/{len(REQUIRED)+1:02d} X - "
                f"{filename:<28} expected {sh(expected_crc)}"
            )
            complete = False
            continue

        data = path.read_bytes()
        actual_size = len(data)
        actual_crc = atom_signature(data)

        if actual_size == expected_size and actual_crc == expected_crc:
            report.append(
                f"{index:02d}/{len(REQUIRED)+1:02d} OK  - "
                f"{filename:<28} checksum {sh(actual_crc)}"
            )
        else:
            report.append(
                f"{index:02d}/{len(REQUIRED)+1:02d} X - "
                f"{filename:<28} got size {actual_size}, "
                f"checksum {sh(actual_crc)}; expected "
                f"size {expected_size}, checksum {sh(expected_crc)}"
            )
            complete = False

    bbc_out = outdir / BBC_BASIC_NAME
    bbc_index = len(REQUIRED) + 1
    if bbc_out.is_file():
        bbc_data = bbc_out.read_bytes()
        actual_sha = hashlib.sha256(bbc_data).hexdigest()
        valid = len(bbc_data) == 16384 and actual_sha == BBC_BASIC_SHA256
        report.append(f"{bbc_index:02d}/{bbc_index:02d} {'OK ' if valid else 'X -'} - ATBASIC3")
        report.append(f"       Size:            {len(bbc_data)} bytes")
        report.append(f"       Expected SHA256: {BBC_BASIC_SHA256}")
        report.append(f"       Actual SHA256:   {actual_sha}")
        if not valid:
            complete = False
    else:
        report.append(f"{bbc_index:02d}/{bbc_index:02d} X - ATBASIC3 (16 KiB BBC BASIC image missing)")
        report.append(f"       Expected SHA256: {BBC_BASIC_SHA256}")
        report.append("       Actual SHA256:   NOT FOUND")
        complete = False

    report += [
        "",
        "STORED ROM DATABASE",
        "-" * 19,
    ]

    for crc in sorted(ROM_DATABASE):
        info = ROM_DATABASE[crc]
        marker = ""
        if info.get("v139"):
            marker = f" -> {info['v139']}"
        report.append(
            f"{sh(crc)}  {info['name']}{marker}"
        )

    report.append(
        f"{sh(ASBKS_V24_SIGNATURE)}  ASBK-S V24 -> ASBK-S_v24.bin"
    )
    report.append("")
    report.append("16 KiB BBC BASIC IMAGE (SHA-256)")
    report.append("-------------------------------")
    report.append(f"{BBC_BASIC_SHA256}  ATBASIC3")

    report.append("")

    if complete:
        report.append(
            "RESULT: COMPLETE - data_ready contains the full V191 data set."
        )
    else:
        report.append(
            "RESULT: INCOMPLETE - required ROM not found in any scanned raw file, wrapper, TAP or ZIP."
        )

    txt = "\n".join(report) + "\n"
    report_path = root / REPORT_NAME
    report_path.write_text(txt, encoding="utf-8")

    print(txt)
    print(f"Report written to: {report_path}")
    print(f"Prepared files in: {outdir}")

    try:
        shutil.rmtree(workdir)
    except OSError:
        pass

    return 0 if complete else 1


if __name__ == "__main__":
    sys.exit(main())
