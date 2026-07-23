#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the deterministic ROS2 TF + occupancy-grid browser fixture.

The fixture is synthetic project test data covered by the repository license.
Reproduce it with:

    python3 -m pip install mcap==1.3.1
    python3 tests/wasm/fixtures/generate_ros2_tf_grid.py

Both object topics span the same four-second window.  This is intentional:
PlotJuggler prefers scalar bounds when an import contains scalars, and the ROS
OccupancyGrid parser exposes scalar metadata.  A single grid sample would
therefore collapse the transport even though /tf contains multiple objects.
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


OUTPUT = Path(__file__).with_name("ros2_tf_grid_real.mcap")
TF_SAMPLE_COUNT = 5
GRID_WIDTH = 200
GRID_HEIGHT = 200
TF_TOPIC = "/tf"
GRID_TOPIC = "/local_costmap/costmap"

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


def quaternion_from_yaw(yaw: float) -> tuple[float, float, float, float]:
    return (0.0, 0.0, math.sin(yaw / 2.0), math.cos(yaw / 2.0))


def transform(
    cdr: CdrWriter,
    timestamp_ns: int,
    parent: str,
    child: str,
    translation: tuple[float, float, float],
    rotation: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0),
) -> None:
    timestamp(cdr, timestamp_ns)
    cdr.string(parent)
    cdr.string(child)
    for value in translation + rotation:
        cdr.float64(value)


def tf_message_cdr(sequence: int) -> bytes:
    timestamp_ns = sequence * STEP_NS
    moving = float(sequence)
    transforms = (
        ("map", "odom", (0.75 * moving, 0.2 * moving, 0.0), quaternion_from_yaw(0.08 * moving)),
        ("odom", "base_footprint", (1.0, 0.0, 0.0), quaternion_from_yaw(0.03 * moving)),
        ("base_footprint", "base_link", (0.0, 0.0, 0.25), (0.0, 0.0, 0.0, 1.0)),
        ("base_link", "left_wheel_link", (0.0, 0.34, -0.12), (0.0, 0.0, 0.0, 1.0)),
        ("base_link", "right_wheel_link", (0.0, -0.34, -0.12), (0.0, 0.0, 0.0, 1.0)),
        ("base_link", "tower", (0.0, 0.0, 0.8), quaternion_from_yaw(-0.12 * moving)),
        ("tower", "laser", (0.18, 0.0, 0.1), (0.0, 0.0, 0.0, 1.0)),
    )
    cdr = CdrWriter()
    cdr.uint32(len(transforms))
    for parent, child, position, rotation in transforms:
        transform(cdr, timestamp_ns, parent, child, position, rotation)
    return bytes(cdr.data)


def occupancy_cells(sequence: int) -> bytes:
    cells = bytearray(GRID_WIDTH * GRID_HEIGHT)
    moving_column = 40 + (sequence * 20)
    for row in range(GRID_HEIGHT):
        for column in range(GRID_WIDTH):
            index = (row * GRID_WIDTH) + column
            if row in (0, GRID_HEIGHT - 1) or column in (0, GRID_WIDTH - 1):
                cells[index] = 100
            elif abs(column - moving_column) <= 1 and 25 <= row < 175:
                cells[index] = 100
            elif (row + column) % 37 == 0:
                cells[index] = 255  # int8 -1: unknown
    return bytes(cells)


def occupancy_grid_cdr(sequence: int) -> bytes:
    timestamp_ns = sequence * STEP_NS
    cdr = CdrWriter()
    timestamp(cdr, timestamp_ns)
    cdr.string("odom")
    timestamp(cdr, 0)  # map_load_time
    cdr.float32(0.05)
    cdr.uint32(GRID_WIDTH)
    cdr.uint32(GRID_HEIGHT)
    for value in (-5.0, -5.0, 0.0):
        cdr.float64(value)
    for value in (0.0, 0.0, 0.0, 1.0):
        cdr.float64(value)
    cdr.byte_sequence(occupancy_cells(sequence))
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
    writer.start(profile="ros2", library="pj-w13-scene3d-generator/1")
    tf_schema = writer.register_schema(name="tf2_msgs/msg/TFMessage", encoding="ros2msg", data=TF_SCHEMA)
    grid_schema = writer.register_schema(
        name="nav_msgs/msg/OccupancyGrid", encoding="ros2msg", data=GRID_SCHEMA
    )
    tf_channel = writer.register_channel(topic=TF_TOPIC, message_encoding="cdr", schema_id=tf_schema)
    grid_channel = writer.register_channel(topic=GRID_TOPIC, message_encoding="cdr", schema_id=grid_schema)
    for sequence in range(TF_SAMPLE_COUNT):
        timestamp_ns = sequence * STEP_NS
        writer.add_message(
            channel_id=tf_channel,
            log_time=timestamp_ns,
            publish_time=timestamp_ns,
            sequence=sequence,
            data=tf_message_cdr(sequence),
        )
        if sequence in (0, TF_SAMPLE_COUNT - 1):
            writer.add_message(
                channel_id=grid_channel,
                log_time=timestamp_ns,
                publish_time=timestamp_ns,
                sequence=sequence,
                data=occupancy_grid_cdr(sequence),
            )
    writer.finish()
    return output.getvalue()


def verify_fixture(payload: bytes) -> None:
    reader = make_reader(io.BytesIO(payload), validate_crcs=True)
    messages = list(reader.iter_messages(log_time_order=True))
    tf_messages = [entry for entry in messages if entry[1].topic == TF_TOPIC]
    grid_messages = [entry for entry in messages if entry[1].topic == GRID_TOPIC]
    if len(tf_messages) != TF_SAMPLE_COUNT or len(grid_messages) != 2:
        raise RuntimeError("fixture topic counts changed")
    expected_times = [sequence * STEP_NS for sequence in range(TF_SAMPLE_COUNT)]
    if [message.log_time for _, _, message in tf_messages] != expected_times:
        raise RuntimeError("TF timestamps changed")
    if [message.log_time for _, _, message in grid_messages] != [expected_times[0], expected_times[-1]]:
        raise RuntimeError("grid bounds no longer span the TF window")
    if any(message.publish_time != message.log_time for _, _, message in messages):
        raise RuntimeError("publish/log timestamp mismatch")


def main() -> None:
    if version("mcap") != "1.3.1":
        raise RuntimeError(f"mcap==1.3.1 is required, found {version('mcap')}")
    payload = build_fixture()
    verify_fixture(payload)
    OUTPUT.write_bytes(payload)
    digest = hashlib.sha256(payload).hexdigest()
    print(f"wrote {OUTPUT} ({len(payload)} bytes, sha256={digest})")


if __name__ == "__main__":
    main()
