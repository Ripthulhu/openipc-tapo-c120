#!/usr/bin/env python3
"""Shared target catalog helpers for the Tapo tooling."""

from __future__ import annotations

import json
import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CATALOG_PATH = REPO_ROOT / "analysis" / "tapo_exploit_compatibility_catalog.json"


def repo_path(path: str | Path) -> Path:
    value = Path(path)
    if value.is_absolute():
        return value
    return REPO_ROOT / value


def normalize_token(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", text.lower())


def load_catalog(path: str | Path | None = None) -> dict:
    catalog_path = Path(path) if path else DEFAULT_CATALOG_PATH
    if not catalog_path.is_absolute():
        catalog_path = REPO_ROOT / catalog_path
    with catalog_path.open("r", encoding="utf-8") as f:
        return json.load(f)


def target_label(target: dict) -> str:
    return f"{target['display']} v{target['hw']} ({target['id']})"


def target_family_status(target: dict, family: str) -> str:
    if family == "c120":
        return target.get("c120", {}).get("status") or target.get("compatibility", {}).get("c120Path", "not_indicated")
    if family == "c200":
        return target.get("c200", {}).get("status") or target.get("compatibility", {}).get("c200RestorePath", "not_indicated")
    raise ValueError(f"unknown target family: {family}")


def iter_targets(
    family: str | None = None,
    statuses: set[str] | None = None,
    catalog_path: str | Path | None = None,
    target_ids: set[str] | None = None,
):
    catalog = load_catalog(catalog_path)
    for target in catalog.get("targets", []):
        if target_ids is not None and target.get("id") not in target_ids:
            continue
        if family:
            status = target_family_status(target, family)
            if status == "not_indicated":
                continue
            if statuses and status not in statuses:
                continue
        yield target


def resolve_target(
    model_or_id: str,
    hw: str | None = None,
    family: str | None = None,
    statuses: set[str] | None = None,
    catalog_path: str | Path | None = None,
    target_ids: set[str] | None = None,
) -> dict:
    token = normalize_token(model_or_id)
    matches = []
    for target in iter_targets(family, statuses, catalog_path, target_ids):
        fields = [target["id"], target["display"], target["model"], *target.get("aliases", [])]
        if any(normalize_token(field) == token for field in fields):
            if hw is None or str(target.get("hw")) == str(hw).lstrip("vV"):
                matches.append(target)
    if not matches:
        family_note = f" for {family}" if family else ""
        filter_note = " in the active target filter" if target_ids is not None else ""
        raise ValueError(f"no catalog target matches {model_or_id!r}{family_note}{filter_note}")
    if len(matches) > 1:
        options = ", ".join(target_label(target) for target in matches)
        raise ValueError(f"{model_or_id!r} matches multiple hardware revisions: {options}; pass --target-hw")
    return matches[0]


def format_target_table(
    family: str,
    statuses: set[str] | None = None,
    catalog_path: str | Path | None = None,
    target_ids: set[str] | None = None,
) -> str:
    rows = []
    for target in iter_targets(family, statuses, catalog_path, target_ids):
        fw = target.get("firmware") or {}
        version = fw.get("version")
        build = fw.get("build")
        firmware = f"{version} build {build}" if version and build else fw.get("name", "unknown")
        load_status = ""
        route = ""
        if family == "c200":
            load_status = target.get("c200", {}).get("loadSensor", {}).get("status", "")
            route = "full" if target.get("c200", {}).get("fullLoadSensorRoute") else "parser_only"
        rows.append((target["display"], "v" + str(target["hw"]), target_family_status(target, family), load_status, route, firmware, target["id"]))
    if not rows:
        return "No matching targets in catalog."
    width_display = max(len("Target"), *(len(row[0]) for row in rows))
    width_hw = max(len("HW"), *(len(row[1]) for row in rows))
    width_status = max(len("Status"), *(len(row[2]) for row in rows))
    if family == "c200":
        width_load = max(len("loadSensor"), *(len(row[3]) for row in rows))
        width_route = max(len("Route"), *(len(row[4]) for row in rows))
        lines = [
            f"{'Target':<{width_display}}  {'HW':<{width_hw}}  {'Status':<{width_status}}  {'loadSensor':<{width_load}}  {'Route':<{width_route}}  Firmware  ID",
            f"{'-' * width_display}  {'-' * width_hw}  {'-' * width_status}  {'-' * width_load}  {'-' * width_route}  --------  --",
        ]
        for display, hw, status, load_status, route, firmware, target_id in rows:
            lines.append(f"{display:<{width_display}}  {hw:<{width_hw}}  {status:<{width_status}}  {load_status:<{width_load}}  {route:<{width_route}}  {firmware}  {target_id}")
    else:
        lines = [
            f"{'Target':<{width_display}}  {'HW':<{width_hw}}  {'Status':<{width_status}}  Firmware  ID",
            f"{'-' * width_display}  {'-' * width_hw}  {'-' * width_status}  --------  --",
        ]
        for display, hw, status, _load_status, _route, firmware, target_id in rows:
            lines.append(f"{display:<{width_display}}  {hw:<{width_hw}}  {status:<{width_status}}  {firmware}  {target_id}")
    return "\n".join(lines)


def add_target_args(parser, family: str) -> None:
    parser.add_argument("--target-model", help="Model/display/id from the generated target catalog.")
    parser.add_argument("--target-hw", help="Hardware revision for --target-model, for example 1 or v1.")
    parser.add_argument("--target-catalog", type=Path, default=DEFAULT_CATALOG_PATH, help="Target catalog JSON path.")
    if family == "c200":
        help_text = "List catalog targets with C200 restore-parser markers and exit; loadSensor/Route show whether the startup pivot was found."
    else:
        help_text = f"List catalog targets compatible with the {family.upper()} route and exit."
    parser.add_argument("--list-targets", action="store_true", help=help_text)


def select_target(
    args,
    family: str,
    statuses: set[str] | None = None,
    target_ids: set[str] | None = None,
) -> dict | None:
    if getattr(args, "list_targets", False):
        print(format_target_table(family, statuses, args.target_catalog, target_ids))
        raise SystemExit(0)
    model = getattr(args, "target_model", None)
    if not model:
        return None
    return resolve_target(model, getattr(args, "target_hw", None), family, statuses, args.target_catalog, target_ids)


def loadsensor_paths(target: dict, library_dir: str | Path | None = None) -> list[Path]:
    paths = []
    load_info = target.get("c200", {}).get("loadSensor", {})
    if load_info.get("path"):
        paths.append(repo_path(load_info["path"]))
    base = Path(library_dir) if library_dir else repo_path("analysis/faq4191_loadsensor")
    if not base.is_absolute():
        base = REPO_ROOT / base
    paths.extend(
        [
            base / target["id"] / "loadSensor.original",
            base / target["id"] / "loadSensor",
        ]
    )
    deduped = []
    seen = set()
    for path in paths:
        key = str(path)
        if key not in seen:
            seen.add(key)
            deduped.append(path)
    return deduped
