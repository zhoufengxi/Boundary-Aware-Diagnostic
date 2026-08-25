//===- CustomAnalyzerFixes.h - CSA custom fix switches ---------*- C++ -*-===//
//
// This header is intended to live at clang/Basic/CustomAnalyzerFixes.h when
// the files in this directory are copied into a Clang source tree.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_BASIC_CUSTOMANALYZERFIXES_H
#define LLVM_CLANG_BASIC_CUSTOMANALYZERFIXES_H

#include "llvm/Support/Compiler.h"

namespace clang {
namespace custom_analyzer_fixes {

struct State {
  bool CXX14LambdaAssignment = false;
  bool CXX17LambdaAssignment = false;
  bool ForEachLambdaArgument = false;
  bool CTUTemplateInstantiation = false;
  bool StructSubobjectInvalidation = false;
  bool ReportPath = false;
};

/// Return the effective custom-fix state for the current analyzer invocation.
/// Outside a ScopedState, every fix is disabled.
LLVM_LIBRARY_VISIBILITY State &getMutableState();

inline const State &getState() { return getMutableState(); }

inline bool isCXX14LambdaAssignmentFixEnabled() {
  return getState().CXX14LambdaAssignment;
}

inline bool isCXX17LambdaAssignmentFixEnabled() {
  return getState().CXX17LambdaAssignment;
}

inline bool isForEachLambdaArgumentFixEnabled() {
  return getState().ForEachLambdaArgument;
}

inline bool isCTUTemplateInstantiationFixEnabled() {
  return getState().CTUTemplateInstantiation;
}

inline bool isStructSubobjectInvalidationFixEnabled() {
  return getState().StructSubobjectInvalidation;
}

inline bool isReportPathFixEnabled() { return getState().ReportPath; }

/// Installs a state for one analyzer invocation. The previous state is restored
/// on destruction so nested and sequential analyses do not leak configuration.
class ScopedState {
  State Previous;

public:
  explicit ScopedState(const State &Current) : Previous(getState()) {
    getMutableState() = Current;
  }

  ScopedState(const ScopedState &) = delete;
  ScopedState &operator=(const ScopedState &) = delete;

  ~ScopedState() { getMutableState() = Previous; }
};

} // namespace custom_analyzer_fixes
} // namespace clang

#endif // LLVM_CLANG_BASIC_CUSTOMANALYZERFIXES_H
