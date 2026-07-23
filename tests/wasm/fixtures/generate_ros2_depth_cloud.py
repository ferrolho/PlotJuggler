#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the deterministic ROS2 DepthCloud browser fixture.

Reproduce with mcap==1.3.1. The fixture covers raw 32FC1, headerless 16UC1
compressedDepth PNG, CameraInfo-by-frame matching, TF placement, a color-image
rejection, and an image whose declared dimensions cross the browser pixel cap.
The PNG stream uses stored DEFLATE blocks so output is independent of host zlib.
"""

import binascii
import hashlib
import io
import math
import struct
from importlib.metadata import version
from pathlib import Path

from mcap.reader import make_reader
from mcap.writer import CompressionType, IndexType, Writer
from scene3d_data_fixture_support import CdrWriter, STEP_NS, timestamp


OUTPUT = Path(__file__).with_name("ros2_depth_cloud_real.mcap")
SAMPLE_COUNT = 3
RAW_WIDTH = 160
RAW_HEIGHT = 120
COMPRESSED_WIDTH = 128
COMPRESSED_HEIGHT = 96

RAW_TOPIC = "/depth_raw"
COMPRESSED_TOPIC = "/depth_compressed"
CAMERA_TOPIC = "/camera_info"
COLOR_TOPIC = "/color_image"
TF_TOPIC = "/tf"
LIMIT_TOPIC = "/zz_too_large"

IMAGE_SCHEMA = b"""std_msgs/Header header
uint32 height
uint32 width
string encoding
uint8 is_bigendian
uint32 step
uint8[] data
================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
"""

COMPRESSED_IMAGE_SCHEMA = b"""std_msgs/Header header
string format
uint8[] data
================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
"""

CAMERA_INFO_SCHEMA = b"""std_msgs/Header header
uint32 height
uint32 width
string distortion_model
float64[] d
float64[9] k
float64[9] r
float64[12] p
uint32 binning_x
uint32 binning_y
sensor_msgs/RegionOfInterest roi
================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
================================================================================
MSG: sensor_msgs/RegionOfInterest
uint32 x_offset
uint32 y_offset
uint32 height
uint32 width
bool do_rectify
"""

TF_SCHEMA = b"""geometry_msgs/TransformStamped[] transforms
================================================================================
MSG: geometry_msgs/TransformStamped
std_msgs/Header header
string child_frame_id
geometry_msgs/Transform transform
================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
================================================================================
MSG: geometry_msgs/Transform
geometry_msgs/Vector3 translation
geometry_msgs/Quaternion rotation
================================================================================
MSG: geometry_msgs/Vector3
float64 x
float64 y
float64 z
================================================================================
MSG: geometry_msgs/Quaternion
float64 x
float64 y
float64 z
float64 w
"""


def header(cdr: CdrWriter, timestamp_ns: int, frame_id: str) -> None:
    timestamp(cdr, timestamp_ns)
    cdr.string(frame_id)


def raw_depth_bytes(sequence: int) -> bytes:
    payload = bytearray(RAW_WIDTH * RAW_HEIGHT * 4)
    for row in range(RAW_HEIGHT):
        for column in range(RAW_WIDTH):
            index = (row * RAW_WIDTH) + column
            depth = 1.0 + (0.35 * sequence) + (0.7 * column / (RAW_WIDTH - 1)) + (0.25 * row / (RAW_HEIGHT - 1))
            if index % 997 == 0:
                depth = math.nan
            elif index % 613 == 0:
                depth = 0.0
            struct.pack_into("<f", payload, index * 4, depth)
    return bytes(payload)


def raw_depth_cdr(sequence: int) -> bytes:
    cdr = CdrWriter()
    timestamp_ns = sequence * STEP_NS
    header(cdr, timestamp_ns, "camera_optical")
    cdr.uint32(RAW_HEIGHT)
    cdr.uint32(RAW_WIDTH)
    cdr.string("32FC1")
    cdr.uint8(0)
    cdr.uint32(RAW_WIDTH * 4)
    cdr.byte_sequence(raw_depth_bytes(sequence))
    return bytes(cdr.data)


def color_image_cdr() -> bytes:
    width = 32
    height = 24
    rgb = bytearray()
    for row in range(height):
        for column in range(width):
            rgb.extend((column * 7 % 256, row * 11 % 256, 180))
    cdr = CdrWriter()
    header(cdr, 0, "camera_optical")
    cdr.uint32(height)
    cdr.uint32(width)
    cdr.string("rgb8")
    cdr.uint8(0)
    cdr.uint32(width * 3)
    cdr.byte_sequence(bytes(rgb))
    return bytes(cdr.data)


def oversized_depth_png() -> bytes:
    width = 1001
    height = 1000
    ihdr = struct.pack(">IIBBBBB", width, height, 16, 0, 0, 0, 0)
    # QImageReader obtains dimensions from IHDR. The adapter rejects them before
    # asking the codec to inflate; a deliberately incomplete raster keeps this
    # guard fixture tiny while still exercising the production size preflight.
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", stored_zlib(b"\x00\x00\x00"))
        + png_chunk(b"IEND", b"")
    )


def oversized_depth_cdr() -> bytes:
    cdr = CdrWriter()
    header(cdr, 0, "camera_optical")
    cdr.string("16UC1; compressedDepth png")
    cdr.byte_sequence(oversized_depth_png())
    return bytes(cdr.data)


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    checksum = binascii.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)


def stored_zlib(payload: bytes) -> bytes:
    output = bytearray(b"\x78\x01")
    offset = 0
    while offset < len(payload):
        block = payload[offset : offset + 65535]
        offset += len(block)
        output.append(1 if offset == len(payload) else 0)
        output.extend(struct.pack("<HH", len(block), len(block) ^ 0xFFFF))
        output.extend(block)
    a = 1
    b = 0
    for byte in payload:
        a = (a + byte) % 65521
        b = (b + a) % 65521
    output.extend(struct.pack(">I", (b << 16) | a))
    return bytes(output)


def depth_png(sequence: int) -> bytes:
    rows = bytearray()
    for row in range(COMPRESSED_HEIGHT):
        rows.append(0)
        for column in range(COMPRESSED_WIDTH):
            index = (row * COMPRESSED_WIDTH) + column
            millimetres = 850 + (sequence * 180) + (column * 4) + (row * 2)
            if index % 701 == 0:
                millimetres = 0
            rows.extend(struct.pack(">H", millimetres))
    ihdr = struct.pack(">IIBBBBB", COMPRESSED_WIDTH, COMPRESSED_HEIGHT, 16, 0, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", stored_zlib(bytes(rows)))
        + png_chunk(b"IEND", b"")
    )


def compressed_depth_cdr(sequence: int) -> bytes:
    cdr = CdrWriter()
    timestamp_ns = sequence * STEP_NS
    header(cdr, timestamp_ns, "camera_optical")
    cdr.string("16UC1; compressedDepth png")
    cdr.byte_sequence(depth_png(sequence))
    return bytes(cdr.data)


def camera_info_cdr() -> bytes:
    fx = 135.0
    fy = 132.0
    cx = (RAW_WIDTH - 1) / 2.0
    cy = (RAW_HEIGHT - 1) / 2.0
    k = (fx, 0.0, cx, 0.0, fy, cy, 0.0, 0.0, 1.0)
    r = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    p = (fx, 0.0, cx, 0.0, 0.0, fy, cy, 0.0, 0.0, 0.0, 1.0, 0.0)
    cdr = CdrWriter()
    header(cdr, 0, "camera_optical")
    cdr.uint32(RAW_HEIGHT)
    cdr.uint32(RAW_WIDTH)
    cdr.string("plumb_bob")
    distortion = (0.01, -0.02, 0.0, 0.0, 0.0)
    cdr.uint32(len(distortion))
    for value in distortion + k + r + p:
        cdr.float64(value)
    cdr.uint32(0)  # binning_x
    cdr.uint32(0)  # binning_y
    cdr.uint32(0)  # roi.x_offset
    cdr.uint32(0)  # roi.y_offset
    cdr.uint32(0)  # roi.height
    cdr.uint32(0)  # roi.width
    cdr.boolean(False)
    return bytes(cdr.data)


def tf_cdr(sequence: int) -> bytes:
    cdr = CdrWriter()
    timestamp_ns = sequence * STEP_NS
    cdr.uint32(1)
    header(cdr, timestamp_ns, "map")
    cdr.string("camera_optical")
    translation = (0.3 * sequence, -0.1 * sequence, 0.25)
    yaw = 0.12 * sequence
    quaternion = (0.0, 0.0, math.sin(yaw / 2.0), math.cos(yaw / 2.0))
    for value in translation + quaternion:
        cdr.float64(value)
    return bytes(cdr.data)


def build_fixture() -> bytes:
    output = io.BytesIO()
    writer = Writer(
        output,
        chunk_size=8 * 1024 * 1024,
        compression=CompressionType.ZSTD,
        index_types=IndexType.ALL,
        enable_crcs=True,
        enable_data_crcs=True,
    )
    writer.start(profile="ros2", library="pj-w19e-depthcloud-generator/1")
    image_schema = writer.register_schema(name="sensor_msgs/msg/Image", encoding="ros2msg", data=IMAGE_SCHEMA)
    compressed_schema = writer.register_schema(
        name="sensor_msgs/msg/CompressedImage", encoding="ros2msg", data=COMPRESSED_IMAGE_SCHEMA
    )
    camera_schema = writer.register_schema(
        name="sensor_msgs/msg/CameraInfo", encoding="ros2msg", data=CAMERA_INFO_SCHEMA
    )
    tf_schema = writer.register_schema(name="tf2_msgs/msg/TFMessage", encoding="ros2msg", data=TF_SCHEMA)
    channels = {
        RAW_TOPIC: writer.register_channel(topic=RAW_TOPIC, message_encoding="cdr", schema_id=image_schema),
        COMPRESSED_TOPIC: writer.register_channel(
            topic=COMPRESSED_TOPIC, message_encoding="cdr", schema_id=compressed_schema
        ),
        CAMERA_TOPIC: writer.register_channel(topic=CAMERA_TOPIC, message_encoding="cdr", schema_id=camera_schema),
        COLOR_TOPIC: writer.register_channel(topic=COLOR_TOPIC, message_encoding="cdr", schema_id=image_schema),
        TF_TOPIC: writer.register_channel(topic=TF_TOPIC, message_encoding="cdr", schema_id=tf_schema),
        LIMIT_TOPIC: writer.register_channel(
            topic=LIMIT_TOPIC, message_encoding="cdr", schema_id=compressed_schema
        ),
    }
    writer.add_message(channel_id=channels[CAMERA_TOPIC], log_time=0, publish_time=0, sequence=0, data=camera_info_cdr())
    writer.add_message(channel_id=channels[COLOR_TOPIC], log_time=0, publish_time=0, sequence=0, data=color_image_cdr())
    writer.add_message(
        channel_id=channels[LIMIT_TOPIC], log_time=0, publish_time=0, sequence=0, data=oversized_depth_cdr()
    )
    for sequence in range(SAMPLE_COUNT):
        timestamp_ns = sequence * STEP_NS
        writer.add_message(
            channel_id=channels[RAW_TOPIC],
            log_time=timestamp_ns,
            publish_time=timestamp_ns,
            sequence=sequence,
            data=raw_depth_cdr(sequence),
        )
        writer.add_message(
            channel_id=channels[COMPRESSED_TOPIC],
            log_time=timestamp_ns,
            publish_time=timestamp_ns,
            sequence=sequence,
            data=compressed_depth_cdr(sequence),
        )
        writer.add_message(
            channel_id=channels[TF_TOPIC],
            log_time=timestamp_ns,
            publish_time=timestamp_ns,
            sequence=sequence,
            data=tf_cdr(sequence),
        )
    writer.finish()
    return output.getvalue()


def verify_fixture(payload: bytes) -> None:
    messages = list(make_reader(io.BytesIO(payload), validate_crcs=True).iter_messages(log_time_order=True))
    by_topic = {
        topic: [message for _, channel, message in messages if channel.topic == topic]
        for topic in (RAW_TOPIC, COMPRESSED_TOPIC, CAMERA_TOPIC, COLOR_TOPIC, TF_TOPIC, LIMIT_TOPIC)
    }
    expected = {
        RAW_TOPIC: SAMPLE_COUNT,
        COMPRESSED_TOPIC: SAMPLE_COUNT,
        CAMERA_TOPIC: 1,
        COLOR_TOPIC: 1,
        TF_TOPIC: SAMPLE_COUNT,
        LIMIT_TOPIC: 1,
    }
    if {topic: len(entries) for topic, entries in by_topic.items()} != expected:
        raise RuntimeError("fixture topic counts changed")
    expected_times = [sequence * STEP_NS for sequence in range(SAMPLE_COUNT)]
    for topic in (RAW_TOPIC, COMPRESSED_TOPIC, TF_TOPIC):
        if [message.log_time for message in by_topic[topic]] != expected_times:
            raise RuntimeError(f"{topic} timestamps changed")
    if any(message.publish_time != message.log_time for _, _, message in messages):
        raise RuntimeError("publish/log timestamp mismatch")
    if not all(by_topic[COMPRESSED_TOPIC][index].data.endswith(depth_png(index)) for index in range(SAMPLE_COUNT)):
        raise RuntimeError("compressedDepth PNG payload changed")


def main() -> None:
    if version("mcap") != "1.3.1":
        raise RuntimeError(f"mcap==1.3.1 is required, found {version('mcap')}")
    payload = build_fixture()
    verify_fixture(payload)
    OUTPUT.write_bytes(payload)
    print(f"wrote {OUTPUT} ({len(payload)} bytes, sha256={hashlib.sha256(payload).hexdigest()})")


if __name__ == "__main__":
    main()
