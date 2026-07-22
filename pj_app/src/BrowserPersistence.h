// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <memory>
#include <optional>

class QSettings;

namespace PJ {

// Browser persistence is deliberately narrower than QSettings.
//
// On WASM the ordinary/default QSettings backend is redirected to /tmp, so all
// existing callers remain useful for the current page without accidentally
// making paths, plugin configs, credentials, or arbitrary extension settings
// durable. This bridge alone owns WebLocalStorageFormat and mirrors a small,
// schema-validated preference allowlist. Native builds never construct it and
// retain their existing QSettings format, locations, and behavior.
class BrowserPersistence final : public QObject {
  friend class BrowserPersistenceTestPeer;

 public:
  struct LayoutRecipe {
    QString id;
    QString name;
    QByteArray xml;
  };

  explicit BrowserPersistence(QObject* parent = nullptr);
  ~BrowserPersistence() override;

  BrowserPersistence(const BrowserPersistence&) = delete;
  BrowserPersistence& operator=(const BrowserPersistence&) = delete;

  [[nodiscard]] static BrowserPersistence* instance();

  // Public policy seams keep the security boundary directly unit-testable on
  // desktop, where WebLocalStorageFormat itself is intentionally never used.
  [[nodiscard]] static QStringList durablePreferenceKeys();
  [[nodiscard]] static bool isDurablePreferenceKey(const QString& key);
  [[nodiscard]] static std::optional<QVariant> sanitizePreference(const QString& key, const QVariant& value);
  [[nodiscard]] static bool isSafeGenericLayoutRecipe(const QByteArray& xml);

  [[nodiscard]] QList<LayoutRecipe> recentLayouts() const;
  [[nodiscard]] std::optional<LayoutRecipe> recentLayout(const QString& id) const;
  bool rememberGenericLayout(const QString& browser_name, const QByteArray& xml, QString* error = nullptr);
  void clearRecentLayouts();

  // Flush allowed session preferences now. Normal product code relies on the
  // event dispatcher's aboutToBlock signal; this entry point is also useful for
  // deterministic acceptance probes and shutdown.
  void syncPreferences();

#ifdef PJ_TARGET_WASM
  // Acceptance-only seams. Definitions are linked only by probe builds; they
  // remain declarations in production WASM so every TU sees one class shape.
  void seedLegacyUnsafeValuesForTest();
  [[nodiscard]] bool durableContainsForTest(const QString& key) const;
  [[nodiscard]] qsizetype durableKeyCountForTest() const;
#endif

 private:
  enum class PreferenceDecodeStatus {
    kAbsent,
    kValid,
    kRejected,
  };

  struct DecodedPreferences {
    PreferenceDecodeStatus status = PreferenceDecodeStatus::kAbsent;
    QHash<QString, QVariant> values;
  };

  [[nodiscard]] static DecodedPreferences decodePreferences(const QVariant& stored);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
