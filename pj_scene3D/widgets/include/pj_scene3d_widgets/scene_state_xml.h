#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomElement>
#include <QString>
#include <glm/vec3.hpp>
#include <optional>

namespace pj::scene3d {

// Range-checked readers for persisted scene-dock XML state, shared by the
// native and WASM Scene3DDockWidget implementations so the accepted layout
// grammar cannot drift between platforms.
//
// All three readers follow the same contract: a MISSING attribute is valid
// (returns true, output untouched — absence means "keep the default"), a
// present-but-malformed or out-of-range value is a validation failure
// (returns false, output untouched).
bool readXmlBool(const QDomElement& element, const QString& key, std::optional<bool>& output);
bool readXmlInt(const QDomElement& element, const QString& key, int minimum, int maximum, std::optional<int>& output);
bool readXmlFloat(
    const QDomElement& element, const QString& key, float minimum, float maximum, std::optional<float>& output);

// The camera_state JSON payload after validation: every field is optional
// (absent keys keep the current camera value), but any present field is
// finite and inside its physical range.
struct ValidatedCameraState {
  std::optional<glm::vec3> focal;
  std::optional<float> radius;
  std::optional<float> azimuth;
  std::optional<float> elevation;
  std::optional<float> fov_y;
  std::optional<float> ortho_scale;
  std::optional<bool> perspective;
};

// Parses and range-checks the camera_state attribute (a JSON object). Returns
// nullopt on malformed JSON, a non-object payload, or any out-of-range field —
// callers treat that as "reject the whole element", never as partial state.
std::optional<ValidatedCameraState> validateCameraState(const QString& encoded);

}  // namespace pj::scene3d
