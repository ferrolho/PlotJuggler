// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "pj_base/builtin/scene_entities.hpp"

namespace pj::scene3d {

// Backend-neutral reducer for the stateful ModelPrimitive half of a
// SceneEntities topic. The native layer currently carries the same semantics
// inline; the browser uses this isolated reducer so its QRhi/Assimp path cannot
// accidentally leak into procedural marker snapshot behavior.
class SceneEntitiesModelState {
 public:
  void clear();
  [[nodiscard]] bool applySnapshot(const PJ::sdk::SceneEntities& snapshot, std::int64_t ingest_ns);
  [[nodiscard]] bool dropExpired(std::int64_t time_ns);
  // Byte projection for the model-only entity copy made by applySnapshot().
  // nullopt means widened arithmetic or the temporary ID index overflowed.
  [[nodiscard]] std::optional<std::uint64_t> projectedRetainedBytes(const PJ::sdk::SceneEntities& snapshot) const;

  [[nodiscard]] const std::map<std::string, PJ::sdk::SceneEntity>& entities() const {
    return entities_;
  }
  [[nodiscard]] std::uint64_t retainedBytes() const {
    return retained_bytes_;
  }

 private:
  using Iterator = std::map<std::string, PJ::sdk::SceneEntity>::iterator;
  Iterator erase(Iterator iterator);

  std::map<std::string, PJ::sdk::SceneEntity> entities_;
  std::map<std::string, std::int64_t> expiry_anchor_ns_;
  std::uint64_t retained_bytes_ = 0;
};

}  // namespace pj::scene3d
