//===- AnalysisProgress.h - Analyzer progress statistics -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_STATICANALYZER_CORE_PATHSENSITIVE_ANALYSISPROGRESS_H
#define LLVM_CLANG_STATICANALYZER_CORE_PATHSENSITIVE_ANALYSISPROGRESS_H

#include "llvm/ADT/StringRef.h"

namespace clang {
namespace ento {

/// Writes low-overhead, process-wide progress samples for path-sensitive
/// analysis. A clang -cc1 process analyzes one translation unit at a time, so
/// a process-wide recorder also covers CTU work performed for that unit.
class AnalysisProgress {
public:
  static void initialize(llvm::StringRef OutputPath, unsigned IntervalMs);
  static void finalize();

  static void setCurrentEntry(llvm::StringRef EntryName);
  static void clearCurrentEntry();

  static void recordWorkListStep(bool IsCTU);
  static void recordPathCompleted();
  static void recordInlinedCall();
  static void recordTopLevelFunctionCompleted();
};

} // namespace ento
} // namespace clang

#endif
