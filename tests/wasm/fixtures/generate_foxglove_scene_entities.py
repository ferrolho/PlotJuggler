#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the deterministic Foxglove SceneUpdate + TF browser fixture.

Reproduce with mcap==1.3.1 and protobuf==7.35.0. Two self-contained procedural
snapshots cover every Foxglove-representable marker family, while distinct
model entities prove windowed identity replay with an embedded five-map PBR GLB
and a CORS-enabled HTTP base-color GLB. A separate 50,001-cube sample crosses
the browser per-layer instance limit before marker geometry allocation.
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


OUTPUT = Path(__file__).with_name("foxglove_scene_entities_real.mcap")
STEP_NS = 1_000_000_000
MARKER_TOPIC = "/markers"
TF_A_TOPIC = "/tf/a"
TF_B_TOPIC = "/tf/b"
LIMIT_TOPIC = "/zz_too_many"
EMBEDDED_MODEL_FIXTURE = (
    Path(__file__).resolve().parents[3]
    / "pj_scene3D/widgets/tests/fixtures/meshes/embedded_pbr.glb"
)
REMOTE_MODEL_FIXTURE = (
    Path(__file__).resolve().parents[3]
    / "pj_scene3D/widgets/tests/fixtures/meshes/embedded_base.glb"
)
MODEL_URL = "https://models.plotjuggler.test/embedded_base.glb"


def add_field(message, name, number, field_type, *, type_name="", repeated=False, packed=False):
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
    if packed:
        field.options.packed = True


def foxglove_descriptor_set() -> bytes:
    """Build the reviewed Foxglove field subset consumed by the pinned parser."""
    field = descriptor_pb2.FieldDescriptorProto
    source = descriptor_pb2.FileDescriptorProto(
        name="foxglove/SceneEntitiesWasmAcceptance.proto",
        package="foxglove",
        syntax="proto3",
    )

    timestamp = source.message_type.add(name="Timestamp")
    add_field(timestamp, "sec", 1, field.TYPE_INT64)
    add_field(timestamp, "nsec", 2, field.TYPE_UINT32)
    duration = source.message_type.add(name="Duration")
    add_field(duration, "sec", 1, field.TYPE_INT64)
    add_field(duration, "nsec", 2, field.TYPE_UINT32)

    vector3 = source.message_type.add(name="Vector3")
    point3 = source.message_type.add(name="Point3")
    for message in (vector3, point3):
        for number, name in enumerate(("x", "y", "z"), start=1):
            add_field(message, name, number, field.TYPE_DOUBLE)

    quaternion = source.message_type.add(name="Quaternion")
    for number, name in enumerate(("x", "y", "z", "w"), start=1):
        add_field(quaternion, name, number, field.TYPE_DOUBLE)
    pose = source.message_type.add(name="Pose")
    add_field(pose, "position", 1, field.TYPE_MESSAGE, type_name=".foxglove.Vector3")
    add_field(pose, "orientation", 2, field.TYPE_MESSAGE, type_name=".foxglove.Quaternion")

    color = source.message_type.add(name="Color")
    for number, name in enumerate(("r", "g", "b", "a"), start=1):
        add_field(color, name, number, field.TYPE_DOUBLE)

    line_type = source.enum_type.add(name="LineType")
    for name, number in (("LINE_STRIP", 0), ("LINE_LOOP", 1), ("LINE_LIST", 2)):
        line_type.value.add(name=name, number=number)

    arrow = source.message_type.add(name="ArrowPrimitive")
    add_field(arrow, "pose", 1, field.TYPE_MESSAGE, type_name=".foxglove.Pose")
    for number, name in enumerate(
        ("shaft_length", "shaft_diameter", "head_length", "head_diameter"), start=2
    ):
        add_field(arrow, name, number, field.TYPE_DOUBLE)
    add_field(arrow, "color", 6, field.TYPE_MESSAGE, type_name=".foxglove.Color")

    for primitive_name in ("CubePrimitive", "SpherePrimitive"):
        primitive = source.message_type.add(name=primitive_name)
        add_field(primitive, "pose", 1, field.TYPE_MESSAGE, type_name=".foxglove.Pose")
        add_field(primitive, "size", 2, field.TYPE_MESSAGE, type_name=".foxglove.Vector3")
        add_field(primitive, "color", 3, field.TYPE_MESSAGE, type_name=".foxglove.Color")

    cylinder = source.message_type.add(name="CylinderPrimitive")
    add_field(cylinder, "pose", 1, field.TYPE_MESSAGE, type_name=".foxglove.Pose")
    add_field(cylinder, "size", 2, field.TYPE_MESSAGE, type_name=".foxglove.Vector3")
    add_field(cylinder, "bottom_scale", 3, field.TYPE_DOUBLE)
    add_field(cylinder, "top_scale", 4, field.TYPE_DOUBLE)
    add_field(cylinder, "color", 5, field.TYPE_MESSAGE, type_name=".foxglove.Color")

    line = source.message_type.add(name="LinePrimitive")
    add_field(line, "type", 1, field.TYPE_ENUM, type_name=".foxglove.LineType")
    add_field(line, "pose", 2, field.TYPE_MESSAGE, type_name=".foxglove.Pose")
    add_field(line, "thickness", 3, field.TYPE_DOUBLE)
    add_field(line, "scale_invariant", 4, field.TYPE_BOOL)
    add_field(line, "points", 5, field.TYPE_MESSAGE, type_name=".foxglove.Point3", repeated=True)
    add_field(line, "color", 6, field.TYPE_MESSAGE, type_name=".foxglove.Color")
    add_field(line, "colors", 7, field.TYPE_MESSAGE, type_name=".foxglove.Color", repeated=True)
    add_field(line, "indices", 8, field.TYPE_FIXED32, repeated=True, packed=True)

    triangle = source.message_type.add(name="TrianglePrimitive")
    add_field(triangle, "pose", 1, field.TYPE_MESSAGE, type_name=".foxglove.Pose")
    add_field(triangle, "points", 2, field.TYPE_MESSAGE, type_name=".foxglove.Point3", repeated=True)
    add_field(triangle, "color", 3, field.TYPE_MESSAGE, type_name=".foxglove.Color")
    add_field(triangle, "colors", 4, field.TYPE_MESSAGE, type_name=".foxglove.Color", repeated=True)
    add_field(triangle, "indices", 5, field.TYPE_FIXED32, repeated=True, packed=True)

    text = source.message_type.add(name="TextPrimitive")
    add_field(text, "pose", 1, field.TYPE_MESSAGE, type_name=".foxglove.Pose")
    add_field(text, "billboard", 2, field.TYPE_BOOL)
    add_field(text, "font_size", 3, field.TYPE_DOUBLE)
    add_field(text, "scale_invariant", 4, field.TYPE_BOOL)
    add_field(text, "color", 5, field.TYPE_MESSAGE, type_name=".foxglove.Color")
    add_field(text, "text", 6, field.TYPE_STRING)

    model = source.message_type.add(name="ModelPrimitive")
    add_field(model, "pose", 1, field.TYPE_MESSAGE, type_name=".foxglove.Pose")
    add_field(model, "scale", 2, field.TYPE_MESSAGE, type_name=".foxglove.Vector3")
    add_field(model, "color", 3, field.TYPE_MESSAGE, type_name=".foxglove.Color")
    add_field(model, "override_color", 4, field.TYPE_BOOL)
    add_field(model, "url", 5, field.TYPE_STRING)
    add_field(model, "media_type", 6, field.TYPE_STRING)
    add_field(model, "data", 7, field.TYPE_BYTES)

    entity = source.message_type.add(name="SceneEntity")
    add_field(entity, "timestamp", 1, field.TYPE_MESSAGE, type_name=".foxglove.Timestamp")
    add_field(entity, "frame_id", 2, field.TYPE_STRING)
    add_field(entity, "id", 3, field.TYPE_STRING)
    add_field(entity, "lifetime", 4, field.TYPE_MESSAGE, type_name=".foxglove.Duration")
    add_field(entity, "frame_locked", 5, field.TYPE_BOOL)
    for number, name, type_name in (
        (7, "arrows", "ArrowPrimitive"),
        (8, "cubes", "CubePrimitive"),
        (9, "spheres", "SpherePrimitive"),
        (10, "cylinders", "CylinderPrimitive"),
        (11, "lines", "LinePrimitive"),
        (12, "triangles", "TrianglePrimitive"),
        (13, "texts", "TextPrimitive"),
        (14, "models", "ModelPrimitive"),
    ):
        add_field(entity, name, number, field.TYPE_MESSAGE, type_name=f".foxglove.{type_name}", repeated=True)

    scene_update = source.message_type.add(name="SceneUpdate")
    add_field(scene_update, "entities", 2, field.TYPE_MESSAGE, type_name=".foxglove.SceneEntity", repeated=True)

    transform = source.message_type.add(name="FrameTransform")
    add_field(transform, "timestamp", 1, field.TYPE_MESSAGE, type_name=".foxglove.Timestamp")
    add_field(transform, "parent_frame_id", 2, field.TYPE_STRING)
    add_field(transform, "child_frame_id", 3, field.TYPE_STRING)
    add_field(transform, "translation", 4, field.TYPE_MESSAGE, type_name=".foxglove.Vector3")
    add_field(transform, "rotation", 5, field.TYPE_MESSAGE, type_name=".foxglove.Quaternion")

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


def double_field(number: int, value: float) -> bytes:
    return tag(number, 1) + struct.pack("<d", value)


def message_field(number: int, payload: bytes) -> bytes:
    return tag(number, 2) + encode_varint(len(payload)) + payload


def string_field(number: int, value: str) -> bytes:
    return message_field(number, value.encode("utf-8"))


def packed_fixed32_field(number: int, values: tuple[int, ...]) -> bytes:
    return message_field(number, struct.pack(f"<{len(values)}I", *values))


def timestamp(timestamp_ns: int) -> bytes:
    return varint_field(1, timestamp_ns // STEP_NS) + varint_field(2, timestamp_ns % STEP_NS)


def vector3(value: tuple[float, float, float]) -> bytes:
    return b"".join(double_field(number, component) for number, component in enumerate(value, start=1))


def quaternion_from_yaw(yaw: float) -> tuple[float, float, float, float]:
    return (0.0, 0.0, math.sin(yaw / 2.0), math.cos(yaw / 2.0))


def quaternion(value: tuple[float, float, float, float]) -> bytes:
    return b"".join(double_field(number, component) for number, component in enumerate(value, start=1))


def pose(position=(0.0, 0.0, 0.0), yaw=0.0) -> bytes:
    return message_field(1, vector3(position)) + message_field(2, quaternion(quaternion_from_yaw(yaw)))


def color(value: tuple[float, float, float, float]) -> bytes:
    return b"".join(double_field(number, component) for number, component in enumerate(value, start=1))


def box(position, size, tint, yaw=0.0) -> bytes:
    return message_field(1, pose(position, yaw)) + message_field(2, vector3(size)) + message_field(3, color(tint))


def cylinder(position, size, bottom_scale, top_scale, tint) -> bytes:
    return b"".join((
        message_field(1, pose(position)),
        message_field(2, vector3(size)),
        double_field(3, bottom_scale),
        double_field(4, top_scale),
        message_field(5, color(tint)),
    ))


def arrow(position, tint, yaw=0.0) -> bytes:
    return b"".join((
        message_field(1, pose(position, yaw)),
        double_field(2, 0.75),
        double_field(3, 0.12),
        double_field(4, 0.30),
        double_field(5, 0.24),
        message_field(6, color(tint)),
    ))


def point(value: tuple[float, float, float]) -> bytes:
    return vector3(value)


def indexed_line() -> bytes:
    points = ((-0.8, -0.8, 0.05), (0.8, -0.8, 0.05), (0.8, 0.8, 0.05), (-0.8, 0.8, 0.05))
    colors = ((1.0, 0.0, 0.0, 1.0), (0.0, 1.0, 0.0, 1.0), (0.0, 0.0, 1.0, 1.0), (1.0, 1.0, 0.0, 1.0))
    output = bytearray(varint_field(1, 1))  # LINE_LOOP
    output.extend(message_field(2, pose()))
    output.extend(double_field(3, 2.0))
    output.extend(varint_field(4, 1))
    for value in points:
        output.extend(message_field(5, point(value)))
    output.extend(message_field(6, color((1.0, 1.0, 1.0, 1.0))))
    for value in colors:
        output.extend(message_field(7, color(value)))
    output.extend(packed_fixed32_field(8, (0, 1, 2, 3)))
    return bytes(output)


def indexed_triangles() -> bytes:
    points = ((-0.7, -0.6, 0.0), (0.7, -0.6, 0.0), (0.7, 0.6, 0.0), (-0.7, 0.6, 0.0))
    colors = ((1.0, 0.2, 0.2, 1.0), (0.2, 1.0, 0.2, 1.0), (0.2, 0.2, 1.0, 1.0), (1.0, 0.2, 1.0, 1.0))
    output = bytearray(message_field(1, pose((0.0, 0.0, 0.15))))
    for value in points:
        output.extend(message_field(2, point(value)))
    output.extend(message_field(3, color((1.0, 1.0, 1.0, 1.0))))
    for value in colors:
        output.extend(message_field(4, color(value)))
    output.extend(packed_fixed32_field(5, (0, 1, 2, 0, 2, 3)))
    return bytes(output)


def text_primitive() -> bytes:
    return b"".join((
        message_field(1, pose((0.0, 0.0, 1.5))),
        varint_field(2, 1),
        double_field(3, 0.25),
        message_field(5, color((1.0, 1.0, 1.0, 1.0))),
        string_field(6, "native-pass-skips-text"),
    ))


def model_primitive(*, position, embedded: bool, override_color: bool) -> bytes:
    source = (
        message_field(6, b"model/gltf-binary")
        + message_field(7, EMBEDDED_MODEL_FIXTURE.read_bytes())
        if embedded
        else string_field(5, MODEL_URL) + string_field(6, "model/gltf-binary")
    )
    return b"".join((
        message_field(1, pose(position, 0.2 if embedded else -0.25)),
        message_field(2, vector3((0.8, 0.8, 0.8))),
        message_field(3, color((0.2, 0.85, 1.0, 0.9) if override_color else (1.0, 1.0, 1.0, 1.0))),
        varint_field(4, int(override_color)),
        source,
    ))


def scene_entity(timestamp_ns: int, frame_id: str, entity_id: str, primitives) -> bytes:
    output = bytearray(message_field(1, timestamp(timestamp_ns)))
    output.extend(string_field(2, frame_id))
    output.extend(string_field(3, entity_id))
    for field_number, payload in primitives:
        output.extend(message_field(field_number, payload))
    return bytes(output)


def scene_update(sequence: int) -> bytes:
    timestamp_ns = sequence * STEP_NS
    shift = 0.45 * sequence
    frame_a = [
        (8, box((-1.2 + shift, 0.0, 0.45), (0.75, 0.55, 0.9), (1.0, 0.15, 0.1, 1.0), 0.2)),
        (9, box((0.0 + shift, 0.0, 0.5), (0.75, 1.0, 0.65), (0.1, 0.3, 1.0, 1.0))),
        (7, arrow((0.7 + shift, -0.65, 0.35), (1.0, 0.75, 0.05, 1.0), 0.4)),
        (11, indexed_line()),
        (13, text_primitive()),
    ]
    frame_b = [
        (10, cylinder((-0.45, 0.75, 0.55), (0.7, 0.7, 1.1), 1.0, 0.25, (0.15, 1.0, 0.35, 1.0))),
        (8, box((0.55, 0.75, 0.25), (0.4, 0.4, 0.5), (0.9, 0.2, 0.9, 1.0))),
        (8, box((1.05, 0.75, 0.25), (0.4, 0.4, 0.5), (0.1, 0.9, 0.9, 1.0))),
        (12, indexed_triangles()),
    ]
    if sequence == 1:
        frame_b.append((8, box((1.55, 0.75, 0.25), (0.4, 0.4, 0.5), (1.0, 0.45, 0.05, 1.0))))
    unresolved = [(9, box((50.0, 50.0, 50.0), (2.0, 2.0, 2.0), (1.0, 1.0, 1.0, 1.0)))]
    entities = [
        message_field(2, scene_entity(timestamp_ns, "marker_a", "a", frame_a)),
        message_field(2, scene_entity(timestamp_ns, "marker_b", "b", frame_b)),
        message_field(2, scene_entity(timestamp_ns, "missing_frame", "missing", unresolved)),
    ]
    # The first entity is omitted from the second update but must remain live:
    # ModelPrimitive follows the native windowed identity/deletion/lifetime
    # track, unlike the latest-batch procedural marker pass.
    if sequence == 0:
        entities.append(message_field(2, scene_entity(
            timestamp_ns,
            "marker_a",
            "model_embedded",
            [(14, model_primitive(position=(-0.7, -0.15, 0.45), embedded=True, override_color=False))],
        )))
    else:
        entities.append(message_field(2, scene_entity(
            timestamp_ns,
            "marker_b",
            "model_remote",
            [(14, model_primitive(position=(0.65, -0.2, 0.5), embedded=False, override_color=True))],
        )))
    return b"".join(entities)


def oversized_scene_update() -> bytes:
    repeated_cube = message_field(
        8, box((0.0, 0.0, 0.0), (0.01, 0.01, 0.01), (1.0, 0.0, 0.0, 1.0))
    )
    entity = b"".join((
        message_field(1, timestamp(0)),
        string_field(2, "marker_a"),
        string_field(3, "over-limit"),
        repeated_cube * 50_001,
    ))
    return message_field(2, entity)


def frame_transform(timestamp_ns: int, child: str, translation, yaw: float) -> bytes:
    return b"".join((
        message_field(1, timestamp(timestamp_ns)),
        string_field(2, "map"),
        string_field(3, child),
        message_field(4, vector3(translation)),
        message_field(5, quaternion(quaternion_from_yaw(yaw))),
    ))


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
    writer.start(profile="", library="pj-w19f-scene-entities-generator/1")
    descriptor = foxglove_descriptor_set()
    scene_schema = writer.register_schema(name="foxglove.SceneUpdate", encoding="protobuf", data=descriptor)
    tf_schema = writer.register_schema(name="foxglove.FrameTransform", encoding="protobuf", data=descriptor)
    marker_channel = writer.register_channel(topic=MARKER_TOPIC, message_encoding="protobuf", schema_id=scene_schema)
    tf_a_channel = writer.register_channel(topic=TF_A_TOPIC, message_encoding="protobuf", schema_id=tf_schema)
    tf_b_channel = writer.register_channel(topic=TF_B_TOPIC, message_encoding="protobuf", schema_id=tf_schema)
    limit_channel = writer.register_channel(topic=LIMIT_TOPIC, message_encoding="protobuf", schema_id=scene_schema)

    writer.add_message(
        channel_id=limit_channel,
        log_time=0,
        publish_time=0,
        sequence=0,
        data=oversized_scene_update(),
    )
    for sequence in range(2):
        timestamp_ns = sequence * STEP_NS
        for channel_id, payload in (
            (marker_channel, scene_update(sequence)),
            (tf_a_channel, frame_transform(timestamp_ns, "marker_a", (0.15 * sequence, 0.0, 0.1), 0.1 * sequence)),
            (tf_b_channel, frame_transform(timestamp_ns, "marker_b", (-0.2, -0.1 * sequence, 0.2), -0.15 * sequence)),
        ):
            writer.add_message(
                channel_id=channel_id,
                log_time=timestamp_ns,
                publish_time=timestamp_ns,
                sequence=sequence,
                data=payload,
            )
    writer.finish()
    return output.getvalue()


def verify_fixture(payload: bytes) -> None:
    if not EMBEDDED_MODEL_FIXTURE.is_file():
        raise RuntimeError(f"missing model fixture: {EMBEDDED_MODEL_FIXTURE}")
    if hashlib.sha256(EMBEDDED_MODEL_FIXTURE.read_bytes()).hexdigest() != (
        "661371aac9890663f6a398031cd9ae9c75b57cfa40c6bb3a444a20be15e839ff"
    ):
        raise RuntimeError("embedded_pbr.glb changed; review and update the acceptance fixture")
    if not REMOTE_MODEL_FIXTURE.is_file():
        raise RuntimeError(f"missing model fixture: {REMOTE_MODEL_FIXTURE}")
    if hashlib.sha256(REMOTE_MODEL_FIXTURE.read_bytes()).hexdigest() != (
        "570a3dda79c199cf5970657ca26ad240276aea471c371c66630c5871e7813e98"
    ):
        raise RuntimeError("embedded_base.glb changed; review and update the acceptance fixture")
    messages = list(make_reader(io.BytesIO(payload), validate_crcs=True).iter_messages(log_time_order=True))
    expected = {MARKER_TOPIC: 2, TF_A_TOPIC: 2, TF_B_TOPIC: 2, LIMIT_TOPIC: 1}
    actual = {topic: sum(channel.topic == topic for _, channel, _ in messages) for topic in expected}
    if actual != expected:
        raise RuntimeError(f"fixture topic counts changed: {actual}")
    if any(message.publish_time != message.log_time for _, _, message in messages):
        raise RuntimeError("publish/log timestamp mismatch")
    if any(schema.encoding != "protobuf" or channel.message_encoding != "protobuf" for schema, channel, _ in messages):
        raise RuntimeError("fixture schema encoding changed")


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
