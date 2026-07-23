#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the deterministic ROS2 TF + robot_description browser fixture.

The fixture is synthetic project test data covered by the repository license.
Reproduce it with:

    python3 -m pip install mcap==1.3.1
    python3 tests/wasm/fixtures/generate_ros2_robot_description.py
"""

import hashlib
import io
from importlib.metadata import version
from pathlib import Path

from mcap.reader import make_reader
from mcap.writer import CompressionType, IndexType, Writer

from generate_ros2_tf_grid import (
    CdrWriter,
    STEP_NS,
    TF_SAMPLE_COUNT,
    TF_SCHEMA,
    TF_TOPIC,
    tf_message_cdr,
)


OUTPUT = Path(__file__).with_name("ros2_robot_description_real.mcap")
URDF = Path(__file__).with_name("browser_robot.urdf")
ROBOT_TOPIC = "/robot_description"
ROBOT_SCHEMA = b"string data\n"


def robot_description_cdr() -> bytes:
    cdr = CdrWriter()
    cdr.string(URDF.read_text(encoding="utf-8"))
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
    writer.start(profile="ros2", library="pj-w19h-robot-description-generator/1")
    tf_schema = writer.register_schema(name="tf2_msgs/msg/TFMessage", encoding="ros2msg", data=TF_SCHEMA)
    robot_schema = writer.register_schema(name="std_msgs/msg/String", encoding="ros2msg", data=ROBOT_SCHEMA)
    tf_channel = writer.register_channel(topic=TF_TOPIC, message_encoding="cdr", schema_id=tf_schema)
    robot_channel = writer.register_channel(topic=ROBOT_TOPIC, message_encoding="cdr", schema_id=robot_schema)
    for sequence in range(TF_SAMPLE_COUNT):
        timestamp_ns = sequence * STEP_NS
        writer.add_message(
            channel_id=tf_channel,
            log_time=timestamp_ns,
            publish_time=timestamp_ns,
            sequence=sequence,
            data=tf_message_cdr(sequence),
        )
        if sequence == 0:
            writer.add_message(
                channel_id=robot_channel,
                log_time=timestamp_ns,
                publish_time=timestamp_ns,
                sequence=sequence,
                data=robot_description_cdr(),
            )
    writer.finish()
    return output.getvalue()


def verify_fixture(payload: bytes) -> None:
    reader = make_reader(io.BytesIO(payload), validate_crcs=True)
    messages = list(reader.iter_messages(log_time_order=True))
    tf_messages = [entry for entry in messages if entry[1].topic == TF_TOPIC]
    robot_messages = [entry for entry in messages if entry[1].topic == ROBOT_TOPIC]
    if len(tf_messages) != TF_SAMPLE_COUNT or len(robot_messages) != 1:
        raise RuntimeError("fixture topic counts changed")
    expected_times = [sequence * STEP_NS for sequence in range(TF_SAMPLE_COUNT)]
    if [message.log_time for _, _, message in tf_messages] != expected_times:
        raise RuntimeError("TF timestamps changed")
    if robot_messages[0][2].log_time != 0 or URDF.read_bytes().strip() not in robot_messages[0][2].data:
        raise RuntimeError("robot_description payload changed")
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
