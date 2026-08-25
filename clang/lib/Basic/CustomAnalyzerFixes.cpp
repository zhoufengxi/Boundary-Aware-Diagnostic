//===- CustomAnalyzerFixes.cpp - CSA custom fix state ---------------------===//
//
// This source is intended to live at clang/lib/Basic/CustomAnalyzerFixes.cpp
// when the files in this directory are copied into a Clang source tree.
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/CustomAnalyzerFixes.h"

namespace clang {
namespace custom_analyzer_fixes {

State &getMutableState() {
  static thread_local State Current;
  return Current;
}

} // namespace custom_analyzer_fixes
} // namespace clang
