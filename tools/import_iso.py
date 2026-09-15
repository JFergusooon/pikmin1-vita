#!/usr/bin/env python3
"""Validate and extract user-owned Pikmin (USA) GameCube disc data."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterator

GAMECUBE_MAGIC = b"\xC2\x33\x9F\x3D"
EXPECTED_GAME_ID = b"GPIE01"
CHUNK_SIZE = 1024 * 1024


class ImportError(Exception):
    pass


@dataclass(frozen=True)
class DiscInfo:
    game_id: str
    revision: int
    title: str
    fst_offset: int
    fst_size: int


def find_dolphin_tool() -> Path | None:
    for command in ("DolphinTool", "dolphin-tool"):
        found = shutil.which(command)
        if found:
            return Path(found)
    app_tool = Path("/Applications/Dolphin.app/Contents/MacOS/DolphinTool")
    return app_tool if app_tool.is_file() else None


def find_dtk() -> Path | None:
    found = shutil.which("dtk")
    if found:
        return Path(found)
    bundled = Path(__file__).parents[1] / "upstream" / "pikmin" / "build" / "tools" / "dtk"
    return bundled if bundled.is_file() else None


@contextmanager
def raw_disc_image(source: Path) -> Iterator[Path]:
    if source.suffix.lower() in {".iso", ".gcm"}:
        yield source
        return

    dolphin_tool = find_dolphin_tool()
    dtk = find_dtk()
    if dolphin_tool is None and dtk is None:
        raise ImportError(
            f"{source.suffix or 'compressed'} images require dtk or DolphinTool; "
            "run the upstream tool setup or provide an ISO"
        )

    with tempfile.TemporaryDirectory(prefix="pikmin-vita-") as temporary:
        temporary_path = Path(temporary)
        converted = temporary_path / "Pikmin-USA-Rev1.iso"
        print(f"Converting {source.suffix.upper()} to a temporary ISO...")
        if dtk is not None:
            local_source = temporary_path / f"source{source.suffix.lower()}"
            local_source.symlink_to(source.resolve())
            command = [
                str(dtk.resolve()),
                "disc",
                "convert",
                local_source.name,
                converted.name,
            ]
            command_cwd = temporary_path
        else:
            command = [
                str(dolphin_tool),
                "convert",
                "-i",
                str(source),
                "-o",
                str(converted),
                "-f",
                "iso",
            ]
            command_cwd = None
        try:
            subprocess.run(command, check=True, cwd=command_cwd)
        except subprocess.CalledProcessError as error:
            raise ImportError(f"disc conversion failed ({error.returncode})") from error
        yield converted


def read_exact(image: BinaryIO, offset: int, size: int) -> bytes:
    image.seek(offset)
    value = image.read(size)
    if len(value) != size:
        raise ImportError(f"disc image ended at 0x{offset + len(value):X}")
    return value


def inspect_disc(image: BinaryIO) -> DiscInfo:
    header = read_exact(image, 0, 0x42C)
    if header[0x1C:0x20] != GAMECUBE_MAGIC:
        raise ImportError("not a valid GameCube disc image")
    if header[:6] != EXPECTED_GAME_ID:
        actual = header[:6].decode("ascii", errors="replace")
        raise ImportError(f"expected Pikmin USA (GPIE01), found {actual!r}")

    fst_offset, fst_size = struct.unpack_from(">II", header, 0x424)
    image.seek(0, 2)
    image_size = image.tell()
    if fst_size < 12 or fst_offset + fst_size > image_size:
        raise ImportError("disc filesystem table is outside the image")

    title = header[0x20:0x400].split(b"\0", 1)[0].decode("ascii", errors="replace")
    return DiscInfo("GPIE01", header[7], title, fst_offset, fst_size)


def safe_name(name_table: bytes, offset: int) -> str:
    if offset >= len(name_table):
        raise ImportError("invalid filename offset in disc filesystem")
    end = name_table.find(b"\0", offset)
    if end < 0:
        raise ImportError("unterminated filename in disc filesystem")
    name = name_table[offset:end].decode("shift_jis", errors="strict")
    if not name or name in {".", ".."} or "/" in name or "\\" in name:
        raise ImportError(f"unsafe filename in disc filesystem: {name!r}")
    return name


def copy_extent(image: BinaryIO, offset: int, size: int, destination: Path) -> None:
    image.seek(0, 2)
    if offset < 0 or size < 0 or offset + size > image.tell():
        raise ImportError(f"file extent for {destination.name!r} is outside the image")

    destination.parent.mkdir(parents=True, exist_ok=True)
    image.seek(offset)
    remaining = size
    with destination.open("wb") as output:
        while remaining:
            chunk = image.read(min(remaining, CHUNK_SIZE))
            if not chunk:
                raise ImportError(f"disc image ended while extracting {destination.name!r}")
            output.write(chunk)
            remaining -= len(chunk)


def extract_files(image: BinaryIO, info: DiscInfo, output: Path) -> int:
    fst = read_exact(image, info.fst_offset, info.fst_size)
    root_kind_name, _, entry_count = struct.unpack_from(">III", fst, 0)
    if root_kind_name >> 24 != 1 or entry_count == 0:
        raise ImportError("invalid disc filesystem root")

    entries_size = entry_count * 12
    if entries_size > len(fst):
        raise ImportError("disc filesystem entry table is truncated")
    names = fst[entries_size:]

    output.mkdir(parents=True, exist_ok=True)
    directories: list[tuple[int, Path]] = [(entry_count, output)]
    extracted = 0

    for index in range(1, entry_count):
        while directories and index >= directories[-1][0]:
            directories.pop()
        if not directories:
            raise ImportError("invalid directory nesting in disc filesystem")

        kind_name, value, size_or_end = struct.unpack_from(">III", fst, index * 12)
        is_directory = (kind_name >> 24) == 1
        name = safe_name(names, kind_name & 0x00FFFFFF)
        destination = directories[-1][1] / name

        if is_directory:
            if size_or_end <= index or size_or_end > entry_count:
                raise ImportError(f"invalid directory range for {name!r}")
            destination.mkdir(parents=True, exist_ok=True)
            directories.append((size_or_end, destination))
        else:
            copy_extent(image, value, size_or_end, destination)
            extracted += 1

    return extracted


def write_manifest(output: Path, info: DiscInfo, extracted_files: int) -> None:
    manifest = {
        "format": 1,
        "game_id": info.game_id,
        "revision": info.revision,
        "title": info.title,
        "files": extracted_files,
        "source_assets_included_in_vpk": False,
    }
    with (output / ".pikmin-vita.json").open("w", encoding="utf-8") as file:
        json.dump(manifest, file, indent=2)
        file.write("\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Extract a user-owned Pikmin (USA) ISO for Pikmin Vita"
    )
    parser.add_argument("iso", type=Path, help="path to PIKMIN (USA).iso")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("runtime-data"),
        help="output directory (default: runtime-data)",
    )
    parser.add_argument(
        "--force", action="store_true", help="replace an existing output directory"
    )
    parser.add_argument(
        "--verify-only", action="store_true", help="validate without extracting files"
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        with raw_disc_image(args.iso) as image_path:
            with image_path.open("rb") as image:
                info = inspect_disc(image)
                print(
                    f"Found {info.title or 'Pikmin'} ({info.game_id}, revision {info.revision})"
                )
                if info.revision != 1:
                    print(
                        "Warning: USA Rev 1 is the current port target; this disc may "
                        "need version-specific compatibility work.",
                        file=sys.stderr,
                    )
                if args.verify_only:
                    return 0

                if args.output.exists():
                    if not args.force:
                        raise ImportError(
                            f"{args.output} already exists; pass --force to replace it"
                        )
                    shutil.rmtree(args.output)

                extracted = extract_files(image, info, args.output)
                write_manifest(args.output, info, extracted)
                print(f"Extracted {extracted} files to {args.output}")
                print("Copy that directory's contents to ux0:data/pikmin/ on your Vita.")
                return 0
    except (OSError, UnicodeError, ImportError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
