#ifndef LLVM_CLANG_STATICANALYZER_CORE_PATHSENSITIVE_UVASYNCMODELING_H
#define LLVM_CLANG_STATICANALYZER_CORE_PATHSENSITIVE_UVASYNCMODELING_H

#include "clang/AST/Decl.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"

namespace clang {
namespace ento {

struct PendingUvCallbackFD {};
struct PendingUvCallbackReq {};




struct PendingUvAfterCallbackFD {};
struct PendingUvAfterCallbackReq {};

struct ActiveUvWorkCallbackFD {};
struct ActiveUvWorkCallbackReq {};

template <>
struct ProgramStateTrait<PendingUvAfterCallbackFD>
    : public ProgramStatePartialTrait<const FunctionDecl *> {
  static void *GDMIndex() {
    static int Index;
    return &Index;
  }
};

template <>
struct ProgramStateTrait<PendingUvAfterCallbackReq>
    : public ProgramStatePartialTrait<const MemRegion *> {
  static void *GDMIndex() {
    static int Index;
    return &Index;
  }
};

template <>
struct ProgramStateTrait<ActiveUvWorkCallbackFD>
    : public ProgramStatePartialTrait<const FunctionDecl *> {
  static void *GDMIndex() {
    static int Index;
    return &Index;
  }
};

template <>
struct ProgramStateTrait<ActiveUvWorkCallbackReq>
    : public ProgramStatePartialTrait<const MemRegion *> {
  static void *GDMIndex() {
    static int Index;
    return &Index;
  }
};

template <>
struct ProgramStateTrait<PendingUvCallbackFD>
    : public ProgramStatePartialTrait<const FunctionDecl *> {
  static void *GDMIndex() {
    static int Index;
    return &Index;
  }
};

template <>
struct ProgramStateTrait<PendingUvCallbackReq>
    : public ProgramStatePartialTrait<const MemRegion *> {
  static void *GDMIndex() {
    static int Index;
    return &Index;
  }
};

} // namespace ento
} // namespace clang

#endif