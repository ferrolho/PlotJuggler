#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the deterministic ROS2 CompressedImage MCAP browser fixture.

The fixture is synthetic project test data covered by the repository license.
Reproduce it with:

    python3 -m pip install mcap==1.3.1
    python3 tests/wasm/fixtures/generate_ros2_compressed_image.py

PNG payloads use stored DEFLATE blocks assembled here instead of a host zlib
encoder, keeping the checked-in MCAP bytes stable across zlib releases. The
three frames have deliberately disjoint saturated colors so browser acceptance
can identify the post-draw frame through a QRhi framebuffer readback.
"""

import base64
import binascii
import hashlib
import io
import struct
import textwrap
from importlib.metadata import version
from pathlib import Path

from mcap.reader import make_reader
from mcap.writer import CompressionType, IndexType, Writer


OUTPUT = Path(__file__).with_name("ros2_compressed_image.mcap.b64")
WIDTH = 96
HEIGHT = 72
STEP_NS = 1_000_000_000
FRAME_COLORS = (
    ((255, 0, 0), (0, 0, 255)),
    ((0, 255, 0), (255, 0, 255)),
    ((255, 255, 0), (0, 255, 255)),
)
SCHEMA = (
    b"std_msgs/Header header\n"
    b"string format\n"
    b"uint8[] data\n"
    b"================\n"
    b"MSG: std_msgs/Header\n"
    b"builtin_interfaces/Time stamp\n"
    b"string frame_id\n"
    b"================\n"
    b"MSG: builtin_interfaces/Time\n"
    b"int32 sec\n"
    b"uint32 nanosec\n"
)


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    checksum = binascii.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)


def stored_zlib(payload: bytes) -> bytes:
    """Return a deterministic zlib stream containing stored DEFLATE blocks."""
    out = bytearray(b"\x78\x01")
    offset = 0
    while offset < len(payload):
        block = payload[offset : offset + 65535]
        offset += len(block)
        out.append(1 if offset == len(payload) else 0)
        out.extend(struct.pack("<HH", len(block), len(block) ^ 0xFFFF))
        out.extend(block)

    a = 1
    b = 0
    for byte in payload:
        a = (a + byte) % 65521
        b = (b + a) % 65521
    out.extend(struct.pack(">I", (b << 16) | a))
    return bytes(out)


def png_frame(left: tuple[int, int, int], right: tuple[int, int, int]) -> bytes:
    rows = bytearray()
    for _ in range(HEIGHT):
        rows.append(0)  # PNG filter: None
        rows.extend(bytes(left) * (WIDTH // 2))
        rows.extend(bytes(right) * (WIDTH - WIDTH // 2))
    ihdr = struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", stored_zlib(bytes(rows)))
        + png_chunk(b"IEND", b"")
    )


class CdrWriter:
    def __init__(self) -> None:
        self.data = bytearray(b"\x00\x01\x00\x00")

    def align(self, alignment: int) -> None:
        self.data.extend(b"\x00" * (-len(self.data) % alignment))

    def int32(self, value: int) -> None:
        self.align(4)
        self.data.extend(struct.pack("<i", value))

    def uint32(self, value: int) -> None:
        self.align(4)
        self.data.extend(struct.pack("<I", value))

    def string(self, value: str) -> None:
        encoded = value.encode("utf-8") + b"\x00"
        self.uint32(len(encoded))
        self.data.extend(encoded)

    def byte_sequence(self, value: bytes) -> None:
        self.uint32(len(value))
        self.data.extend(value)


def compressed_image_cdr(sequence: int, png: bytes) -> bytes:
    cdr = CdrWriter()
    cdr.int32(sequence)
    cdr.uint32(0)
    cdr.string("camera_optical")
    cdr.string("png")
    cdr.byte_sequence(png)
    return bytes(cdr.data)


def build_fixture() -> bytes:
    output = io.BytesIO()
    writer = Writer(
        output,
        compression=CompressionType.NONE,
        index_types=IndexType.ALL,
        enable_crcs=True,
        enable_data_crcs=True,
    )
    writer.start(profile="ros2", library="pj-w11-generator/1")
    schema_id = writer.register_schema(
        name="sensor_msgs/msg/CompressedImage",
        encoding="ros2msg",
        data=SCHEMA,
    )
    channel_id = writer.register_channel(
        topic="/camera/image/compressed",
        message_encoding="cdr",
        schema_id=schema_id,
    )
    for sequence, colors in enumerate(FRAME_COLORS):
        timestamp = sequence * STEP_NS
        writer.add_message(
            channel_id=channel_id,
            log_time=timestamp,
            publish_time=timestamp,
            sequence=sequence,
            data=compressed_image_cdr(sequence, png_frame(*colors)),
        )
    writer.finish()
    return output.getvalue()


def verify_fixture(payload: bytes) -> None:
    reader = make_reader(io.BytesIO(payload), validate_crcs=True)
    messages = list(reader.iter_messages(log_time_order=True))
    if len(messages) != len(FRAME_COLORS):
        raise RuntimeError(f"expected {len(FRAME_COLORS)} messages, found {len(messages)}")
    for sequence, (schema, channel, message) in enumerate(messages):
        if schema.name != "sensor_msgs/msg/CompressedImage" or channel.topic != "/camera/image/compressed":
            raise RuntimeError("fixture schema/topic identity changed")
        if message.log_time != sequence * STEP_NS or message.publish_time != message.log_time:
            raise RuntimeError("fixture timestamps do not match the declared sequence")
        expected_png = png_frame(*FRAME_COLORS[sequence])
        if not message.data.endswith(expected_png):
            raise RuntimeError(f"frame {sequence} does not contain the expected PNG payload")


def main() -> None:
    if version("mcap") != "1.3.1":
        raise RuntimeError(f"mcap==1.3.1 is required, found {version('mcap')}")
    payload = build_fixture()
    verify_fixture(payload)
    encoded = base64.b64encode(payload).decode("ascii")
    OUTPUT.write_text("\n".join(textwrap.wrap(encoded, width=76)) + "\n", encoding="ascii")
    digest = hashlib.sha256(payload).hexdigest()
    print(f"wrote {OUTPUT} ({len(payload)} bytes, sha256={digest})")


if __name__ == "__main__":
    main()
