#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Package real Cloudini/Draco blobs into a deterministic ROS2 browser fixture.

Reproduce with mcap==1.3.1 after explicitly building and running the native
``wasm_compressed_pointcloud_payload_generator`` target. The MCAP contains two
valid codec topics, a bad-then-valid recovery topic, and TF for physical render
placement. Repeated payloads make seek identity change without bloating git.
"""

import argparse
import hashlib
import io
import math
import struct
from importlib.metadata import version
from pathlib import Path

from mcap.reader import make_reader
from mcap.writer import CompressionType, IndexType, Writer
from scene3d_data_fixture_support import CdrWriter, STEP_NS, timestamp


OUTPUT = Path(__file__).with_name("ros2_compressed_pointcloud_real.mcap")
SAMPLE_COUNT = 3
CLOUDINI_TOPIC = "/cloudini_points"
DRACO_TOPIC = "/draco_points"
RECOVERY_TOPIC = "/zz_recovering_points"
TF_TOPIC = "/tf"

COMPRESSED_SCHEMA = b"""builtin_interfaces/Time timestamp
string frame_id
geometry_msgs/Pose pose
uint8[] data
string format
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
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


def compressed_cdr(timestamp_ns: int, blob: bytes, codec: str) -> bytes:
    cdr = CdrWriter()
    timestamp(cdr, timestamp_ns)
    cdr.string("compressed_lidar")
    for value in (0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0):
        cdr.float64(value)
    cdr.byte_sequence(blob)
    cdr.string(codec)
    return bytes(cdr.data)


def quaternion_from_yaw(yaw: float) -> tuple[float, float, float, float]:
    return (0.0, 0.0, math.sin(yaw / 2.0), math.cos(yaw / 2.0))


def append_transform(
    cdr: CdrWriter,
    timestamp_ns: int,
    parent: str,
    child: str,
    translation: tuple[float, float, float],
    rotation: tuple[float, float, float, float],
) -> None:
    timestamp(cdr, timestamp_ns)
    cdr.string(parent)
    cdr.string(child)
    for value in translation + rotation:
        cdr.float64(value)


def tf_message_cdr(sequence: int) -> bytes:
    timestamp_ns = sequence * STEP_NS
    cdr = CdrWriter()
    cdr.uint32(2)
    append_transform(
        cdr,
        timestamp_ns,
        "map",
        "base_link",
        (0.75 * sequence, -0.25 * sequence, 0.1),
        quaternion_from_yaw(0.2 * sequence),
    )
    append_transform(
        cdr,
        timestamp_ns,
        "base_link",
        "compressed_lidar",
        (0.4, 0.0, 0.8),
        (0.0, 0.0, 0.0, 1.0),
    )
    return bytes(cdr.data)


def build_fixture(cloudini: bytes, draco: bytes) -> bytes:
    output = io.BytesIO()
    writer = Writer(
        output,
        chunk_size=16 * 1024 * 1024,
        compression=CompressionType.ZSTD,
        index_types=IndexType.ALL,
        enable_crcs=True,
        enable_data_crcs=True,
    )
    writer.start(profile="ros2", library="pj-w19a-compressed-pointcloud-generator/1")
    compressed_schema = writer.register_schema(
        name="foxglove_msgs/msg/CompressedPointCloud", encoding="ros2msg", data=COMPRESSED_SCHEMA
    )
    tf_schema = writer.register_schema(name="tf2_msgs/msg/TFMessage", encoding="ros2msg", data=TF_SCHEMA)
    cloudini_channel = writer.register_channel(
        topic=CLOUDINI_TOPIC, message_encoding="cdr", schema_id=compressed_schema
    )
    draco_channel = writer.register_channel(topic=DRACO_TOPIC, message_encoding="cdr", schema_id=compressed_schema)
    recovery_channel = writer.register_channel(
        topic=RECOVERY_TOPIC, message_encoding="cdr", schema_id=compressed_schema
    )
    tf_channel = writer.register_channel(topic=TF_TOPIC, message_encoding="cdr", schema_id=tf_schema)
    for sequence in range(SAMPLE_COUNT):
        timestamp_ns = sequence * STEP_NS
        writer.add_message(
            channel_id=cloudini_channel,
            log_time=timestamp_ns,
            publish_time=timestamp_ns,
            sequence=sequence,
            data=compressed_cdr(timestamp_ns, cloudini, "cloudini"),
        )
        writer.add_message(
            channel_id=draco_channel,
            log_time=timestamp_ns,
            publish_time=timestamp_ns,
            sequence=sequence,
            data=compressed_cdr(timestamp_ns, draco, "draco"),
        )
        writer.add_message(
            channel_id=tf_channel,
            log_time=timestamp_ns,
            publish_time=timestamp_ns,
            sequence=sequence,
            data=tf_message_cdr(sequence),
        )
    writer.add_message(
        channel_id=recovery_channel,
        log_time=0,
        publish_time=0,
        sequence=0,
        data=compressed_cdr(0, b"not-a-self-describing-point-cloud", "unsupported_fixture_codec"),
    )
    writer.add_message(
        channel_id=recovery_channel,
        log_time=2 * STEP_NS,
        publish_time=2 * STEP_NS,
        sequence=1,
        data=compressed_cdr(2 * STEP_NS, cloudini, "cloudini"),
    )
    writer.finish()
    return output.getvalue()


def verify_fixture(payload: bytes) -> None:
    messages = list(make_reader(io.BytesIO(payload), validate_crcs=True).iter_messages(log_time_order=True))
    by_topic = {
        topic: [message for _, channel, message in messages if channel.topic == topic]
        for topic in (CLOUDINI_TOPIC, DRACO_TOPIC, RECOVERY_TOPIC, TF_TOPIC)
    }
    if len(by_topic[CLOUDINI_TOPIC]) != SAMPLE_COUNT or len(by_topic[DRACO_TOPIC]) != SAMPLE_COUNT:
        raise RuntimeError("valid compressed topic counts changed")
    if len(by_topic[RECOVERY_TOPIC]) != 2 or len(by_topic[TF_TOPIC]) != SAMPLE_COUNT:
        raise RuntimeError("recovery/TF topic counts changed")
    expected_times = [sequence * STEP_NS for sequence in range(SAMPLE_COUNT)]
    for topic in (CLOUDINI_TOPIC, DRACO_TOPIC, TF_TOPIC):
        if [message.log_time for message in by_topic[topic]] != expected_times:
            raise RuntimeError(f"timestamps changed for {topic}")
    if [message.log_time for message in by_topic[RECOVERY_TOPIC]] != [0, 2 * STEP_NS]:
        raise RuntimeError("recovery timestamps changed")
    if any(message.publish_time != message.log_time for _, _, message in messages):
        raise RuntimeError("publish/log timestamp mismatch")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("payload_directory", type=Path)
    args = parser.parse_args()
    if version("mcap") != "1.3.1":
        raise RuntimeError(f"mcap==1.3.1 is required, found {version('mcap')}")
    cloudini = (args.payload_directory / "cloudini.bin").read_bytes()
    draco = (args.payload_directory / "draco.bin").read_bytes()
    payload = build_fixture(cloudini, draco)
    verify_fixture(payload)
    OUTPUT.write_bytes(payload)
    print(
        f"wrote {OUTPUT} ({len(payload)} bytes, sha256={hashlib.sha256(payload).hexdigest()}, "
        f"cloudini={len(cloudini)}, draco={len(draco)})"
    )


if __name__ == "__main__":
    main()
