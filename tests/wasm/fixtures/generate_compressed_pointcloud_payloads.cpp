// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Generates the REAL Cloudini and Draco blobs embedded by
// generate_ros2_compressed_pointcloud.py. This native-only, explicit CMake
// helper is not part of the application or the default test build.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "cloudini_lib/cloudini.hpp"
#include "draco/compression/encode.h"
#include "draco/core/draco_types.h"
#include "draco/core/encoder_buffer.h"
#include "draco/metadata/geometry_metadata.h"
#include "draco/point_cloud/point_cloud_builder.h"

namespace {

constexpr std::uint32_t kWidth = 600;
constexpr std::uint32_t kHeight = 300;
constexpr std::uint32_t kUniqueRings = 10;
constexpr double kPi = 3.14159265358979323846;

struct Point {
  float x;
  float y;
  float z;
  float intensity;
};
static_assert(sizeof(Point) == 16);

std::vector<Point> makePoints() {
  std::vector<Point> points;
  points.reserve(static_cast<std::size_t>(kWidth) * kHeight);
  for (std::uint32_t row = 0; row < kHeight; ++row) {
    const float radial_fraction = static_cast<float>(row % kUniqueRings) / static_cast<float>(kUniqueRings - 1);
    const float radius = 1.25F + 3.75F * radial_fraction;
    for (std::uint32_t column = 0; column < kWidth; ++column) {
      const double angle = (2.0 * kPi * static_cast<double>(column)) / static_cast<double>(kWidth);
      points.push_back(
          Point{
              .x = radius * static_cast<float>(std::cos(angle)),
              .y = radius * static_cast<float>(std::sin(angle)),
              .z = 0.35F * static_cast<float>(std::sin(3.0 * angle)) + 0.6F * radial_fraction,
              .intensity = 0.7F * radial_fraction + 0.15F * (static_cast<float>(std::sin(angle)) + 1.0F),
          });
    }
  }
  return points;
}

std::vector<std::uint8_t> encodeCloudini(const std::vector<Point>& points) {
  Cloudini::EncodingInfo info;
  info.width = kWidth;
  info.height = kHeight;
  info.point_step = sizeof(Point);
  info.encoding_opt = Cloudini::EncodingOptions::NONE;
  info.compression_opt = Cloudini::CompressionOption::ZSTD;
  info.fields = {
      {"x", 0, Cloudini::FieldType::FLOAT32, std::nullopt},
      {"y", 4, Cloudini::FieldType::FLOAT32, std::nullopt},
      {"z", 8, Cloudini::FieldType::FLOAT32, std::nullopt},
      {"intensity", 12, Cloudini::FieldType::FLOAT32, std::nullopt},
  };
  Cloudini::PointcloudEncoder encoder(info);
  const Cloudini::ConstBufferView input(
      reinterpret_cast<const std::uint8_t*>(points.data()), points.size() * sizeof(Point));
  std::vector<std::uint8_t> output;
  encoder.encode(input, output);
  if (output.empty()) {
    throw std::runtime_error("Cloudini encoder returned an empty blob");
  }
  return output;
}

std::vector<std::uint8_t> encodeDraco(const std::vector<Point>& points) {
  draco::PointCloudBuilder builder;
  builder.Start(static_cast<draco::PointIndex::ValueType>(points.size()));
  const int position_attribute = builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
  const int intensity_attribute = builder.AddAttribute(draco::GeometryAttribute::GENERIC, 1, draco::DT_FLOAT32);
  for (std::size_t index = 0; index < points.size(); ++index) {
    const float xyz[3] = {points[index].x, points[index].y, points[index].z};
    const auto point_index = draco::PointIndex(static_cast<std::uint32_t>(index));
    builder.SetAttributeValueForPoint(position_attribute, point_index, xyz);
    builder.SetAttributeValueForPoint(intensity_attribute, point_index, &points[index].intensity);
  }
  auto metadata = std::make_unique<draco::AttributeMetadata>();
  metadata->AddEntryString("name", "intensity");
  builder.AddAttributeMetadata(intensity_attribute, std::move(metadata));
  std::unique_ptr<draco::PointCloud> cloud = builder.Finalize(/*deduplicate_points=*/false);
  if (!cloud) {
    throw std::runtime_error("Draco point-cloud builder failed");
  }
  draco::Encoder encoder;
  encoder.SetEncodingMethod(draco::POINT_CLOUD_KD_TREE_ENCODING);
  encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, 14);
  encoder.SetAttributeQuantization(draco::GeometryAttribute::GENERIC, 12);
  draco::EncoderBuffer buffer;
  const draco::Status status = encoder.EncodePointCloudToBuffer(*cloud, &buffer);
  if (!status.ok()) {
    throw std::runtime_error(std::string("Draco encoder failed: ") + status.error_msg());
  }
  return std::vector<std::uint8_t>(buffer.data(), buffer.data() + buffer.size());
}

void writeBlob(const std::filesystem::path& path, const std::vector<std::uint8_t>& blob) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  stream.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " OUTPUT_DIRECTORY\n";
    return 2;
  }
  try {
    const std::filesystem::path output_directory(argv[1]);
    std::filesystem::create_directories(output_directory);
    const std::vector<Point> points = makePoints();
    const std::vector<std::uint8_t> cloudini = encodeCloudini(points);
    const std::vector<std::uint8_t> draco = encodeDraco(points);
    writeBlob(output_directory / "cloudini.bin", cloudini);
    writeBlob(output_directory / "draco.bin", draco);
    std::cout << "wrote " << points.size() << " points: cloudini=" << cloudini.size()
              << " bytes, draco=" << draco.size() << " bytes\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
