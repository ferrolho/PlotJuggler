#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Generate the deterministic Foxglove JPEG/annotation Scene2D fixture.

Reproduce it with:

    python3 -m pip install mcap==1.3.1 Pillow==12.1.1 protobuf==7.35.0
    python3 tests/wasm/fixtures/generate_foxglove_scene2d.py

The MCAP is self-describing: each channel carries a protobuf FileDescriptorSet
and uses the official Foxglove schema name. Pillow is pinned because the JPEG
bytes themselves are part of the acceptance contract; the browser subsequently
decodes them through Scene2D's pinned libjpeg-turbo build.
"""

import base64
import hashlib
import io
import struct
import textwrap
from importlib.metadata import version
from pathlib import Path

from google.protobuf import descriptor_pb2
from mcap.reader import make_reader
from mcap.writer import CompressionType, IndexType, Writer
from PIL import Image


OUTPUT = Path(__file__).with_name("foxglove_scene2d.mcap.b64")
WIDTH = 96
HEIGHT = 72
STEP_NS = 1_000_000_000
TOPIC_A = "/foxglove/jpeg/a"
TOPIC_B = "/foxglove/jpeg/b"
ANNOTATION_TOPIC = "/foxglove/annotations/a"
IMAGE_COLORS = (
    ((255, 0, 0), (0, 0, 255)),
    ((0, 255, 0), (255, 0, 255)),
)


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
    """Build the reviewed Foxglove field subset consumed by the pinned parser."""
    field = descriptor_pb2.FieldDescriptorProto
    source = descriptor_pb2.FileDescriptorProto(
        name="foxglove/Scene2DWasmAcceptance.proto",
        package="foxglove",
        syntax="proto3",
    )

    timestamp = source.message_type.add(name="Timestamp")
    add_field(timestamp, "sec", 1, field.TYPE_INT64)
    add_field(timestamp, "nsec", 2, field.TYPE_UINT32)

    color = source.message_type.add(name="Color")
    for number, name in enumerate(("r", "g", "b", "a"), start=1):
        add_field(color, name, number, field.TYPE_DOUBLE)

    point = source.message_type.add(name="Point2")
    add_field(point, "x", 1, field.TYPE_DOUBLE)
    add_field(point, "y", 2, field.TYPE_DOUBLE)

    points_type = source.enum_type.add(name="PointsAnnotationType")
    for name, number in (
        ("UNKNOWN", 0),
        ("POINTS", 1),
        ("LINE_LOOP", 2),
        ("LINE_STRIP", 3),
        ("LINE_LIST", 4),
    ):
        points_type.value.add(name=name, number=number)

    compressed = source.message_type.add(name="CompressedImage")
    add_field(compressed, "timestamp", 1, field.TYPE_MESSAGE, type_name=".foxglove.Timestamp")
    add_field(compressed, "data", 2, field.TYPE_BYTES)
    add_field(compressed, "format", 3, field.TYPE_STRING)
    add_field(compressed, "frame_id", 4, field.TYPE_STRING)

    circle = source.message_type.add(name="CircleAnnotation")
    add_field(circle, "timestamp", 1, field.TYPE_MESSAGE, type_name=".foxglove.Timestamp")
    add_field(circle, "position", 2, field.TYPE_MESSAGE, type_name=".foxglove.Point2")
    add_field(circle, "diameter", 3, field.TYPE_DOUBLE)
    add_field(circle, "thickness", 4, field.TYPE_DOUBLE)
    add_field(circle, "fill_color", 5, field.TYPE_MESSAGE, type_name=".foxglove.Color")
    add_field(circle, "outline_color", 6, field.TYPE_MESSAGE, type_name=".foxglove.Color")

    points = source.message_type.add(name="PointsAnnotation")
    add_field(points, "timestamp", 1, field.TYPE_MESSAGE, type_name=".foxglove.Timestamp")
    add_field(points, "type", 2, field.TYPE_ENUM, type_name=".foxglove.PointsAnnotationType")
    add_field(points, "points", 3, field.TYPE_MESSAGE, type_name=".foxglove.Point2", repeated=True)
    add_field(points, "outline_color", 4, field.TYPE_MESSAGE, type_name=".foxglove.Color")
    add_field(points, "outline_colors", 5, field.TYPE_MESSAGE, type_name=".foxglove.Color", repeated=True)
    add_field(points, "fill_color", 6, field.TYPE_MESSAGE, type_name=".foxglove.Color")
    add_field(points, "thickness", 7, field.TYPE_DOUBLE)

    text = source.message_type.add(name="TextAnnotation")
    add_field(text, "timestamp", 1, field.TYPE_MESSAGE, type_name=".foxglove.Timestamp")
    add_field(text, "position", 2, field.TYPE_MESSAGE, type_name=".foxglove.Point2")
    add_field(text, "text", 3, field.TYPE_STRING)
    add_field(text, "font_size", 4, field.TYPE_DOUBLE)
    add_field(text, "text_color", 5, field.TYPE_MESSAGE, type_name=".foxglove.Color")
    add_field(text, "background_color", 6, field.TYPE_MESSAGE, type_name=".foxglove.Color")

    annotations = source.message_type.add(name="ImageAnnotations")
    add_field(annotations, "circles", 1, field.TYPE_MESSAGE, type_name=".foxglove.CircleAnnotation", repeated=True)
    add_field(annotations, "points", 2, field.TYPE_MESSAGE, type_name=".foxglove.PointsAnnotation", repeated=True)
    add_field(annotations, "texts", 3, field.TYPE_MESSAGE, type_name=".foxglove.TextAnnotation", repeated=True)

    descriptor_set = descriptor_pb2.FileDescriptorSet()
    descriptor_set.file.add().CopyFrom(source)
    return descriptor_set.SerializeToString(deterministic=True)


def encode_varint(value: int) -> bytes:
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def tag(number: int, wire_type: int) -> bytes:
    return encode_varint((number << 3) | wire_type)


def message_field(number: int, payload: bytes) -> bytes:
    return tag(number, 2) + encode_varint(len(payload)) + payload


def string_field(number: int, value: str) -> bytes:
    return message_field(number, value.encode("utf-8"))


def varint_field(number: int, value: int) -> bytes:
    return tag(number, 0) + encode_varint(value)


def double_field(number: int, value: float) -> bytes:
    return tag(number, 1) + struct.pack("<d", value)


def timestamp(timestamp_ns: int) -> bytes:
    return varint_field(1, timestamp_ns // STEP_NS) + varint_field(2, timestamp_ns % STEP_NS)


def point(x: float, y: float) -> bytes:
    return double_field(1, x) + double_field(2, y)


def color(red: float, green: float, blue: float, alpha: float) -> bytes:
    return b"".join(
        double_field(number, value)
        for number, value in enumerate((red, green, blue, alpha), start=1)
    )


def jpeg_frame(left: tuple[int, int, int], right: tuple[int, int, int]) -> bytes:
    image = Image.new("RGB", (WIDTH, HEIGHT), left)
    image.paste(right, (WIDTH // 2, 0, WIDTH, HEIGHT))
    output = io.BytesIO()
    image.save(output, format="JPEG", quality=100, subsampling=0, optimize=False, progressive=False)
    return output.getvalue()


def compressed_image(timestamp_ns: int, jpeg: bytes) -> bytes:
    return b"".join(
        (
            message_field(1, timestamp(timestamp_ns)),
            message_field(2, jpeg),
            string_field(3, "jpeg"),
            string_field(4, "camera_optical"),
        )
    )


def image_annotations(timestamp_ns: int) -> bytes:
    outline = color(1.0, 1.0, 1.0, 1.0)
    transparent = color(0.0, 0.0, 0.0, 0.0)
    rectangle = bytearray()
    rectangle.extend(message_field(1, timestamp(timestamp_ns)))
    rectangle.extend(varint_field(2, 2))  # LINE_LOOP
    for x, y in ((12.0, 10.0), (84.0, 10.0), (84.0, 62.0), (12.0, 62.0)):
        rectangle.extend(message_field(3, point(x, y)))
    rectangle.extend(message_field(4, outline))
    rectangle.extend(message_field(6, transparent))
    rectangle.extend(double_field(7, 3.0))
    return message_field(2, bytes(rectangle))


def build_fixture() -> bytes:
    output = io.BytesIO()
    writer = Writer(
        output,
        compression=CompressionType.NONE,
        index_types=IndexType.ALL,
        enable_crcs=True,
        enable_data_crcs=True,
    )
    writer.start(profile="", library="pj-w11-foxglove-generator/1")
    descriptor = foxglove_descriptor_set()
    image_schema = writer.register_schema(
        name="foxglove.CompressedImage",
        encoding="protobuf",
        data=descriptor,
    )
    annotation_schema = writer.register_schema(
        name="foxglove.ImageAnnotations",
        encoding="protobuf",
        data=descriptor,
    )
    image_a = writer.register_channel(topic=TOPIC_A, message_encoding="protobuf", schema_id=image_schema)
    image_b = writer.register_channel(topic=TOPIC_B, message_encoding="protobuf", schema_id=image_schema)
    annotations = writer.register_channel(
        topic=ANNOTATION_TOPIC,
        message_encoding="protobuf",
        schema_id=annotation_schema,
    )
    frames = tuple(jpeg_frame(*colors) for colors in IMAGE_COLORS)
    for sequence in range(3):
        timestamp_ns = sequence * STEP_NS
        for channel_id, payload in (
            (image_a, compressed_image(timestamp_ns, frames[0])),
            (image_b, compressed_image(timestamp_ns, frames[1])),
            (annotations, image_annotations(timestamp_ns)),
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
    messages = list(make_reader(io.BytesIO(payload), validate_crcs=True).iter_messages(log_time_order=True))
    expected_topics = {TOPIC_A, TOPIC_B, ANNOTATION_TOPIC}
    if len(messages) != 9 or {channel.topic for _, channel, _ in messages} != expected_topics:
        raise RuntimeError("fixture topic/message matrix changed")
    for schema, channel, message in messages:
        expected_schema = "foxglove.ImageAnnotations" if channel.topic == ANNOTATION_TOPIC else "foxglove.CompressedImage"
        if schema.name != expected_schema or schema.encoding != "protobuf" or channel.message_encoding != "protobuf":
            raise RuntimeError("fixture schema identity changed")
        if message.log_time != message.publish_time or message.log_time != message.sequence * STEP_NS:
            raise RuntimeError("fixture timestamps do not match the declared sequence")


def main() -> None:
    required = {"mcap": "1.3.1", "Pillow": "12.1.1", "protobuf": "7.35.0"}
    for package, expected in required.items():
        actual = version(package)
        if actual != expected:
            raise RuntimeError(f"{package}=={expected} is required, found {actual}")
    payload = build_fixture()
    verify_fixture(payload)
    encoded = base64.b64encode(payload).decode("ascii")
    OUTPUT.write_text("\n".join(textwrap.wrap(encoded, width=76)) + "\n", encoding="ascii")
    print(f"wrote {OUTPUT} ({len(payload)} bytes, sha256={hashlib.sha256(payload).hexdigest()})")


if __name__ == "__main__":
    main()
