#!/usr/bin/env python3
"""Create a single KernelSU module archive containing every supported GKI LKM."""

from __future__ import annotations

import argparse
import gzip
import json
import os
import re
import stat
import sys
import time
import zipfile
from pathlib import Path
from string import Template


ROOT = Path(__file__).resolve().parents[1]
TARGETS_FILE = ROOT / "config" / "targets.json"
MODULE_TEMPLATE_DIR = ROOT / "module"
MODULE_FILE = "gki_ntsync.ko"
SEMVER_RE = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
TARGET_ID_RE = re.compile(r"^[a-z0-9.-]+$")


def load_targets(path: Path = TARGETS_FILE) -> list[dict[str, str]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    targets = data.get("targets")
    if not isinstance(targets, list) or not targets:
        raise ValueError("targets.json must contain a non-empty targets list")

    seen_ids: set[str] = set()
    for target in targets:
        if not isinstance(target, dict):
            raise ValueError("each target must be an object")
        target_id = target.get("id")
        pattern = target.get("kmi_pattern")
        if not isinstance(target_id, str) or not TARGET_ID_RE.fullmatch(target_id):
            raise ValueError(f"invalid target id: {target_id!r}")
        if target_id in seen_ids:
            raise ValueError(f"duplicate target id: {target_id}")
        if not isinstance(pattern, str) or not pattern:
            raise ValueError(f"missing kmi_pattern for {target_id}")
        seen_ids.add(target_id)
    return targets


def version_code(version: str) -> int:
    match = SEMVER_RE.fullmatch(version)
    if not match:
        raise ValueError("version must use MAJOR.MINOR.PATCH format")
    major, minor, patch = (int(part) for part in match.groups())
    if minor > 999 or patch > 999:
        raise ValueError("minor and patch versions must be at most 999")
    return major * 1_000_000 + minor * 1_000 + patch


def render_kmi_selector(targets: list[dict[str, str]]) -> str:
    lines = ["#!/system/bin/sh", "", "select_kmi() {", '  case "$1" in']
    for target in targets:
        lines.extend(
            [
                f'    {target["kmi_pattern"]})',
                f'      printf "%s\\n" "{target["id"]}"',
                "      return 0",
                "      ;;",
            ]
        )
    lines.extend(["    *)", "      return 1", "      ;;", "  esac", "}", ""])
    return "\n".join(lines)


def zip_info(name: str, executable: bool, timestamp: tuple[int, int, int, int, int, int]) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time=timestamp)
    info.compress_type = zipfile.ZIP_DEFLATED
    mode = 0o755 if executable else 0o644
    info.external_attr = (stat.S_IFREG | mode) << 16
    return info


def archive_timestamp() -> tuple[int, int, int, int, int, int]:
    source_date_epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "315532800"))
    return time.gmtime(max(source_date_epoch, 315532800))[:6]


def required_modules(modules_dir: Path, targets: list[dict[str, str]]) -> dict[str, Path]:
    modules: dict[str, Path] = {}
    for target in targets:
        module = modules_dir / target["id"] / MODULE_FILE
        if not module.is_file() or module.stat().st_size == 0:
            raise FileNotFoundError(f"missing module for {target['id']}: {module}")
        modules[target["id"]] = module
    return modules


def build_archive(
    modules_dir: Path,
    output: Path,
    version: str,
    package_version_code: int | None = None,
) -> Path:
    targets = load_targets()
    modules = required_modules(modules_dir, targets)
    resolved_version_code = package_version_code if package_version_code is not None else version_code(version)
    if resolved_version_code < 1:
        raise ValueError("version code must be positive")

    output.parent.mkdir(parents=True, exist_ok=True)
    timestamp = archive_timestamp()
    module_prop = Template((MODULE_TEMPLATE_DIR / "module.prop.in").read_text(encoding="utf-8"))
    scripts = ("customize.sh", "load.sh", "service.sh", "late-load.sh", "action.sh")

    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        archive.writestr(
            zip_info("module.prop", False, timestamp),
            module_prop.substitute(VERSION=version, VERSION_CODE=resolved_version_code),
        )
        archive.writestr(zip_info("select-kmi.sh", True, timestamp), render_kmi_selector(targets))
        for script in scripts:
            archive.writestr(
                zip_info(script, True, timestamp),
                (MODULE_TEMPLATE_DIR / script).read_text(encoding="utf-8"),
            )
        for target_id, module in modules.items():
            compressed_module = gzip.compress(module.read_bytes(), mtime=0)
            archive.writestr(
                zip_info(f"payload/{target_id}/{MODULE_FILE}.gz", False, timestamp),
                compressed_module,
            )
    return output


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--modules-dir", type=Path, required=True, help=f"directory containing <target>/{MODULE_FILE}")
    parser.add_argument("--version", required=True, help="module version in MAJOR.MINOR.PATCH format")
    parser.add_argument("--version-code", type=int, help="override the generated numeric version code")
    parser.add_argument("--output", type=Path, required=True, help="output KernelSU module zip")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    try:
        output = build_archive(args.modules_dir, args.output, args.version, args.version_code)
    except (FileNotFoundError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
