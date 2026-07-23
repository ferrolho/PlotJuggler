// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QColor>
#include <QDomDocument>
#include <QFuture>
#include <QFutureWatcher>
#include <QMap>
#include <QMetaObject>
#include <QString>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_datastore/object_store.hpp"
#include "pj_scene3d_core/robot_model.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/wasm/model_renderable_wasm.h"
#include "pj_scene_common/scene_layer.h"

namespace PJ {
class SessionManager;
}

namespace pj::scene3d {

class MeshLoader;
class UrdfPackageResolver;
class UrlFetcher;

// Browser RobotModel adapter. Local files are byte capabilities: only the URDF
// selected through QFileDialog is retained, so sibling/package disk references
// remain unresolved unless an exact in-band embedded asset exists. URL sources
// may resolve relative assets through bounded CORS fetches.
class WasmRobotModelLayer final : public PJ::ISceneLayer, public WasmModelRenderable {
  Q_OBJECT

 public:
  enum class SourceType { kTopic, kFile, kUrl };
  enum class DisplayMode { kAuto, kVisual, kCollision };

  WasmRobotModelLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~WasmRobotModelLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  bool attach(const PJ::SceneLayerContext& context) override;
  void detach() override;
  void setTrackerTime(PJ::Timepoint time) override;
  [[nodiscard]] std::uint64_t renderKey(PJ::Timepoint time) const override;
  void setVisible(bool visible) override;
  QWidget* createConfigWidget(QWidget* parent) override;
  QDomElement xmlSaveState(QDomDocument& document) const override;
  bool xmlLoadState(const QDomElement& element) override;

  void setTransformBuffer(std::shared_ptr<TransformBuffer> buffer);
  void setEmbeddedAssets(QMap<QString, QByteArray> assets);
  void setSourceTopic(PJ::ObjectTopicId topic_id, QString display_name = {});
  void setSourceFileContent(QString filename, QByteArray bytes);
  void setSourceUrl(QString url);
  void setFramePrefix(QString prefix);
  void setDisplayMode(DisplayMode mode);
  void setFallbackColor(QColor color);
  void setIgnoreColladaUpAxis(bool ignore);
  void retry();

  [[nodiscard]] SourceType sourceType() const {
    return source_type_;
  }
  [[nodiscard]] QString sourceValue() const {
    return source_value_;
  }
  [[nodiscard]] QString statusText() const {
    return status_text_;
  }
  [[nodiscard]] QString framePrefix() const {
    return frame_prefix_;
  }
  [[nodiscard]] DisplayMode displayMode() const {
    return display_mode_;
  }
  [[nodiscard]] QColor fallbackColor() const {
    return fallback_color_;
  }
  [[nodiscard]] bool ignoreColladaUpAxis() const {
    return ignore_collada_up_axis_;
  }
  [[nodiscard]] std::size_t fixedJointBridgeCount() const {
    return static_bridges_.size();
  }

  [[nodiscard]] const std::vector<WasmModelDrawCall>& modelDrawCalls() const override {
    return model_draw_calls_;
  }
  [[nodiscard]] const std::unordered_map<std::string, std::shared_ptr<const MeshData>>& modelMeshes() const override {
    return model_meshes_;
  }
  [[nodiscard]] std::uint64_t modelRevision() const override {
    return model_revision_;
  }
  [[nodiscard]] bool visible() const override {
    return visible_;
  }
  [[nodiscard]] float opacity() const override {
    return 1.0F;
  }
  [[nodiscard]] bool colorOverrideEnabled() const override {
    return false;
  }
  [[nodiscard]] QColor overrideColor() const override {
    return fallback_color_;
  }
  [[nodiscard]] bool wireframe() const override {
    return false;
  }
  [[nodiscard]] const std::string& sourceFrame() const override {
    return source_frame_;
  }
  [[nodiscard]] bool contributesToSceneBounds() const override {
    return false;
  }
  void noteRenderFailure(QString warning) override;
  void noteRenderSuccess() override;

  [[nodiscard]] static bool validateXml(const QDomElement& element);

 signals:
  void sourceFrameChanged(QString frame);
  void statusTextChanged(QString status);

 private:
  struct ParseRequest {
    QByteArray bytes;
    QString format;
    QString label;
    QString base;
    bool source_is_url = false;
    std::uint64_t generation = 0;
    std::shared_ptr<UrdfPackageResolver> resolver;
  };
  struct ParseResult {
    std::optional<RobotModel> model;
    QString error;
    QString label;
    std::uint64_t generation = 0;
    std::shared_ptr<UrdfPackageResolver> resolver;
  };
  struct MeshTask {
    std::string key;
    QString source;
    QString format_hint;
    bool is_url = false;
    std::uint64_t generation = 0;
  };
  struct ActiveImport;

  void loadFromCurrentSource();
  bool tryLoadTopicDescription();
  void queueParse(QByteArray bytes, QString format, QString label, QString base, bool source_is_url);
  void startParse(ParseRequest request);
  [[nodiscard]] static ParseResult parse(ParseRequest request);
  void onParseFinished();
  void applyParsed(ParseResult result);
  void resetModelState();
  void rebuildStaticBridges();
  void ensureStaticBridges();
  void rebuildDrawCalls();
  void queueMeshLoads();
  void pumpMeshLoads();
  void startMeshImport(MeshTask task, QByteArray bytes = {});
  void finishMeshImport();
  void updateStatus();
  void setStatus(QString status);
  void setWarning(QString warning);
  [[nodiscard]] QString unresolvedClause() const;
  [[nodiscard]] QString loadFailureClause() const;

  PJ::ObjectTopicId topic_id_;
  PJ::ObjectTopicId source_topic_id_;
  QString display_name_;
  PJ::SessionManager* session_ = nullptr;
  QMetaObject::Connection reload_connection_;
  std::shared_ptr<TransformBuffer> tf_buffer_;
  PJ::Timepoint tracker_time_{};

  SourceType source_type_ = SourceType::kTopic;
  QString source_value_;
  QByteArray local_file_bytes_;
  QMap<QString, QByteArray> embedded_assets_;
  QString frame_prefix_;
  DisplayMode display_mode_ = DisplayMode::kAuto;
  QColor fallback_color_{178, 178, 178};
  bool ignore_collada_up_axis_ = false;
  bool visible_ = true;
  bool latch_pending_ = false;
  std::chrono::steady_clock::time_point last_latch_retry_{};

  std::optional<RobotModel> model_;
  std::shared_ptr<UrdfPackageResolver> resolver_;
  std::vector<StampedTransform> static_bridges_;
  std::string source_frame_;
  bool model_has_visuals_ = false;
  int total_mesh_count_ = 0;
  int loaded_mesh_count_ = 0;
  int unresolved_mesh_count_ = 0;
  std::uint64_t mesh_retained_bytes_ = 0;

  std::unique_ptr<MeshLoader> mesh_loader_;
  std::unique_ptr<UrlFetcher> url_fetcher_;
  std::unordered_map<std::string, std::shared_ptr<const MeshData>> model_meshes_;
  std::vector<WasmModelDrawCall> model_draw_calls_;
  std::deque<MeshTask> mesh_queue_;
  std::optional<MeshTask> active_fetch_;
  std::unique_ptr<ActiveImport> active_import_;
  std::unordered_map<std::string, QString> mesh_errors_;
  std::uint64_t model_revision_ = 0;
  std::uint64_t load_generation_ = 0;
  std::uint64_t next_load_generation_ = 1;

  QFutureWatcher<ParseResult>* parse_watcher_ = nullptr;
  std::optional<ParseRequest> pending_parse_;
  std::uint64_t running_parse_generation_ = 0;
  std::uint64_t wanted_parse_generation_ = 0;
  std::uint64_t next_parse_generation_ = 1;
  QString base_status_;
  QString render_warning_;
  QString warning_reason_;
  QString status_text_;
};

}  // namespace pj::scene3d
