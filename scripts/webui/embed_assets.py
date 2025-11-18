#!/usr/bin/env python3
"""Generate embedded web asset data for the ESP-IDF build.

The script scans the webui/ directory for static assets and emits a C++ include
file that defines byte arrays and metadata for runtime lookup. Assets can be
optionally gzip-compressed to reduce flash usage; the metadata notes whether
compression was applied so the HTTP layer can set the proper header.
"""

from __future__ import annotations

import argparse
import gzip
from pathlib import Path
from typing import Iterable, List, Tuple

_TEXT_EXTENSIONS = {
    ".html": "text/html",
    ".htm": "text/html",
    ".js": "application/javascript",
    ".css": "text/css",
    ".json": "application/json",
}


def iter_assets(root: Path) -> Iterable[Tuple[Path, Path, str]]:
    # Files to exclude from embedding (development files)
    exclude_patterns = {
        "package.json",
        "package-lock.json",
        "tsconfig.json",
        "vite.config.ts",
        "svelte.config.js",
        ".gitignore",
        "build.sh",
    }

    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(root)

        # Skip excluded files
        if path.name in exclude_patterns:
            continue
        # Skip dev index.html in root (use dist/index.html instead)
        if rel == Path("index.html"):
            continue
        # Skip source directories (src, node_modules, etc.)
        if any(part in {"src", "node_modules", ".git"} for part in rel.parts):
            continue

        content_type = _TEXT_EXTENSIONS.get(path.suffix.lower())
        if content_type is None:
            continue

        # Flatten dist/ directory structure: dist/index.html -> index.html, dist/assets/* -> assets/*
        if rel.parts[0] == "dist" and len(rel.parts) > 1:
            rel = Path(*rel.parts[1:])  # Remove 'dist/' prefix

        yield path, rel, content_type


def format_byte_array(data: bytes) -> str:
    columns = 12
    lines: List[str] = []
    for i in range(0, len(data), columns):
        chunk = data[i : i + columns]
        lines.append(", ".join(f"0x{byte:02x}" for byte in chunk))
    return ",\n    ".join(lines)


def sanitize_symbol(rel_path: Path) -> str:
    # Replace hyphens and other invalid C++ identifier chars with underscores
    parts = [part.replace("-", "_").replace(".", "_") for part in rel_path.with_suffix("").parts]
    return "kAsset_" + "_".join(parts)


def generate_include(
    assets: Iterable[Tuple[Path, Path, str]],
    output: Path,
    gzip_assets: bool,
) -> None:
    lines: List[str] = []
    lines.append("// AUTO-GENERATED FILE. DO NOT EDIT.")
    lines.append("#include <cstddef>")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("namespace ui::assets::generated {")
    lines.append("using ::ui::assets::Asset;")
    lines.append("")


    manifest_entries: List[str] = []

    for src_path, rel_path, content_type in assets:
        data = src_path.read_bytes()
        if gzip_assets:
            data = gzip.compress(data)
        symbol = sanitize_symbol(rel_path)
        lines.append(f"alignas(4) const std::uint8_t {symbol}[] = {{")
        lines.append(f"    {format_byte_array(data)}")
        lines.append("};")
        lines.append("")
        compressed_flag = "true" if gzip_assets else "false"
        manifest_entries.append(
            '    {"/%s", %s, sizeof(%s), "%s", %s},'
            % (rel_path.as_posix(), symbol, symbol, content_type, compressed_flag)
        )

    lines.append("const Asset kAssets[] = {")
    lines.extend(manifest_entries)
    lines.append("};")
    lines.append("constexpr std::size_t kAssetCount = sizeof(kAssets) / sizeof(kAssets[0]);")
    lines.append("\n}  // namespace ui::assets::generated\n")

    output.write_text("\n".join(lines))


def main() -> None:
    parser = argparse.ArgumentParser(description="Embed web assets for firmware build")
    parser.add_argument("--source", type=Path, default=Path("webui"), help="Directory containing assets")
    parser.add_argument("--output", type=Path, required=True, help="Output include file path")
    parser.add_argument("--gzip", action="store_true", help="Compress assets before embedding")
    args = parser.parse_args()

    if not args.source.is_dir():
        raise SystemExit(f"Source directory {args.source} does not exist")

    assets = list(iter_assets(args.source))
    if not assets:
        raise SystemExit("No embeddable assets found in source directory")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    generate_include(assets, args.output, args.gzip)


if __name__ == "__main__":
    main()
