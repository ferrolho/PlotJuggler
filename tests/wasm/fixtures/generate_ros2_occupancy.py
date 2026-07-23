#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the deterministic ROS2 occupancy-grid/update browser fixture.

The non-square, rotated base grid proves origin-pose geometry. Three patches
prove incremental upload, clipped updates, and backward reconstruction; a
second base proves epoch replacement. The final 8193x1 grid crosses the
browser texture-dimension limit while remaining cheap to decode and store.

Reproduce with mcap==1.3.1:

    python3 tests/wasm/fixtures/generate_ros2_occupancy.py
"""

import hashlib
import io
import math
import struct
from importlib.metadata import version
from pathlib import Path

from mcap.reader import make_reader
from mcap.writer import CompressionType, IndexType, Writer
from scene3d_data_fixture_support import CdrWriter, STEP_NS, timestamp


OUTPUT = Path(__file__).with_name("ros2_occupancy_real.mcap")
WIDTH = 160
HEIGHT = 96
BASE_TOPIC = "/costmap"
UPDATE_TOPIC = BASE_TOPIC + "_updates"
TF_TOPIC = "/tf"
LIMIT_TOPIC = "/zz_too_large"

GRID_SCHEMA = b"""std_msgs/Header header
nav_msgs/MapMetaData info
int8[] data
================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
================================================================================
MSG: nav_msgs/MapMetaData
builtin_interfaces/Time map_load_time
float32 resolution
uint32 width
uint32 height
geometry_msgs/Pose origin
================================================================================
MSG: geometry_msgs/Pose
geometry_msgs/Point position
geometry_msgs/Quaternion orientation
================================================================================
MSG: geometry_msgs/Point
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

UPDATE_SCHEMA = b"""std_msgs/Header header
int32 x
int32 y
uint32 width
uint32 height
int8[] data
================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
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


def quaternion_from_yaw(yaw: float) -> tuple[float, float, float, float]:
    return (0.0, 0.0, math.sin(yaw / 2.0), math.cos(yaw / 2.0))


def base_cells(epoch: int) -> bytes:
    cells = bytearray([0] * (WIDTH * HEIGHT))
    for row in range(HEIGHT):
        for column in range(WIDTH):
            index = row * WIDTH + column
            if row in (0, HEIGHT - 1) or column in (0, WIDTH - 1):
                cells[index] = 100
            elif (row * 3 + column * 5 + epoch) % 41 == 0:
                cells[index] = 255  # signed int8 -1, unknown
            elif epoch != 0 and 58 <= column < 102 and 38 <= row < 58:
                cells[index] = 75
    return bytes(cells)


def occupancy_grid_cdr(timestamp_ns: int, cells: bytes, width: int, height: int) -> bytes:
    cdr = CdrWriter()
    timestamp(cdr, timestamp_ns)
    cdr.string("grid_frame")
    timestamp(cdr, 0)
    cdr.float32(0.05)
    cdr.uint32(width)
    cdr.uint32(height)
    for value in (-2.0, -1.0, 0.05):
        cdr.float64(value)
    for value in quaternion_from_yaw(math.radians(30.0)):
        cdr.float64(value)
    cdr.byte_sequence(cells)
    return bytes(cdr.data)


def update_cdr(
    timestamp_ns: int,
    x: int,
    y: int,
    width: int,
    height: int,
    value: int,
    data_cells: int | None = None,
) -> bytes:
    cdr = CdrWriter()
    timestamp(cdr, timestamp_ns)
    cdr.string("grid_frame")
    cdr.int32(x)
    cdr.int32(y)
    cdr.uint32(width)
    cdr.uint32(height)
    cdr.byte_sequence(bytes([value]) * (width * height if data_cells is None else data_cells))
    return bytes(cdr.data)


def tf_cdr(sequence: int) -> bytes:
    timestamp_ns = sequence * STEP_NS
    cdr = CdrWriter()
    cdr.uint32(1)
    timestamp(cdr, timestamp_ns)
    cdr.string("map")
    cdr.string("grid_frame")
    for value in (1.0, 2.0, 0.0) + quaternion_from_yaw(math.radians(10.0)):
        cdr.float64(value)
    return bytes(cdr.data)


def build_fixture() -> bytes:
    output = io.BytesIO()
    writer = Writer(
        output,
        compression=CompressionType.ZSTD,
        index_types=IndexType.ALL,
        enable_crcs=True,
        enable_data_crcs=True,
    )
    writer.start(profile="ros2", library="pj-w16-occupancy-generator/1")
    grid_schema = writer.register_schema(
        name="nav_msgs/msg/OccupancyGrid", encoding="ros2msg", data=GRID_SCHEMA
    )
    update_schema = writer.register_schema(
        name="map_msgs/OccupancyGridUpdate", encoding="ros2msg", data=UPDATE_SCHEMA
    )
    tf_schema = writer.register_schema(
        name="tf2_msgs/msg/TFMessage", encoding="ros2msg", data=TF_SCHEMA
    )
    base_channel = writer.register_channel(
        topic=BASE_TOPIC, message_encoding="cdr", schema_id=grid_schema
    )
    update_channel = writer.register_channel(
        topic=UPDATE_TOPIC, message_encoding="cdr", schema_id=update_schema
    )
    tf_channel = writer.register_channel(topic=TF_TOPIC, message_encoding="cdr", schema_id=tf_schema)
    limit_channel = writer.register_channel(
        topic=LIMIT_TOPIC, message_encoding="cdr", schema_id=grid_schema
    )

    for sequence in range(5):
        timestamp_ns = sequence * STEP_NS
        writer.add_message(
            channel_id=tf_channel,
            log_time=timestamp_ns,
            publish_time=timestamp_ns,
            sequence=sequence,
            data=tf_cdr(sequence),
        )
        if sequence == 0:
            writer.add_message(
                channel_id=base_channel,
                log_time=timestamp_ns,
                publish_time=timestamp_ns,
                sequence=sequence,
                data=occupancy_grid_cdr(timestamp_ns, base_cells(0), WIDTH, HEIGHT),
            )
            continue
        if sequence == 4:
            writer.add_message(
                channel_id=base_channel,
                log_time=timestamp_ns,
                publish_time=timestamp_ns,
                sequence=sequence,
                data=occupancy_grid_cdr(timestamp_ns, base_cells(1), WIDTH, HEIGHT),
            )
            continue
        patches = {
            1: (20, 18, 24, 16, 100),
            2: (72, 30, 32, 20, 50),
            3: (-4, 70, 20, 20, 100),
        }
        writer.add_message(
            channel_id=update_channel,
            log_time=timestamp_ns,
            publish_time=timestamp_ns,
            sequence=sequence,
            data=update_cdr(timestamp_ns, *patches[sequence]),
        )

    # A semantically corrupt but syntactically valid CDR sample must match
    # desktop's fault isolation: skip only this short-data patch, retain the
    # valid reconstruction, and surface a warning. A truncated CDR would be
    # rejected earlier by the official importer's transactional scan and would
    # therefore test the wrong boundary.
    corrupt_time = 5 * STEP_NS // 2
    writer.add_message(
        channel_id=update_channel,
        log_time=corrupt_time,
        publish_time=corrupt_time,
        sequence=99,
        data=update_cdr(corrupt_time, 8, 8, 4, 4, 100, data_cells=3),
    )

    oversized_width = 8193
    writer.add_message(
        channel_id=limit_channel,
        log_time=0,
        publish_time=0,
        sequence=0,
        data=occupancy_grid_cdr(0, bytes(oversized_width), oversized_width, 1),
    )
    writer.finish()
    return output.getvalue()


def verify_fixture(payload: bytes) -> None:
    messages = list(
        make_reader(io.BytesIO(payload), validate_crcs=True).iter_messages(log_time_order=True)
    )
    by_topic = {
        topic: [entry for entry in messages if entry[1].topic == topic]
        for topic in (BASE_TOPIC, UPDATE_TOPIC, TF_TOPIC, LIMIT_TOPIC)
    }
    expected_counts = {BASE_TOPIC: 2, UPDATE_TOPIC: 4, TF_TOPIC: 5, LIMIT_TOPIC: 1}
    if {topic: len(entries) for topic, entries in by_topic.items()} != expected_counts:
        raise RuntimeError("fixture topic counts changed")
    if [message.log_time for _, _, message in by_topic[BASE_TOPIC]] != [0, 4 * STEP_NS]:
        raise RuntimeError("base epochs changed")
    if [message.log_time for _, _, message in by_topic[UPDATE_TOPIC]] != [
        STEP_NS,
        2 * STEP_NS,
        5 * STEP_NS // 2,
        3 * STEP_NS,
    ]:
        raise RuntimeError("update timestamps changed")
    if any(message.publish_time != message.log_time for _, _, message in messages):
        raise RuntimeError("publish/log timestamp mismatch")


def main() -> None:
    if version("mcap") != "1.3.1":
        raise RuntimeError(f"mcap==1.3.1 is required, found {version('mcap')}")
    payload = build_fixture()
    verify_fixture(payload)
    OUTPUT.write_bytes(payload)
    print(
        f"wrote {OUTPUT} ({len(payload)} bytes, "
        f"sha256={hashlib.sha256(payload).hexdigest()})"
    )


if __name__ == "__main__":
    main()
