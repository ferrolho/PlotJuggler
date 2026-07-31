// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Smoke test for the embedded-CPython backend: proves the interpreter boots
// (PYTHONHOME / stdlib found), a filter module inspects + instantiates, and
// calculate() computes.

#include "pj_scripting/python_engine.h"

#include <gtest/gtest.h>

#include "pj_scripting/script_engine.h"

using namespace PJ::scripting;

namespace {
constexpr const char* kDoubleSource = R"PY(# pj-script: python
class T:
    id = "double_it"
    name = "Double"
    output = "double"
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *args):
        return value * 2
)PY";
}  // namespace

TEST(PythonEngine, InspectReadsClassMetadata) {
  auto engine = makePythonEngine();
  auto classes = engine->inspectModule(kDoubleSource, "test");
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  ASSERT_EQ(classes->size(), 1u);
  EXPECT_EQ((*classes)[0].id, "double_it");
  EXPECT_EQ((*classes)[0].output_kind, "double");
}

TEST(PythonEngine, CalculateComputesValueTimesTwo) {
  auto engine = makePythonEngine();
  auto classes = engine->inspectModule(kDoubleSource, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance((*classes)[0], "{}");
  ASSERT_TRUE(inst.has_value()) << (inst.has_value() ? "" : inst.error());
  auto r = (*inst)->calculate(0.0, 21.0);
  EXPECT_FALSE(r.suppress);
  EXPECT_DOUBLE_EQ(r.value, 42.0);
  EXPECT_FALSE((*inst)->failed());
}

TEST(PythonEngine, SyntaxErrorIsReported) {
  auto engine = makePythonEngine();
  auto classes = engine->inspectModule("# pj-script: python\nclass T\n  bad", "test");
  EXPECT_FALSE(classes.has_value());
}

TEST(PythonEngine, RuntimeErrorFailsStickily) {
  auto engine = makePythonEngine();
  constexpr const char* kBad = R"PY(# pj-script: python
class T:
    id = "boom"
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *args):
        return value + "oops"
)PY";
  auto classes = engine->inspectModule(kBad, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance((*classes)[0], "{}");
  ASSERT_TRUE(inst.has_value());
  auto r = (*inst)->calculate(0.0, 1.0);
  EXPECT_TRUE(r.suppress);         // a runtime error suppresses output
  EXPECT_TRUE((*inst)->failed());  // ...and puts the instance into a sticky failed state
}

// The stdlib's C extensions (lib-dynload/*.so) carry no copy of the interpreter:
// they resolve Py* at dlopen time against whatever the process already exports. An
// embedded CPython that does not publish those symbols dynamically breaks EVERY one
// of them, not just the module that happens to be imported first — so import all
// three allowed modules (whose undefined symbols differ; `statistics` alone pulls
// five extensions) and pin one of them down to a call. Without this the suite passes
// on a build whose Python cannot import anything.
TEST(PythonEngine, StdlibCExtensionsImport) {
  auto engine = makePythonEngine();
  constexpr const char* kImports = R"PY(# pj-script: python
import math
import cmath
import statistics

class T:
    id = "stdlib_sqrt"
    name = "Stdlib sqrt"
    output = "double"
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *args):
        return math.sqrt(value)
)PY";
  auto classes = engine->inspectModule(kImports, "test");
  // inspectModule executes the module body, so a broken lib-dynload surfaces here
  // as the ImportError text rather than as a mysterious empty catalogue.
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  ASSERT_EQ(classes->size(), 1u);
  auto inst = engine->createInstance((*classes)[0], "{}");
  ASSERT_TRUE(inst.has_value()) << (inst.has_value() ? "" : inst.error());
  auto r = (*inst)->calculate(0.0, 144.0);
  EXPECT_FALSE(r.suppress);
  EXPECT_DOUBLE_EQ(r.value, 12.0);
  EXPECT_FALSE((*inst)->failed());
}

// Data Processors compute over timeseries; the filesystem, the network and
// subprocesses are out of scope. A rejection must name the policy — a script author
// who imports `os` should learn it is unavailable here, not read a linker error.
TEST(PythonEngine, DisallowedImportsAreRejectedByPolicy) {
  auto engine = makePythonEngine();
  for (const char* module : {"os", "subprocess", "socket", "shutil"}) {
    const std::string source = std::string("# pj-script: python\nimport ") + module + "\n";
    auto classes = engine->inspectModule(source, "test");
    ASSERT_FALSE(classes.has_value()) << "'" << module << "' must not be importable from a filter";
    EXPECT_NE(classes.error().find("not available in Data Processors"), std::string::npos)
        << "rejection of '" << module << "' must state the policy, got: " << classes.error();
  }
}

// The gate governs only what the SCRIPT imports. `statistics` internally imports
// `random`, `fractions`, `_statistics` and more; those run under the module's own
// globals with unrestricted builtins, so an allowlist entry must not be crippled by
// its own dependencies being off-list.
TEST(PythonEngine, AllowedModulesKeepTheirOwnDependencies) {
  auto engine = makePythonEngine();
  constexpr const char* kSource = R"PY(# pj-script: python
import statistics

class T:
    id = "stdev"
    output = "double"
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *args):
        return statistics.mean([value, value, value])
)PY";
  auto classes = engine->inspectModule(kSource, "test");
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  auto inst = engine->createInstance((*classes)[0], "{}");
  ASSERT_TRUE(inst.has_value()) << (inst.has_value() ? "" : inst.error());
  EXPECT_DOUBLE_EQ((*inst)->calculate(0.0, 7.0).value, 7.0);
}
