// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Test-only DataSource plugin that advertises kCapabilityHasDialog and carries an
// embedded dialog, used to exercise FileLoader's shutdown-while-suspended-at-the-
// plugin-config-dialog path. When a load runs without skip_dialog, the prologue
// coroutine suspends on DataSourceDialogAwaiter with this dialog open; tearing the
// loader down must reject the plugin (exactly one onRejected) BEFORE the embedded
// dialog is destroyed — otherwise a late reject reads a freed plugin ctx (UAF).
//
// The reject/destroy ordering happens across the DSO boundary, so it is recorded
// to an append-only probe file whose path comes from the PJ_DIALOG_PROBE_FILE
// env var: onRejected() appends "rejected\n", the dialog destructor appends
// "destroyed\n". The test asserts exactly one "rejected" and that it precedes the
// "destroyed" line. Claims ".dlgprobe".

#include <fstream>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_base/sdk/platform.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>

namespace {

constexpr const char* kDialogManifestJson = R"({
  "id": "dialog-probe-dialog",
  "name": "Dialog Probe",
  "version": "1.0.0",
  "description": "Records onRejected/destroy ordering for the shutdown UAF test"
})";

// A minimal but valid dialog UI: the QDialogButtonBox MUST be named "buttonBox"
// and declare standardButtons, or the host renders no OK/Cancel (see the
// dialog_protocol CLAUDE.md trap).
constexpr const char* kUiContent = R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>DialogProbe</class>
 <widget class="QWidget" name="DialogProbe">
  <layout class="QVBoxLayout">
   <item><widget class="QLineEdit" name="name_input"/></item>
   <item><widget class="QDialogButtonBox" name="buttonBox">
    <property name="standardButtons"><set>QDialogButtonBox::Cancel|QDialogButtonBox::Ok</set></property>
   </widget></item>
  </layout>
 </widget>
</ui>
)";

// Append one line to the probe file named by PJ_DIALOG_PROBE_FILE, flushing so an
// abrupt teardown never loses the record. A no-op when the env var is unset.
void recordProbe(const char* line) {
  const auto path = PJ::sdk::getEnv("PJ_DIALOG_PROBE_FILE");
  if (!path.has_value() || path->empty()) {
    return;
  }
  std::ofstream file(*path, std::ios::app);
  if (file) {
    file << line << '\n';
    file.flush();
  }
}

}  // namespace

class DialogProbeDialog : public PJ::DialogPluginTyped {
 public:
  // Record "destroyed" ONLY for the instance that was actually rejected. The
  // catalog/loader may create+destroy throwaway source instances (each embedding
  // a dialog) while probing capabilities; those were never opened or rejected, so
  // gating on rejected_ keeps the probe log to exactly the load's dialog and lets
  // the test assert reject-before-destroy for that one instance unambiguously.
  ~DialogProbeDialog() override {
    if (rejected_) {
      recordProbe("destroyed");
    }
  }

  std::string manifest() const override {
    return kDialogManifestJson;
  }
  std::string ui_content() const override {
    return kUiContent;
  }
  std::string widget_data() override {
    PJ::WidgetData wd;
    wd.setText("name_input", name_);
    wd.setOkEnabled(true);
    return wd.toJson();
  }
  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (widget_name == "name_input") {
      name_ = std::string(text);
      return true;
    }
    return false;
  }
  void onRejected() override {
    rejected_ = true;
    recordProbe("rejected");
  }
  std::string saveConfig() const override {
    return "{}";
  }
  bool loadConfig(std::string_view) override {
    return true;
  }

 private:
  std::string name_ = "default";
  bool rejected_ = false;
};

class DialogProbeSource : public PJ::FileSourceBase {
 public:
  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
  }

  PJ::Status importData() override {
    // Never reached in the shutdown test (the dialog is cancelled before accept),
    // but kept loadable so a full accept path would still ingest one row.
    auto topic = writeHost().ensureTopic("mock/file_data");
    if (!topic) {
      return PJ::unexpected(topic.error());
    }
    auto status = writeHost().appendRecord(*topic, PJ::Timestamp{100}, {{.name = "value", .value = 1.0}});
    if (!status) {
      return PJ::unexpected(status.error());
    }
    return PJ::okStatus();
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view json) override {
    return dialog_.loadConfig(json) ? PJ::okStatus() : PJ::unexpected("bad config");
  }

 private:
  DialogProbeDialog dialog_;
};

PJ_DATA_SOURCE_PLUGIN(
    DialogProbeSource, R"({"id":"dialog-probe-source","name":"Dialog Probe Source","version":"1.0.0",)"
                       R"("description":"DataSource with an embedded dialog for the shutdown UAF test",)"
                       R"("file_extensions":[".dlgprobe"]})")

PJ_DIALOG_PLUGIN(DialogProbeDialog, kDialogManifestJson)
