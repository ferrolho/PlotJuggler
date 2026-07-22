// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <memory>
#include <string>
#include <vector>

#include "pj_scripting/python_engine.h"

namespace PJ::scripting {
namespace {

class UnavailablePythonEngine final : public ScriptEngine {
 public:
  Expected<std::vector<FilterClass>> inspectModule(
      const std::string& /*source*/, const std::string& /*origin*/) override {
    return PJ::unexpected("Python Data Processors are unavailable in the WebAssembly build");
  }

  Expected<std::unique_ptr<FilterInstance>> createInstance(
      const FilterClass& /*klass*/, const std::string& /*params_json*/) override {
    return PJ::unexpected("Python Data Processors are unavailable in the WebAssembly build");
  }
};

}  // namespace

std::shared_ptr<ScriptEngine> makePythonEngine(BudgetLimits /*limits*/) {
  return std::make_shared<UnavailablePythonEngine>();
}

}  // namespace PJ::scripting
