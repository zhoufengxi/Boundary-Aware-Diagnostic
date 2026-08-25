//===- ConfigDrivenCallbacks.h ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared state used by configuration-driven callback invocation.  A checker
// records an immutable callback sequence after evaluating a modeled call.
// ExprEngine consumes the sequence by creating synthetic CallEnter nodes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_STATICANALYZER_CORE_PATHSENSITIVE_CONFIGDRIVENCALLBACKS_H
#define LLVM_CLANG_STATICANALYZER_CORE_PATHSENSITIVE_CONFIGDRIVENCALLBACKS_H

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"

namespace clang {
class StackFrameContext;

namespace ento {

namespace configured_callback {

constexpr unsigned MaxCallbacks = 8;
constexpr unsigned MaxArguments = 8;
constexpr unsigned MaxNestingDepth = 8;

struct Invocation {
  const FunctionDecl *Callee = nullptr;
  /// True when Callee was imported from another translation unit. ExprEngine
  /// uses this to enqueue the synthetic CallEnter on the CTU worklist, subject
  /// to the analyzer's normal CTU phase-one inlining policy.
  bool IsForeign = false;
  /// A synthetic, declaration-correct call site used by CallEnter, CallExit
  /// reconstruction and path diagnostics.  Each synthetic argument has a
  /// durable, non-transparent AST anchor.  Resolved argument SVals are bound
  /// to those caller-side anchors, after which the normal CallEvent and
  /// ProgramState::enterStackFrame path initializes callee parameters.
  const CallExpr *Call = nullptr;
  unsigned ArgumentCount = 0;
  SVal Arguments[MaxArguments];
};

/// Immutable after allocation.  Instances live in ProgramStateManager's bump
/// allocator, so ProgramState may safely retain the pointer.
struct Sequence {
  const Expr *Origin = nullptr;
  unsigned Count = 0;
  Invocation Invocations[MaxCallbacks];

  /// The callback that was active when a nested modeled call created this
  /// sequence.  It is restored after the nested sequence completes.
  const Sequence *Parent = nullptr;
  unsigned ParentIndex = 0;
  const StackFrameContext *ParentFrame = nullptr;
};

} // namespace configured_callback

struct PendingConfiguredCallbackSequence {};
struct ActiveConfiguredCallbackSequence {};
struct ActiveConfiguredCallbackIndex {};
struct ActiveConfiguredCallbackFrame {};
/// Marks the continuation of a completed outermost configured callback
/// sequence. Worklists that normally deprioritize revisited CFG locations use
/// this bit to avoid starving callback side effects behind unrelated paths.
struct ConfiguredCallbackContinuation {};

template <>
struct ProgramStateTrait<PendingConfiguredCallbackSequence>
    : public ProgramStatePartialTrait<const configured_callback::Sequence *> {
  static void *GDMIndex() {
    static int Index;
    return &Index;
  }
};

template <>
struct ProgramStateTrait<ActiveConfiguredCallbackSequence>
    : public ProgramStatePartialTrait<const configured_callback::Sequence *> {
  static void *GDMIndex() {
    static int Index;
    return &Index;
  }
};

template <>
struct ProgramStateTrait<ActiveConfiguredCallbackIndex>
    : public ProgramStatePartialTrait<unsigned> {
  static void *GDMIndex() {
    static int Index;
    return &Index;
  }
};

template <>
struct ProgramStateTrait<ActiveConfiguredCallbackFrame>
    : public ProgramStatePartialTrait<const StackFrameContext *> {
  static void *GDMIndex() {
    static int Index;
    return &Index;
  }
};

template <>
struct ProgramStateTrait<ConfiguredCallbackContinuation>
    : public ProgramStatePartialTrait<bool> {
  static void *GDMIndex() {
    static int Index;
    return &Index;
  }
};

} // namespace ento
} // namespace clang

#endif // LLVM_CLANG_STATICANALYZER_CORE_PATHSENSITIVE_CONFIGDRIVENCALLBACKS_H
