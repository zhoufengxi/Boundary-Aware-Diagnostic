//===--- AllocationState.h ------------------------------------- *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_STATICANALYZER_CHECKERS_ALLOCATIONSTATE_H
#define LLVM_CLANG_LIB_STATICANALYZER_CHECKERS_ALLOCATIONSTATE_H

#include "clang/StaticAnalyzer/Core/BugReporter/BugReporterVisitors.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"

namespace clang {
class FunctionDecl;

namespace ento {

namespace allocation_state {

/// Allocation families exposed to configurable API models.  The internal
/// MallocChecker representation intentionally remains private.
enum class AllocationFamilyKind {
  Malloc,
  CXXNew,
  CXXNewArray,
  Alloca,
};

ProgramStateRef markAllocated(ProgramStateRef State, SymbolRef Sym,
                              const Expr *Origin,
                              AllocationFamilyKind Family);

/// Returns true when MallocChecker already tracks \p Sym.  Configurable
/// release models use this to avoid manufacturing a released allocation for
/// an otherwise unknown value.
bool isTracked(ProgramStateRef State, SymbolRef Sym);

/// Returns true when \p Sym is tracked as already released.  Configurable
/// deallocation summaries use this to diagnose a second modeled release
/// before preserving the released state.
bool isReleased(ProgramStateRef State, SymbolRef Sym);

/// Returns true when \p Sym was released by a configurable summary for
/// \p Deallocator.  MallocChecker uses this narrow exception to let a repeated
/// invocation reach the configurable summary's double-release diagnostic;
/// passing the released value to any other function remains a use-after-free.
bool wasReleasedBy(ProgramStateRef State, SymbolRef Sym,
                   const FunctionDecl *Deallocator);

ProgramStateRef markReleased(ProgramStateRef State, SymbolRef Sym,
                             const Expr *Origin);

ProgramStateRef markReleased(ProgramStateRef State, SymbolRef Sym,
                             const Expr *Origin,
                             const FunctionDecl *Deallocator);

ProgramStateRef markRelinquished(ProgramStateRef State, SymbolRef Sym,
                                 const Expr *Origin);

ProgramStateRef markEscaped(ProgramStateRef State, SymbolRef Sym);

/// Creates the standard MallocChecker path visitor for \p Sym.  Clients that
/// emit ownership diagnostics can use it to show allocation and release
/// transitions instead of producing only a final warning event.
std::unique_ptr<BugReporterVisitor>
getAllocationBRVisitor(SymbolRef Sym);

/// This function provides an additional visitor that augments the bug report
/// with information relevant to memory errors caused by the misuse of
/// AF_InnerBuffer symbols.
std::unique_ptr<BugReporterVisitor> getInnerPointerBRVisitor(SymbolRef Sym);

/// 'Sym' represents a pointer to the inner buffer of a container object.
/// This function looks up the memory region of that object in
/// DanglingInternalBufferChecker's program state map.
const MemRegion *getContainerObjRegion(ProgramStateRef State, SymbolRef Sym);

} // end namespace allocation_state

} // end namespace ento
} // end namespace clang

#endif
