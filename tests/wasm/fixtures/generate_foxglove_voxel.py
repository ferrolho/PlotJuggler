#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the deterministic Foxglove VoxelGrid browser fixture.

Reproduce with mcap==1.3.1 and protobuf==7.35.0:

    python3 tests/wasm/fixtures/generate_foxglove_voxel.py

The scalar topic has occupancy and float-cost fields in a padded cell, a
non-cubic lattice, and a rotated/transformed origin. The color topic exercises
the direct RGBA8 path. The final topic is one texel beyond the measured WebGL2
3D-texture dimension ceiling while remaining below the browser layer budget.
"""

import hashlib
import io
import math
import struct
from importlib.metadata import version
from pathlib import Path

from google.protobuf import descriptor_pb2
from mcap.reader import make_reader
from mcap.writer import CompressionType, IndexType, Writer


OUTPUT = Path(__file__).with_name("foxglove_voxel_real.mcap")
STEP_NS = 1_000_000_000
SCALAR_TOPIC = "/voxels/scalar"
COLOR_TOPIC = "/voxels/color"
LIMIT_TOPIC = "/zz_gpu_dimension_limit"
SCALAR_DIMS = (24, 18, 12)
COLOR_DIMS = (8, 7, 6)
LIMIT_DIMS = (2049, 1, 1)


def add_field(message, name, number, field_type, *, type_name="", repeated=False):
    field = message.field.add()
    field.name = name
    field.number = number
    field.type = field_type
    field.label = (
        descriptor_pb2.FieldDescriptorProto.LABEL_REPEATED
        if repeated
        else descriptor_pb2.FieldDescriptorProto.LABEL_OPTIONAL
    )
    if type_name:
        field.type_name = type_name


def foxglove_descriptor_set() -> bytes:
    field = descriptor_pb2.FieldDescriptorProto
    source = descriptor_pb2.FileDescriptorProto(
        name="foxglove/VoxelGridWasmAcceptance.proto",
        package="foxglove",
        syntax="proto3",
    )

    timestamp = source.message_type.add(name="Timestamp")
    add_field(timestamp, "sec", 1, field.TYPE_INT64)
    add_field(timestamp, "nsec", 2, field.TYPE_UINT32)

    vector3 = source.message_type.add(name="Vector3")
    for number, name in enumerate(("x", "y", "z"), start=1):
        add_field(vector3, name, number, field.TYPE_DOUBLE)

    quaternion = source.message_type.add(name="Quaternion")
    for number, name in enumerate(("x", "y", "z", "w"), start=1):
        add_field(quaternion, name, number, field.TYPE_DOUBLE)

    pose = source.message_type.add(name="Pose")
    add_field(pose, "position", 1, field.TYPE_MESSAGE, type_name=".foxglove.Vector3")
    add_field(pose, "orientation", 2, field.TYPE_MESSAGE, type_name=".foxglove.Quaternion")

    numeric_type = source.enum_type.add(name="NumericType")
    for name, number in (
        ("UNKNOWN", 0),
        ("UINT8", 1),
        ("INT8", 2),
        ("UINT16", 3),
        ("INT16", 4),
        ("UINT32", 5),
        ("INT32", 6),
        ("FLOAT32", 7),
        ("FLOAT64", 8),
    ):
        numeric_type.value.add(name=name, number=number)

    packed_field = source.message_type.add(name="PackedElementField")
    add_field(packed_field, "name", 1, field.TYPE_STRING)
    add_field(packed_field, "offset", 2, field.TYPE_FIXED32)
    add_field(packed_field, "type", 3, field.TYPE_ENUM, type_name=".foxglove.NumericType")

    voxel = source.message_type.add(name="VoxelGrid")
    add_field(voxel, "timestamp", 1, field.TYPE_MESSAGE, type_name=".foxglove.Timestamp")
    add_field(voxel, "frame_id", 2, field.TYPE_STRING)
    add_field(voxel, "pose", 3, field.TYPE_MESSAGE, type_name=".foxglove.Pose")
    add_field(voxel, "row_count", 4, field.TYPE_FIXED32)
    add_field(voxel, "column_count", 5, field.TYPE_FIXED32)
    add_field(voxel, "cell_size", 6, field.TYPE_MESSAGE, type_name=".foxglove.Vector3")
    add_field(voxel, "slice_stride", 7, field.TYPE_FIXED32)
    add_field(voxel, "row_stride", 8, field.TYPE_FIXED32)
    add_field(voxel, "cell_stride", 9, field.TYPE_FIXED32)
    add_field(
        voxel,
        "fields",
        10,
        field.TYPE_MESSAGE,
        type_name=".foxglove.PackedElementField",
        repeated=True,
    )
    add_field(voxel, "data", 11, field.TYPE_BYTES)

    descriptor_set = descriptor_pb2.FileDescriptorSet()
    descriptor_set.file.add().CopyFrom(source)
    return descriptor_set.SerializeToString(deterministic=True)


def encode_varint(value: int) -> bytes:
    output = bytearray()
    while value >= 0x80:
        output.append((value & 0x7F) | 0x80)
        value >>= 7
    output.append(value)
    return bytes(output)


def tag(number: int, wire_type: int) -> bytes:
    return encode_varint((number << 3) | wire_type)


def varint_field(number: int, value: int) -> bytes:
    return tag(number, 0) + encode_varint(value)


def fixed32_field(number: int, value: int) -> bytes:
    return tag(number, 5) + struct.pack("<I", value)


def double_field(number: int, value: float) -> bytes:
    return tag(number, 1) + struct.pack("<d", value)


def message_field(number: int, payload: bytes) -> bytes:
    return tag(number, 2) + encode_varint(len(payload)) + payload


def string_field(number: int, value: str) -> bytes:
    return message_field(number, value.encode("utf-8"))


def timestamp(timestamp_ns: int) -> bytes:
    return varint_field(1, timestamp_ns // STEP_NS) + varint_field(2, timestamp_ns % STEP_NS)


def vector3(x: float, y: float, z: float) -> bytes:
    return double_field(1, x) + double_field(2, y) + double_field(3, z)


def pose() -> bytes:
    yaw = math.radians(25.0)
    quaternion = b"".join(
        double_field(number, value)
        for number, value in enumerate((0.0, 0.0, math.sin(yaw / 2.0), math.cos(yaw / 2.0)), start=1)
    )
    return message_field(1, vector3(1.25, -0.75, 0.5)) + message_field(2, quaternion)


def packed_field(name: str, offset: int, numeric_type: int) -> bytes:
    return string_field(1, name) + fixed32_field(2, offset) + varint_field(3, numeric_type)


def scalar_data(sequence: int) -> bytes:
    columns, rows, slices = SCALAR_DIMS
    output = bytearray(columns * rows * slices * 12)
    index = 0
    for z in range(slices):
        for y in range(rows):
            for x in range(columns):
                occupied = 255 if ((x - 11) ** 2 + (y - 8) ** 2 + (z - 5) ** 2) < 36 else 0
                if sequence == 2 and x == y and z < 12:
                    occupied = 180
                cost = ((x * 7 + y * 11 + z * 13 + sequence * 17) % 251) / 10.0
                output[index] = occupied
                struct.pack_into("<f", output, index + 4, cost)
                visible = occupied != 0
                output[index + 8 : index + 12] = bytes(
                    ((x * 23 + sequence * 19) & 0xFF, (y * 29) & 0xFF, (z * 41) & 0xFF, 255 if visible else 0)
                )
                index += 12
    return bytes(output)


def color_data(sequence: int) -> bytes:
    columns, rows, slices = COLOR_DIMS
    output = bytearray(columns * rows * slices * 4)
    index = 0
    for z in range(slices):
        for y in range(rows):
            for x in range(columns):
                visible = (x + y + z + sequence) % 3 == 0
                output[index : index + 4] = bytes(
                    ((x * 31 + sequence * 17) & 0xFF, (y * 39) & 0xFF, (z * 47) & 0xFF, 255 if visible else 0)
                )
                index += 4
    return bytes(output)


def voxel_grid(
    timestamp_ns: int,
    dimensions: tuple[int, int, int],
    cell_size: tuple[float, float, float],
    cell_stride: int,
    fields: tuple[tuple[str, int, int], ...],
    data: bytes,
) -> bytes:
    columns, rows, _ = dimensions
    row_stride = columns * cell_stride
    slice_stride = rows * row_stride
    payload = bytearray()
    payload.extend(message_field(1, timestamp(timestamp_ns)))
    payload.extend(string_field(2, "map"))
    payload.extend(message_field(3, pose()))
    payload.extend(fixed32_field(4, rows))
    payload.extend(fixed32_field(5, columns))
    payload.extend(message_field(6, vector3(*cell_size)))
    payload.extend(fixed32_field(7, slice_stride))
    payload.extend(fixed32_field(8, row_stride))
    payload.extend(fixed32_field(9, cell_stride))
    for name, offset, numeric_type in fields:
        payload.extend(message_field(10, packed_field(name, offset, numeric_type)))
    payload.extend(message_field(11, data))
    return bytes(payload)


def build_fixture() -> bytes:
    output = io.BytesIO()
    writer = Writer(
        output,
        compression=CompressionType.ZSTD,
        index_types=IndexType.ALL,
        enable_crcs=True,
        enable_data_crcs=True,
    )
    writer.start(profile="", library="pj-w17-voxel-generator/1")
    schema_id = writer.register_schema(
        name="foxglove.VoxelGrid",
        encoding="protobuf",
        data=foxglove_descriptor_set(),
    )
    scalar_channel = writer.register_channel(
        topic=SCALAR_TOPIC, message_encoding="protobuf", schema_id=schema_id
    )
    color_channel = writer.register_channel(
        topic=COLOR_TOPIC, message_encoding="protobuf", schema_id=schema_id
    )
    limit_channel = writer.register_channel(
        topic=LIMIT_TOPIC, message_encoding="protobuf", schema_id=schema_id
    )

    for sequence in range(3):
        timestamp_ns = sequence * STEP_NS
        scalar = voxel_grid(
            timestamp_ns,
            SCALAR_DIMS,
            (0.08, 0.12, 0.18),
            12,
            (("occupancy", 0, 1), ("cost", 4, 7), ("color", 8, 5)),
            scalar_data(sequence),
        )
        color = voxel_grid(
            timestamp_ns,
            COLOR_DIMS,
            (0.16, 0.11, 0.09),
            4,
            (("color", 0, 5),),
            color_data(sequence),
        )
        for channel_id, data in ((scalar_channel, scalar), (color_channel, color)):
            writer.add_message(
                channel_id=channel_id,
                log_time=timestamp_ns,
                publish_time=timestamp_ns,
                sequence=sequence,
                data=data,
            )

    limit_columns, limit_rows, _ = LIMIT_DIMS
    limit_data = bytes(limit_columns * limit_rows)
    writer.add_message(
        channel_id=limit_channel,
        log_time=0,
        publish_time=0,
        sequence=0,
        data=voxel_grid(0, LIMIT_DIMS, (0.01, 0.01, 0.01), 1, (("occupancy", 0, 1),), limit_data),
    )
    writer.finish()
    return output.getvalue()


def verify_fixture(payload: bytes) -> None:
    messages = list(make_reader(io.BytesIO(payload), validate_crcs=True).iter_messages(log_time_order=True))
    by_topic = {
        topic: [entry for entry in messages if entry[1].topic == topic]
        for topic in (SCALAR_TOPIC, COLOR_TOPIC, LIMIT_TOPIC)
    }
    expected = {SCALAR_TOPIC: 3, COLOR_TOPIC: 3, LIMIT_TOPIC: 1}
    if {topic: len(entries) for topic, entries in by_topic.items()} != expected:
        raise RuntimeError("fixture topic counts changed")
    if any(
        schema.name != "foxglove.VoxelGrid"
        or schema.encoding != "protobuf"
        or channel.message_encoding != "protobuf"
        for schema, channel, _ in messages
    ):
        raise RuntimeError("fixture schema identity changed")
    if any(message.publish_time != message.log_time for _, _, message in messages):
        raise RuntimeError("publish/log timestamp mismatch")
    if LIMIT_DIMS != (2049, 1, 1) or LIMIT_DIMS[0] * LIMIT_DIMS[1] * LIMIT_DIMS[2] >= 4 * 1024 * 1024:
        raise RuntimeError("limit sample must isolate the measured 2048-texel GPU dimension ceiling")


def main() -> None:
    required = {"mcap": "1.3.1", "protobuf": "7.35.0"}
    for package, expected in required.items():
        actual = version(package)
        if actual != expected:
            raise RuntimeError(f"{package}=={expected} is required, found {actual}")
    payload = build_fixture()
    verify_fixture(payload)
    OUTPUT.write_bytes(payload)
    print(f"wrote {OUTPUT} ({len(payload)} bytes, sha256={hashlib.sha256(payload).hexdigest()})")


if __name__ == "__main__":
    main()
