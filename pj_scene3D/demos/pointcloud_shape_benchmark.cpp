// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Offscreen-GL render benchmark for the three point-cloud shapes (point, sphere,
// cube), measuring the PRODUCTION PointcloudRenderPass — same shaders, same VBO
// layout, same MSAA sample count as the scene HDR chain.
//
// It exists because the cube shape is structurally far more expensive than the
// sprite shapes (one instanced 12-triangle solid per point vs one point sprite),
// so any change to the cube path needs a number, not an assertion. Two camera
// framings are timed because they stress opposite ends of the pipeline:
//
//   wide  — the whole cloud in view, cubes projecting to about a pixel. Vertex +
//           primitive-setup bound; this is the "large cloud" case users complain
//           about.
//   close — camera inside the cloud, cubes covering many pixels each. Fill bound.
//
// Wall time (glFinish-bracketed) is the headline number: it is the only metric a
// software rasterizer reports honestly. GL_TIME_ELAPSED is printed alongside when
// the driver supports it.
//
// Run headless with QT_QPA_PLATFORM=offscreen. Prints GL_RENDERER so software vs
// hardware GL is never ambiguous. No app, no user interaction.
//
// Usage: pointcloud_shape_benchmark [--lod PX] [N1 N2 ...]
//   N...     point counts to time (default sweep)
//   --lod PX cube LOD threshold for the "cube+LOD" column (default
//            kDefaultCubeLodThresholdPx); sweep it to re-derive that default

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QString>
#include <QStringList>
#include <QSurfaceFormat>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/span.hpp"
#include "pj_base/time.hpp"
#include "pj_scene3d_core/pointcloud_convert.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"
#include "pj_scene3d_widgets/render_pass.h"

namespace {

using pj::scene3d::AttribLayout;
using pj::scene3d::checkFastPath;
using pj::scene3d::FastCloudData;
using pj::scene3d::FrameContext;
using pj::scene3d::PointcloudRenderPass;
using pj::scene3d::TransformBuffer;
using pj::scene3d::ViewParams;
using PJ::sdk::PointCloud;
using PJ::sdk::PointField;
using DT = PointField::Datatype;

using Clock = std::chrono::steady_clock;

// Matches the scene HDR chain (kDefaultMsaaSamples) — MSAA multiplies the cost of
// the many tiny triangles a cube cloud emits, so benchmarking without it would
// flatter the cube path.
constexpr int kSamples = 4;
constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr uint32_t kStride = 16;  // xyz float32 + intensity float32
constexpr float kCubeSizeMeters = 0.05f;
constexpr int kWarmupFrames = 3;
constexpr int kTimedFrames = 10;

// Lidar-ish extent: a wide, shallow slab, so the "wide" camera sees the whole
// cloud at a realistic sub-pixel-per-cube density.
constexpr float kHalfExtentXY = 20.0f;
constexpr float kHalfExtentZ = 4.0f;

double medianMs(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  return samples.empty() ? 0.0 : samples[samples.size() / 2];
}

struct SyntheticCloud {
  std::vector<uint8_t> bytes;
  PointCloud cloud;

  explicit SyntheticCloud(std::size_t n) : bytes(n * kStride, 0) {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> xy(-kHalfExtentXY, kHalfExtentXY);
    std::uniform_real_distribution<float> z(-kHalfExtentZ, kHalfExtentZ);
    std::uniform_real_distribution<float> intensity(0.0f, 1.0f);
    for (std::size_t i = 0; i < n; ++i) {
      const std::array<float, 4> point = {xy(rng), xy(rng), z(rng), intensity(rng)};
      std::memcpy(bytes.data() + i * kStride, point.data(), sizeof(point));
    }
    cloud.width = static_cast<uint32_t>(n);
    cloud.height = 1;
    cloud.point_step = kStride;
    cloud.row_step = static_cast<uint32_t>(n) * kStride;
    cloud.is_bigendian = false;
    cloud.frame_id = "lidar";
    cloud.fields = {
        {"x", 0, DT::kFloat32, 1},
        {"y", 4, DT::kFloat32, 1},
        {"z", 8, DT::kFloat32, 1},
        {"intensity", 12, DT::kFloat32, 1}};
    cloud.data = PJ::Span<const uint8_t>(bytes.data(), bytes.size());
  }

  // The slab's exact extent — the points are drawn uniformly from it, so this is what
  // the layer's bounds scan would report.
  [[nodiscard]] static pj::scene3d::AABB bounds() {
    return pj::scene3d::AABB{
        glm::vec3(-kHalfExtentXY, -kHalfExtentXY, -kHalfExtentZ), glm::vec3(kHalfExtentXY, kHalfExtentXY, kHalfExtentZ),
        true};
  }
};

struct Framing {
  const char* name;
  glm::vec3 eye;
  glm::vec3 center;
};

// Wide: the whole slab in frame, each cube about a pixel across (vertex/setup
// bound). Close: inside the cloud, cubes covering real screen area (fill bound).
const std::array<Framing, 2> kFramings = {{
    {"wide", {0.0f, -45.0f, 25.0f}, {0.0f, 0.0f, 0.0f}},
    {"close", {0.0f, -2.0f, 0.6f}, {0.0f, 3.0f, 0.0f}},
}};

struct Timing {
  double wall_ms{0.0};
  double gpu_ms{0.0};  // 0 when the driver has no usable timer query
};

struct ShapeCase {
  const char* name;
  PointcloudRenderPass::Shape shape;
  // Cube only. 0 forces the hexagon fan at every projected size, which is what makes
  // the last two columns a same-binary A/B of the screen-space LOD.
  float lod_threshold_px;
};

// The LOD threshold is a parameter rather than baked in, so --lod can sweep it without
// mutating global state between the parse and the run.
std::array<ShapeCase, 4> shapeCases(float cube_lod_threshold_px) {
  return {{
      {"point", PointcloudRenderPass::Shape::kPoint, 0.0f},
      {"sphere", PointcloudRenderPass::Shape::kSphere, 0.0f},
      {"cube", PointcloudRenderPass::Shape::kCube, 0.0f},
      {"cube+LOD", PointcloudRenderPass::Shape::kCube, cube_lod_threshold_px},
  }};
}

ViewParams makeViewParams(const Framing& framing) {
  ViewParams vp;
  vp.view = glm::lookAt(framing.eye, framing.center, glm::vec3(0.0f, 0.0f, 1.0f));
  vp.proj =
      glm::perspective(glm::radians(60.0f), static_cast<float>(kWidth) / static_cast<float>(kHeight), 0.1f, 500.0f);
  vp.viewport_width_px = kWidth;
  vp.viewport_height_px = kHeight;
  vp.device_width_px = kWidth;
  vp.device_height_px = kHeight;
  vp.camera_pos_world = framing.eye;
  return vp;
}

// One shape x one framing. The pass is rebuilt per case so no state (uploaded
// VBO, cube VAO bindings) leaks between measurements; the upload happens during
// warmup, so the timed frames measure drawing only.
Timing timeCase(
    QOpenGLFunctions_4_5_Core* gl, const SyntheticCloud& cloud, const AttribLayout& layout, const ShapeCase& shape_case,
    const Framing& framing) {
  PointcloudRenderPass pass;
  pass.setShape(shape_case.shape);
  pass.setCubeLodThresholdPx(shape_case.lod_threshold_px);
  pass.setColorType(PointcloudRenderPass::ColorType::kField);
  pass.setColormapRange(0.0f, 1.0f);
  pass.setSizeMeters(kCubeSizeMeters);
  pass.initializeGL();
  pass.setActiveFastCloud(FastCloudData{cloud.cloud, cloud.cloud.width, layout});
  // The layer always publishes an extent, and the LOD's whole-pass skip is driven by
  // it — measuring without one would hide the case the optimization is aimed at.
  // setActiveFastCloud clears it, so this has to follow the swap.
  pass.setGeometryBounds(SyntheticCloud::bounds());

  const TransformBuffer tf(TransformBuffer::kKeepAll);
  const std::string fixed = "lidar";  // == cloud frame → identity model
  const FrameContext frame_ctx{tf, fixed, PJ::fromRaw(0)};
  const ViewParams view_params = makeViewParams(framing);

  const auto draw_frame = [&]() {
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    pass.render(view_params, frame_ctx);
  };

  for (int i = 0; i < kWarmupFrames; ++i) {
    draw_frame();
  }
  gl->glFinish();

  GLuint query = 0;
  gl->glGenQueries(1, &query);
  std::vector<double> wall_ms;
  std::vector<double> gpu_ms;
  for (int i = 0; i < kTimedFrames; ++i) {
    const auto t0 = Clock::now();
    gl->glBeginQuery(GL_TIME_ELAPSED, query);
    draw_frame();
    gl->glEndQuery(GL_TIME_ELAPSED);
    gl->glFinish();
    const auto t1 = Clock::now();
    wall_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    GLuint64 elapsed_ns = 0;
    gl->glGetQueryObjectui64v(query, GL_QUERY_RESULT, &elapsed_ns);
    gpu_ms.push_back(static_cast<double>(elapsed_ns) / 1.0e6);
  }
  gl->glDeleteQueries(1, &query);

  return Timing{medianMs(std::move(wall_ms)), medianMs(std::move(gpu_ms))};
}

int runBenchmark(const std::vector<std::size_t>& sizes, float cube_lod_threshold_px) {
  const std::array<ShapeCase, 4> shapes = shapeCases(cube_lod_threshold_px);
  QSurfaceFormat format;
  format.setVersion(4, 5);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setDepthBufferSize(24);

  QOffscreenSurface surface;
  surface.setFormat(format);
  surface.create();
  QOpenGLContext context;
  context.setFormat(format);
  if (!surface.isValid() || !context.create() || !context.makeCurrent(&surface)) {
    std::fprintf(stderr, "Failed to create an offscreen GL 4.5 context\n");
    return 1;
  }
  auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(&context);
  if (gl == nullptr) {
    std::fprintf(stderr, "No GL 4.5 core function set — the scene shaders cannot compile here\n");
    return 1;
  }
  gl->initializeOpenGLFunctions();

  const auto* renderer = reinterpret_cast<const char*>(gl->glGetString(GL_RENDERER));
  const auto* version = reinterpret_cast<const char*>(gl->glGetString(GL_VERSION));
  std::printf("GL_RENDERER : %s\n", renderer != nullptr ? renderer : "(null)");
  std::printf("GL_VERSION  : %s\n", version != nullptr ? version : "(null)");
  std::printf(
      "viewport    : %dx%d, MSAA %dx, cube/sphere size %.3f m, median of %d frames\n\n", kWidth, kHeight, kSamples,
      static_cast<double>(kCubeSizeMeters), kTimedFrames);

  QOpenGLFramebufferObjectFormat fbo_format;
  fbo_format.setAttachment(QOpenGLFramebufferObject::Depth);
  fbo_format.setInternalTextureFormat(GL_RGBA16F);  // linear HDR target, as the scene chain uses
  fbo_format.setSamples(kSamples);
  QOpenGLFramebufferObject fbo(kWidth, kHeight, fbo_format);
  if (!fbo.bind()) {
    std::fprintf(stderr, "Failed to bind the offscreen render target\n");
    return 1;
  }
  gl->glViewport(0, 0, kWidth, kHeight);
  gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  gl->glEnable(GL_DEPTH_TEST);
  gl->glEnable(GL_MULTISAMPLE);

  std::printf(
      "%9s | %6s | %10s | %10s | %10s | %10s | %s\n", "points", "camera", "point", "sphere", "cube", "cube+LOD",
      "LOD speedup");
  std::printf("%s\n", std::string(89, '-').c_str());

  for (const std::size_t n : sizes) {
    const SyntheticCloud cloud(n);
    const auto layout = checkFastPath(cloud.cloud, "");
    if (!layout.has_value()) {
      std::fprintf(stderr, "synthetic cloud missed the zero-copy fast path — benchmark invalid\n");
      return 2;
    }
    for (const Framing& framing : kFramings) {
      std::array<Timing, shapes.size()> timings{};
      for (std::size_t i = 0; i < shapes.size(); ++i) {
        timings[i] = timeCase(gl, cloud, *layout, shapes[i], framing);
      }
      const double cube_ms = timings[2].wall_ms;
      const double cube_lod_ms = timings[3].wall_ms;
      std::printf(
          "%9zu | %6s | %7.2f ms | %7.2f ms | %7.2f ms | %7.2f ms | %10.2fx\n", n, framing.name, timings[0].wall_ms,
          timings[1].wall_ms, cube_ms, cube_lod_ms, cube_lod_ms > 0.0 ? cube_ms / cube_lod_ms : 0.0);
      if (timings[2].gpu_ms > 0.0) {
        std::printf(
            "%9s | %6s | %7.2f ms | %7.2f ms | %7.2f ms | %7.2f ms | %10s   (GL_TIME_ELAPSED)\n", "", "",
            timings[0].gpu_ms, timings[1].gpu_ms, timings[2].gpu_ms, timings[3].gpu_ms, "");
      }
    }
  }

  std::printf(
      "\nwall = glFinish-bracketed median frame time (the honest metric on a software rasterizer).\n"
      "'wide' is vertex/primitive-setup bound (cubes ~1 px); 'close' is fill bound.\n"
      "'cube' forces the hexagon fan at every size; 'cube+LOD' lets sub-threshold cubes\n"
      "become matched point sprites, so the last column is a same-binary A/B of the LOD.\n");
  fbo.release();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  std::vector<std::size_t> sizes;
  // Sweeping this is how kDefaultCubeLodThresholdPx stays a reproducible measurement
  // rather than a remembered number.
  float cube_lod_threshold_px = pj::scene3d::kDefaultCubeLodThresholdPx;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--lod" && i + 1 < argc) {
      cube_lod_threshold_px = std::stof(argv[++i]);
      continue;
    }
    sizes.push_back(static_cast<std::size_t>(std::stoll(arg)));
  }
  if (sizes.empty()) {
    sizes = {100000, 500000};
  }
  return runBenchmark(sizes, cube_lod_threshold_px);
}
