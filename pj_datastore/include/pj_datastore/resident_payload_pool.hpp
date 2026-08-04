#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// ResidentPayloadPool: an app-global, byte-bounded FIFO window over ingest-time
// object payloads.
//
// Purpose: a host running an ObjectIngestPolicy that fetches bytes at ingest
// (kLazyObjectsEagerScalars) holds each payload exactly once, for free, inside
// the producer's synchronous push — the "hot path". Dropping those bytes forces
// every later object read to re-fetch from the source (for MCAP: re-decompress
// a chunk); keeping them all is unbounded residency. The pool is the middle
// ground: admitted payloads stay resident until the byte budget rolls over,
// then their slots empty and the owning entries degrade to their lazy re-fetch
// fallback. Consumers chasing the ingest live edge (a 3D dock during a
// progressive load) therefore hit resident bytes while the entry remains
// inside the admitted window; steady-state memory stays bounded by
// `capacity_bytes`.
//
// Eviction is FIFO by admission order, deliberately not LRU: the workload is a
// monotonic producer with live-edge consumers, so arrival recency is the right
// signal, and FIFO keeps reads free of any shared bookkeeping. Admission-driven
// eviction runs on the admitting (ingest) thread; there is no eviction thread.
// A payload larger than the whole capacity is rejected up front rather than
// evicting useful residents for a seed that can never fit.
//
// Accounting charges `payload.bytes.size()`. A zero-copy anchored seed may pin
// a LARGER upstream allocation (e.g. the producer's decompressed chunk) until
// eviction — the budget bounds logical payload bytes, not anchor-backed RSS;
// copying every seed would make the charge exact at the cost of an ingest-path
// memcpy per message.
//
// Threading: `admit()` and slot destruction may run on any thread; `load()` is
// called concurrently by consumer threads. Payload anchors (which may run
// plugin release code) are always released OUTSIDE the pool mutex. Lock order:
// callers may hold ObjectStore locks when slots retire (entry destruction);
// pool code never takes store locks.

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>

#include "pj_base/buffer_anchor.hpp"

namespace PJ {

class ResidentPayloadPool;

namespace detail {
struct ResidentPoolState;
}  // namespace detail

/// One admitted payload's evictable residency. Owned (shared_ptr) by the
/// ObjectStore entry it seeds; the pool tracks it weakly. Presence of the
/// payload IS the residency state: it makes a one-way transition resident ->
/// empty — on pool eviction or slot destruction — and is never re-seeded, so a
/// reader either copies the immutable resident payload or observes empty and
/// uses its lazy fallback.
class ResidentSlot {
 public:
  ~ResidentSlot();

  ResidentSlot(const ResidentSlot&) = delete;
  ResidentSlot& operator=(const ResidentSlot&) = delete;
  ResidentSlot(ResidentSlot&&) = delete;
  ResidentSlot& operator=(ResidentSlot&&) = delete;

  /// A copy of the resident payload (span + anchor refcount bump), or nullopt
  /// once evicted. Cheap enough for render-rate reads: a per-slot mutex around
  /// the copy.
  [[nodiscard]] std::optional<sdk::PayloadView> load() const;

 private:
  friend class ResidentPayloadPool;
  ResidentSlot(std::shared_ptr<detail::ResidentPoolState> state, sdk::PayloadView payload, size_t charged_bytes);

  /// Move the payload out (nullopt if already taken). The caller owns the
  /// matching accounting subtraction and releases the returned payload's
  /// anchor outside the pool mutex.
  std::optional<sdk::PayloadView> take();

  mutable std::mutex mutex_;
  std::optional<sdk::PayloadView> payload_;
  std::shared_ptr<detail::ResidentPoolState> state_;
  size_t charged_bytes_ = 0;
};

/// See file header. Create one per app/session and share it (via shared_ptr)
/// with every ObjectStore that ingest pushes into; a capacity of 0 disables
/// admission entirely (every push degrades to the plain lazy path).
class ResidentPayloadPool {
 public:
  /// Counters are cumulative for the pool's lifetime; `resident_bytes` and
  /// `high_water_bytes` describe residency, not entry retention.
  struct Stats {
    size_t capacity_bytes = 0;
    size_t resident_bytes = 0;
    size_t high_water_bytes = 0;
    uint64_t admitted = 0;
    uint64_t rejected_oversize = 0;
    uint64_t evicted = 0;
    uint64_t resident_hits = 0;
  };

  explicit ResidentPayloadPool(size_t capacity_bytes);
  ~ResidentPayloadPool();

  ResidentPayloadPool(const ResidentPayloadPool&) = delete;
  ResidentPayloadPool& operator=(const ResidentPayloadPool&) = delete;
  ResidentPayloadPool(ResidentPayloadPool&&) = delete;
  ResidentPayloadPool& operator=(ResidentPayloadPool&&) = delete;

  /// Admit `payload` (charged at payload.bytes.size()) and return its slot, or
  /// nullptr when the pool is disabled, the payload is empty, or it exceeds the
  /// whole capacity. May evict older slots (FIFO) on this thread to make room;
  /// evicted anchors are released after the pool lock drops.
  [[nodiscard]] std::shared_ptr<ResidentSlot> admit(sdk::PayloadView payload);

  /// Evict every resident slot (entries keep their fallbacks). For memory
  /// pressure or tests; anchors are released outside the lock.
  void trim();

  [[nodiscard]] Stats stats() const;

 private:
  std::shared_ptr<detail::ResidentPoolState> state_;
};

}  // namespace PJ
