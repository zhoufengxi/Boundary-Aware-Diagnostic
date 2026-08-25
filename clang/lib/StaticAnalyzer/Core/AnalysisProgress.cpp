//===- AnalysisProgress.cpp - Analyzer progress statistics ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/StaticAnalyzer/Core/PathSensitive/AnalysisProgress.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>

using namespace clang;
using namespace ento;

namespace {

using Clock = std::chrono::steady_clock;

struct ProgressState {
  std::unique_ptr<llvm::raw_fd_ostream> Output;
  Clock::time_point StartTime;
  Clock::time_point LastSampleTime;
  std::chrono::milliseconds Interval{1000};
  std::string CurrentEntry;
  uint64_t PathsCompleted = 0;
  uint64_t TopLevelFunctionsCompleted = 0;
  uint64_t InlinedCalls = 0;
  uint64_t STUSteps = 0;
  uint64_t CTUSteps = 0;
  uint64_t StepsSinceClockCheck = 0;

  bool enabled() const { return Output != nullptr; }

  void writeEscaped(llvm::StringRef Value) {
    *Output << '"';
    for (char C : Value) {
      if (C == '"')
        *Output << "\"\"";
      else if (C == '\n' || C == '\r')
        *Output << ' ';
      else
        *Output << C;
    }
    *Output << '"';
  }

  void writeSample(bool Force) {
    if (!enabled())
      return;

    const Clock::time_point Now = Clock::now();
    if (!Force && Now - LastSampleTime < Interval)
      return;

    const auto Elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Now - StartTime);
    *Output << Elapsed.count() << ',' << PathsCompleted << ','
            << TopLevelFunctionsCompleted << ',' << InlinedCalls << ','
            << STUSteps << ',' << CTUSteps << ',';
    writeEscaped(CurrentEntry);
    *Output << '\n';
    Output->flush();
    LastSampleTime = Now;
  }
};

ProgressState &getState() {
  static ProgressState State;
  return State;
}

} // namespace

void AnalysisProgress::initialize(llvm::StringRef OutputPath,
                                  unsigned IntervalMs) {
  ProgressState &State = getState();
  State.Output.reset();
  State.CurrentEntry.clear();
  State.PathsCompleted = 0;
  State.TopLevelFunctionsCompleted = 0;
  State.InlinedCalls = 0;
  State.STUSteps = 0;
  State.CTUSteps = 0;
  State.StepsSinceClockCheck = 0;

  if (OutputPath.empty())
    return;

  std::error_code EC;
  auto Output = std::make_unique<llvm::raw_fd_ostream>(
      OutputPath, EC, llvm::sys::fs::OF_Text);
  if (EC) {
    llvm::errs() << "warning: cannot open analyzer progress CSV '"
                 << OutputPath << "': " << EC.message() << '\n';
    return;
  }

  State.Output = std::move(Output);
  State.Interval = std::chrono::milliseconds(std::max(1u, IntervalMs));
  State.StartTime = Clock::now();
  State.LastSampleTime = State.StartTime;
  *State.Output << "elapsed_ms,paths_completed,"
                   "top_level_functions_completed,inlined_calls,stu_steps,"
                   "ctu_steps,current_entry\n";
  State.writeSample(/*Force=*/true);
}

void AnalysisProgress::finalize() {
  ProgressState &State = getState();
  if (!State.enabled())
    return;
  State.writeSample(/*Force=*/true);
  State.Output.reset();
  State.CurrentEntry.clear();
}

void AnalysisProgress::setCurrentEntry(llvm::StringRef EntryName) {
  ProgressState &State = getState();
  if (State.enabled())
    State.CurrentEntry = EntryName.str();
}

void AnalysisProgress::clearCurrentEntry() {
  ProgressState &State = getState();
  if (State.enabled())
    State.CurrentEntry.clear();
}

void AnalysisProgress::recordWorkListStep(bool IsCTU) {
  ProgressState &State = getState();
  if (!State.enabled())
    return;

  if (IsCTU)
    ++State.CTUSteps;
  else
    ++State.STUSteps;

  // Reading a clock and checking the output stream on every exploded-graph
  // step would distort the measurement. Check time once per 256 steps. The
  // resulting samples are approximate: a single expensive solver operation
  // can still delay a sample until control returns to the worklist.
  if (++State.StepsSinceClockCheck >= 256) {
    State.StepsSinceClockCheck = 0;
    State.writeSample(/*Force=*/false);
  }
}

void AnalysisProgress::recordPathCompleted() {
  ProgressState &State = getState();
  if (State.enabled())
    ++State.PathsCompleted;
}

void AnalysisProgress::recordInlinedCall() {
  ProgressState &State = getState();
  if (State.enabled())
    ++State.InlinedCalls;
}

void AnalysisProgress::recordTopLevelFunctionCompleted() {
  ProgressState &State = getState();
  if (!State.enabled())
    return;
  ++State.TopLevelFunctionsCompleted;
  State.writeSample(/*Force=*/false);
}
