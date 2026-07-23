#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the deterministic five-map glTF/GLB renderer fixture."""

import argparse
import json
import struct
import zlib
from pathlib import Path


OUTPUT = Path(__file__).with_name("embedded_pbr.glb")
NEUTRAL_OUTPUT = Path(__file__).with_name("embedded_neutral.glb")


def png_rgba(width: int, height: int, pixels: bytes) -> bytes:
    if len(pixels) != width * height * 4:
        raise ValueError("RGBA payload size does not match the image dimensions")

    def chunk(kind: bytes, payload: bytes) -> bytes:
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    rows = b"".join(
        b"\0" + pixels[row * width * 4 : (row + 1) * width * 4]
        for row in range(height)
    )
    return b"".join((
        b"\x89PNG\r\n\x1a\n",
        chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)),
        chunk(b"IDAT", zlib.compress(rows, level=9)),
        chunk(b"IEND", b""),
    ))


def solid_rgba(width: int, height: int, color: tuple[int, int, int, int]) -> bytes:
    return bytes(color) * (width * height)


def build_fixture() -> bytes:
    positions = struct.pack(
        "<12f", 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 0.0
    )
    normals = struct.pack("<12f", *(0.0, 0.0, 1.0) * 4)
    uvs = struct.pack("<8f", 0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0)
    indices = struct.pack("<6H", 0, 1, 2, 0, 2, 3)

    # Each slot is deliberately non-neutral. The normal points partly along +X;
    # metallic-roughness follows glTF's G=roughness/B=metallic packing; AO is
    # half strength; emissive is cyan. Base/emissive are color maps, the other
    # three are data maps.
    images = (
        png_rgba(2, 2, bytes((80, 80, 80, 255, 180, 180, 180, 255)) * 2),
        png_rgba(2, 2, solid_rgba(2, 2, (255, 72, 224, 255))),
        png_rgba(2, 2, solid_rgba(2, 2, (218, 128, 218, 255))),
        png_rgba(2, 2, solid_rgba(2, 2, (96, 255, 255, 255))),
        png_rgba(2, 2, solid_rgba(2, 2, (0, 255, 255, 255))),
    )

    binary = bytearray()
    views = []

    def append_view(payload: bytes, target: int | None = None) -> int:
        while len(binary) % 4:
            binary.append(0)
        view = {"buffer": 0, "byteOffset": len(binary), "byteLength": len(payload)}
        if target is not None:
            view["target"] = target
        views.append(view)
        binary.extend(payload)
        return len(views) - 1

    position_view = append_view(positions, 34962)
    normal_view = append_view(normals, 34962)
    uv_view = append_view(uvs, 34962)
    index_view = append_view(indices, 34963)
    image_views = [append_view(image) for image in images]
    while len(binary) % 4:
        binary.append(0)

    document = {
        "accessors": [
            {
                "bufferView": position_view,
                "componentType": 5126,
                "count": 4,
                "type": "VEC3",
                "min": [0, 0, 0],
                "max": [1, 1, 0],
            },
            {"bufferView": normal_view, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": uv_view, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": index_view, "componentType": 5123, "count": 6, "type": "SCALAR"},
        ],
        "asset": {"generator": "pj4-w19i-pbr-fixture", "version": "2.0"},
        "bufferViews": views,
        "buffers": [{"byteLength": len(binary)}],
        "images": [{"bufferView": view, "mimeType": "image/png"} for view in image_views],
        "materials": [{
            "emissiveFactor": [0.0, 1.0, 1.0],
            "emissiveTexture": {"index": 4},
            "name": "all_five_maps",
            "normalTexture": {"index": 2},
            "occlusionTexture": {"index": 3, "strength": 1.0},
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.8, 0.7, 0.6, 1.0],
                "baseColorTexture": {"index": 0},
                "metallicFactor": 0.65,
                "metallicRoughnessTexture": {"index": 1},
                "roughnessFactor": 0.55,
            },
        }],
        "meshes": [{
            "primitives": [{
                "attributes": {"NORMAL": 1, "POSITION": 0, "TEXCOORD_0": 2},
                "indices": 3,
                "material": 0,
            }],
        }],
        "nodes": [{"mesh": 0}],
        "samplers": [{}],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "textures": [{"sampler": 0, "source": index} for index in range(5)],
    }

    json_chunk = json.dumps(document, separators=(",", ":"), sort_keys=True).encode("utf-8")
    json_chunk += b" " * (-len(json_chunk) % 4)
    total = 12 + 8 + len(json_chunk) + 8 + len(binary)
    return b"".join((
        struct.pack("<4sII", b"glTF", 2, total),
        struct.pack("<II", len(json_chunk), 0x4E4F534A),
        json_chunk,
        struct.pack("<II", len(binary), 0x004E4942),
        bytes(binary),
    ))


def build_neutral_fixture() -> bytes:
    """Build the same quad with no material or texture slots as a control."""
    payloads = (
        (struct.pack(
            "<12f", 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 0.0
        ), 34962),
        (struct.pack("<12f", *(0.0, 0.0, 1.0) * 4), 34962),
        (struct.pack("<8f", 0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0), 34962),
        (struct.pack("<6H", 0, 1, 2, 0, 2, 3), 34963),
    )
    binary = bytearray()
    views = []
    for payload, target in payloads:
        while len(binary) % 4:
            binary.append(0)
        views.append({
            "buffer": 0,
            "byteOffset": len(binary),
            "byteLength": len(payload),
            "target": target,
        })
        binary.extend(payload)
    while len(binary) % 4:
        binary.append(0)

    document = {
        "accessors": [
            {
                "bufferView": 0,
                "componentType": 5126,
                "count": 4,
                "type": "VEC3",
                "min": [0, 0, 0],
                "max": [1, 1, 0],
            },
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
        ],
        "asset": {"generator": "pj4-w19i-neutral-control", "version": "2.0"},
        "bufferViews": views,
        "buffers": [{"byteLength": len(binary)}],
        "materials": [{
            "name": "neutral_map_free_control",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.65, 0.65, 0.65, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 1.0,
            },
        }],
        "meshes": [{"primitives": [{
            "attributes": {"NORMAL": 1, "POSITION": 0, "TEXCOORD_0": 2},
            "indices": 3,
            "material": 0,
        }]}],
        "nodes": [{"mesh": 0}],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
    }
    json_chunk = json.dumps(document, separators=(",", ":"), sort_keys=True).encode("utf-8")
    json_chunk += b" " * (-len(json_chunk) % 4)
    total = 12 + 8 + len(json_chunk) + 8 + len(binary)
    return b"".join((
        struct.pack("<4sII", b"glTF", 2, total),
        struct.pack("<II", len(json_chunk), 0x4E4F534A),
        json_chunk,
        struct.pack("<II", len(binary), 0x004E4942),
        bytes(binary),
    ))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="fail unless the checked-in fixture is byte-exact")
    args = parser.parse_args()
    outputs = ((OUTPUT, build_fixture()), (NEUTRAL_OUTPUT, build_neutral_fixture()))
    if args.check:
        for output, payload in outputs:
            if not output.is_file() or output.read_bytes() != payload:
                raise SystemExit(f"{output} is stale; regenerate it with {Path(__file__).name}")
            print(f"verified {output} ({len(payload)} bytes)")
        return
    for output, payload in outputs:
        output.write_bytes(payload)
        print(f"wrote {output} ({len(payload)} bytes)")


if __name__ == "__main__":
    main()
