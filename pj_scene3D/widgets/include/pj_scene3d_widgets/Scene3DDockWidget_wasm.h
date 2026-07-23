#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QDomDocument>
#include <QList>
#include <QMap>
#include <QMetaObject>
#include <QString>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene_common/scene_dock_widget.h"

class QResizeEvent;
class QLabel;
class QSettings;
class QToolButton;
class QWidget;

namespace pj::scene3d {
class SceneViewWidget;
class TransformService;
}  // namespace pj::scene3d

namespace PJ {

class ComboBox;

// Browser Scene3D family. The QRhi path supports raw/compressed PointCloud,
// DepthCloud, PosesInFrame, OccupancyGrid, VoxelGrid, SceneEntities, and URDF
// RobotModel layers.
class Scene3DDockWidget : public SceneDockWidget {
  Q_OBJECT

 public:
  explicit Scene3DDockWidget(QWidget* parent = nullptr);
  ~Scene3DDockWidget() override;

  void setSessionManager(SessionManager* session) override;
  void setTransformService(pj::scene3d::TransformService* service);
  [[nodiscard]] pj::scene3d::SceneViewWidget* sceneView() {
    return view_;
  }
  [[nodiscard]] const pj::scene3d::SceneViewWidget* sceneView() const {
    return view_;
  }

  [[nodiscard]] static bool handlesObjectType(sdk::BuiltinObjectType object_type);
  bool addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);
  bool tryAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  void onTrackerTime(double time) override;

  void setSettings(QSettings* settings) {
    settings_ = settings;
  }
  void setEmbeddedAssets(QMap<QString, QByteArray> assets) {
    embedded_assets_ = std::move(assets);
  }
  void setSourcePath(const QString& path) {
    source_path_ = path;
  }

  QDomElement xmlSaveState(QDomDocument& document) const override;
  bool xmlLoadState(const QDomElement& element) override;

  enum class FixedFrameMode { kAutoRoot, kExplicit };
  [[nodiscard]] QList<pj::scene3d::FrameRow> availableFrames() const {
    return available_frames_;
  }
  [[nodiscard]] QString currentFixedFrame() const;
  [[nodiscard]] bool isAutoRootMode() const {
    return fixed_frame_mode_ == FixedFrameMode::kAutoRoot;
  }
  [[nodiscard]] QString currentFollowFrame() const;
  void setFixedFrame(const QString& frame);
  void setFixedFrameAutoRoot();
  void setFollowFrame(const QString& frame);
  void recenterOnFollowFrame();

  [[nodiscard]] bool layerVisible(ObjectTopicId topic_id) const;
  struct OrphanSnapshot {
    bool is_orphan = false;
    QString reason;
  };
  [[nodiscard]] OrphanSnapshot orphanState(ObjectTopicId /*topic_id*/) const {
    return {};
  }

  struct RobotDescriptionTopic {
    ObjectTopicId topic_id;
    QString name;
  };
  [[nodiscard]] QList<RobotDescriptionTopic> robotDescriptionTopics() const;

 public slots:
  // Browser-local files are content capabilities, not paths. The selected URDF
  // bytes stay live only for this layer and are deliberately not embedded in a
  // saved layout; restored local-file layers ask the user to select again.
  ObjectTopicId addRobotModelLayerFromContent(QString filename, QByteArray bytes);
  ObjectTopicId addRobotModelLayerFromUrl(const QString& url);

 signals:
  void availableFramesChanged(const QList<pj::scene3d::FrameRow>& frames);
  void currentFixedFrameChanged(const QString& frame);
  void fixedFrameModeChanged(bool auto_root);
  void followFrameChanged(const QString& frame);
  void sceneViewReady();

 protected:
  QWidget* createSceneView() override;
  std::unique_ptr<SceneLayerContext> makeContext() override;
  [[nodiscard]] bool acceptsObjectType(sdk::BuiltinObjectType object_type) const override;
  [[nodiscard]] bool acceptsDeferredObjectType(sdk::BuiltinObjectType object_type) const override;
  [[nodiscard]] DeferredElementKind deferredElementKind(sdk::BuiltinObjectType object_type) const override;
  bool handleSceneConfigTopic(
      ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  void syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) override;
  void refreshView() override;
  [[nodiscard]] uint64_t viewRenderKey(PJ::Timepoint time) const override;
  [[nodiscard]] QString xmlTag() const override;
  [[nodiscard]] bool acceptsStateChildTag(const QString& tag) const override;
  [[nodiscard]] DatasetId representativeDatasetId() const override;
  bool pruneEvictedObjects() override;
  bool restoreOnePending(const QDomElement& element) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  enum class RestoreResult { kRestored, kDeferred, kInvalid };

  void reconnectLiveSamples(SessionManager* session);
  void prepareTransformBuffer(ObjectTopicId topic_id);
  void resetTransformBindingIfDatasetGone();
  void resetTransformBinding();
  void onDatasetTransformsReady(DatasetId dataset_id);
  void onAvailableFrames(const QList<pj::scene3d::FrameRow>& frames);
  [[nodiscard]] QString resolveAutoFixedFrame(const QList<pj::scene3d::FrameRow>& frames) const;
  void applyResolvedFixedFrame(const QString& frame);
  void refreshFrameOverlayCombo();
  void layoutOverlayControls();
  [[nodiscard]] RestoreResult restoreConfigTopic(const QDomElement& element);
  [[nodiscard]] RestoreResult restoreLocalRobotLayer(const QDomElement& element);
  [[nodiscard]] bool isLocalRobotLayerId(ObjectTopicId topic_id) const;
  ObjectTopicId allocateLocalRobotLayerId();
  void ensureLocalTransformBuffer();

  pj::scene3d::TransformService* transform_service_ = nullptr;
  pj::scene3d::SceneViewWidget* view_ = nullptr;
  std::shared_ptr<pj::scene3d::TransformBuffer> tf_buffer_;
  bool tf_buffer_is_local_ = false;
  DatasetId dataset_id_ = 0;
  std::unordered_set<uint32_t> config_topics_;
  std::unordered_set<uint32_t> layer_topics_;
  std::unordered_map<uint32_t, DatasetId> scene_topic_datasets_;
  QMetaObject::Connection live_samples_connection_;
  QMetaObject::Connection transforms_ready_connection_;
  double last_tracker_display_ = 0.0;

  QList<pj::scene3d::FrameRow> available_frames_;
  QList<pj::scene3d::FrameRow> transform_frames_;
  std::vector<std::string> fallback_frames_;
  FixedFrameMode fixed_frame_mode_ = FixedFrameMode::kAutoRoot;
  ComboBox* frame_overlay_combo_ = nullptr;
  ComboBox* camera_model_combo_ = nullptr;
  QToolButton* home_button_ = nullptr;
  QLabel* rendering_warning_label_ = nullptr;

  QSettings* settings_ = nullptr;
  QMap<QString, QByteArray> embedded_assets_;
  QString source_path_;
  uint32_t next_local_robot_topic_id_ = std::numeric_limits<uint32_t>::max();
  std::unordered_set<uint32_t> local_robot_layer_ids_;
  bool xml_rollback_in_progress_ = false;
};

}  // namespace PJ
