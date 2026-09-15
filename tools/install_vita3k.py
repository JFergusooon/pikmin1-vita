#!/usr/bin/env python3
"""Install the built VPK and imported game data into a local Vita3K install.

Vita3K keeps an emulated Vita filesystem under its preference path, so the VPK
is unpacked to ux0:/app/<title id> and runtime-data is mirrored to
ux0:/data/pikmin rather than going through the emulator's installer UI.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import sys
import zipfile

TITLE_ID = "PIKMINVIT"

PREF_PATH_CANDIDATES = (
    "~/Library/Application Support/Vita3K/Vita3K",
    "~/.local/share/Vita3K/Vita3K",
    "~/AppData/Roaming/Vita3K/Vita3K",
)


def find_pref_path(explicit: pathlib.Path | None) -> pathlib.Path:
    if explicit is not None:
        pref_path = explicit.expanduser()
        if not (pref_path / "fs").is_dir():
            raise SystemExit(f"no Vita3K filesystem at {pref_path}/fs")
        return pref_path

    for candidate in PREF_PATH_CANDIDATES:
        pref_path = pathlib.Path(candidate).expanduser()
        if (pref_path / "fs").is_dir():
            return pref_path

    raise SystemExit(
        "could not locate a Vita3K install; pass --pref-path with the directory "
        "that contains Vita3K's fs/ folder"
    )


def install_vpk(vpk: pathlib.Path, ux0: pathlib.Path) -> pathlib.Path:
    if not vpk.is_file():
        raise SystemExit(f"missing VPK at {vpk}; build it first with cmake --build build")

    destination = ux0 / "app" / TITLE_ID
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)

    with zipfile.ZipFile(vpk) as archive:
        for name in archive.namelist():
            target = (destination / name).resolve()
            if not str(target).startswith(str(destination.resolve())):
                raise SystemExit(f"refusing to extract outside destination: {name}")
        archive.extractall(destination)

    if not (destination / "eboot.bin").is_file():
        raise SystemExit(f"{vpk} did not contain eboot.bin")
    return destination


def install_runtime_data(runtime_data: pathlib.Path, ux0: pathlib.Path) -> pathlib.Path | None:
    manifest = runtime_data / ".pikmin-vita.json"
    if not manifest.is_file():
        return None

    destination = ux0 / "data" / "pikmin"
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(runtime_data, destination)
    return destination


def main() -> None:
    root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pref-path", type=pathlib.Path, default=None)
    parser.add_argument("--vpk", type=pathlib.Path, default=root / "build" / "pikmin_vita.vpk")
    parser.add_argument("--runtime-data", type=pathlib.Path, default=root / "runtime-data")
    parser.add_argument(
        "--skip-runtime-data",
        action="store_true",
        help="install only the application, leaving ux0:/data/pikmin untouched",
    )
    args = parser.parse_args()

    pref_path = find_pref_path(args.pref_path)
    ux0 = pref_path / "fs" / "ux0"

    app = install_vpk(args.vpk, ux0)
    print(f"installed {TITLE_ID} to {app}")

    if args.skip_runtime_data:
        print("skipped runtime-data")
        return

    data = install_runtime_data(args.runtime_data, ux0)
    if data is None:
        print(
            f"no import manifest in {args.runtime_data}; run tools/import_iso.py to "
            "populate it (the prototype still boots without game data)",
            file=sys.stderr,
        )
    else:
        print(f"installed game data to {data}")


if __name__ == "__main__":
    main()
