//=-- ExprEngineCallAndReturn.cpp - Support for call/return -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines ExprEngine's support for calls and returns.
//
//===----------------------------------------------------------------------===//
// 处理CTU奇怪断链，此时 f = lambda for_each 模板 处理结构体断链 完成 UV(仅日志)(已去除) 模拟UV完成 现在加断链原因Note输出 有损优化 现在vector lambdaleaks(仅日志)
#include "PrettyStackTraceLocationContext.h"
#include "mydbg.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/Analysis/Analyses/LiveVariables.h"
#include "clang/Analysis/ConstructionContext.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/DynamicExtent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ExprEngine.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/AnalysisProgress.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/SaveAndRestore.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/UvAsyncModeling.h"
#include <cstdlib>
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporterVisitors.h"
#include <optional>

using namespace clang;
using namespace ento;

#define DEBUG_TYPE "ExprEngine"

STATISTIC(NumOfDynamicDispatchPathSplits,
  "The # of times we split the path due to imprecise dynamic dispatch info");

STATISTIC(NumInlinedCalls,
  "The # of times we inlined a call");

STATISTIC(NumReachedInlineCountMax,
  "The # of times we reached inline count maximum");

STATISTIC(NumUvPostCallFastPathSkips,
          "The # of non-uv post-call batches that bypassed uv state lookups");
STATISTIC(NumUvWorkExitFastPathSkips,
          "The # of ordinary call-exit batches that bypassed uv state lookups");
STATISTIC(NumUvPostCallStateLookups,
          "The # of uv post-call batches that queried pending callback state");
STATISTIC(NumUvWorkExitStateLookups,
          "The # of uv work-exit batches that queried after-callback state");
STATISTIC(NumUvCallbacksInlined,
          "The # of synthetic uv callbacks successfully inlined");
STATISTIC(MaxUvCallbackDepth,
          "The maximum location-context depth of an inlined uv callback");

namespace {

static bool CSAIsUvQueueWorkWithQos(const CallEvent &Call) {
  const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!FD)
    return false;
  const IdentifierInfo *II = FD->getIdentifier();
  return II && II->getName() == "uv_queue_work_with_qos";
}

static bool CSA_BREAK_CHECK() {
  return std::getenv("CSA_BREAK_CHECK") != nullptr;
}

static bool CSA_VECTOR_DBG() {
  return std::getenv("CSA_VECTOR_DBG") != nullptr && false;
}

static bool CSA_VECTOR_isInDebugFunction(const LocationContext *LC) {
  for (const LocationContext *I = LC; I; I = I->getParent()) {
    const Decl *D = I->getDecl();
    const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
    if (!FD)
      continue;

    // 只关注当前测试用例，避免无关日志过多。
    if (FD->getNameAsString() == "double_free")
      return true;
  }
  return false;
}

static std::string CSA_VECTOR_getDeclName(const Decl *D) {
  const auto *ND = dyn_cast_or_null<NamedDecl>(D);
  if (!ND)
    return "<unknown-decl>";
  return ND->getQualifiedNameAsString();
}

static bool CSA_VECTOR_isInterestingName(StringRef Name) {
  return Name.contains("std::vector<int *>") ||
         Name.contains("__gnu_cxx::__normal_iterator<int **, std::vector<int *>>") ||
         Name.contains("__gnu_cxx::operator!=");
}

static bool CSA_VECTOR_shouldLogCall(const CallEvent &Call) {
  if (!CSA_VECTOR_DBG())
    return false;

  if (!CSA_VECTOR_isInDebugFunction(Call.getLocationContext()))
    return false;

  std::string Name = CSA_VECTOR_getDeclName(Call.getDecl());
  return CSA_VECTOR_isInterestingName(Name);
}

static bool CSA_VECTOR_shouldLogDecl(const Decl *D) {
  if (!CSA_VECTOR_DBG())
    return false;

  std::string Name = CSA_VECTOR_getDeclName(D);
  return CSA_VECTOR_isInterestingName(Name);
}

static void CSA_VECTOR_printLoc(SourceManager &SM, SourceLocation Loc) {
  if (Loc.isInvalid()) {
    llvm::errs() << "<invalid-loc>";
    return;
  }

  PresumedLoc PLoc = SM.getPresumedLoc(SM.getExpansionLoc(Loc));
  if (!PLoc.isValid()) {
    llvm::errs() << "<invalid-presumed-loc>";
    return;
  }

  llvm::errs() << PLoc.getFilename()
               << ":" << PLoc.getLine()
               << ":" << PLoc.getColumn();
}

static void CSA_VECTOR_dumpSVal(StringRef Prefix, SVal V) {
  llvm::errs() << Prefix;
  V.dumpToStream(llvm::errs());
  llvm::errs() << "\n";
}

enum class CSABreakKind : unsigned {
  None = 0,

  // Generic conservative evaluation.
  ConservativeEvalFallback,

  // Inline decision.
  UnknownCalleeDecl,
  GlobalInliningDisabled,
  FunctionSummaryDisallowsInlining,
  StaticInlineabilityCheckFailed,
  CallKindDisallowedAlways,
  CallKindDisallowedOnce,
  RecursiveOrStackDepthLimitReached,
  LargeCalleeInlineCountLimitReached,
  MinimalInliningRejected,
  ReplayWithoutInlining,

  // Declaration-level inlineability.
  VariadicFunction,
  TemplateFunctionInliningDisabled,
  CXXStdLibraryInliningDisabled,
  CXXContainerMethodInliningDisabled,
  CXXSharedPtrDtorInliningDisabled,
  CFGConstructionFailed,
  HugeCallee,
  RelaxedLiveVariablesFailed,

  // Call-kind inlineability.
  CXXMemberFunctionInliningDisabled,
  CXXConstructorInliningDisabled,
  NewAllocatedObjectConstructorAllocatorInliningDisabled,
  ArrayConstructorOrDestructorUnsupported,
  ConstructorRequiresDestructorInlining,
  TemporaryConstructorWithoutTemporaryDtorsInCFG,
  ImproperlyModeledCtorDtorTargetRegion,
  TemporaryLifetimeExtendedViaAggregateUnsupported,
  CXXDestructorInliningDisabled,
  ArrayDestructorUnsupported,
  TemporaryDestructorInliningDisabled,
  ImproperlyModeledDestructorTargetRegion,
  CXXAllocatorInliningDisabled,
  ObjCMethodInliningDisabled,
  ObjCMethodRequiresDynamicDispatchIPA,

  // Dynamic dispatch / CTU.
  DynamicDispatchNotEnabled,
  DynamicDispatchInlineBranchConservative,
  DynamicDispatchConservativeBranch,
  DynamicDispatchConservativePathCreated,
  CTUPhase1ConservativeEval,
  CTUDispatchBifurcationReused,
};

struct CSABreakInfo {
  CSABreakKind Kind = CSABreakKind::None;
  const Decl *D = nullptr;
};

static thread_local CSABreakInfo CSA_LastBreakInfo;

static void CSA_clearBreakInfo() {
  CSA_LastBreakInfo = CSABreakInfo();
}

static bool CSA_hasBreakInfo() {
  return CSA_LastBreakInfo.Kind != CSABreakKind::None;
}

static void CSA_setBreakInfo(const Decl *D, CSABreakKind Kind) {
  if (!CSA_BREAK_CHECK())
    return;

  CSA_LastBreakInfo.Kind = Kind;
  CSA_LastBreakInfo.D = D;
}

static StringRef CSA_getBreakReason(CSABreakKind Kind) {
  switch (Kind) {
  case CSABreakKind::None:
    return "no analysis gap was recorded";

  case CSABreakKind::ConservativeEvalFallback:
    return "the call was evaluated conservatively";

  case CSABreakKind::UnknownCalleeDecl:
    return "callee declaration is unavailable";
  case CSABreakKind::GlobalInliningDisabled:
    return "global inlining is disabled by analyzer options";
  case CSABreakKind::FunctionSummaryDisallowsInlining:
    return "function summary marks this callee as non-inlinable";
  case CSABreakKind::StaticInlineabilityCheckFailed:
    return "callee declaration failed static inlineability checks";
  case CSABreakKind::CallKindDisallowedAlways:
    return "this call kind is always disallowed by the current inlining policy";
  case CSABreakKind::CallKindDisallowedOnce:
    return "this call kind is disallowed at this call site";
  case CSABreakKind::RecursiveOrStackDepthLimitReached:
    return "recursive inlining or maximum inline stack depth limit was reached";
  case CSABreakKind::LargeCalleeInlineCountLimitReached:
    return "large callee has been inlined more times than the configured limit";
  case CSABreakKind::MinimalInliningRejected:
    return "minimal inlining mode rejects this non-small or recursive callee";
  case CSABreakKind::ReplayWithoutInlining:
    return "inlining was already attempted for this call and failed; the replay path disables inlining";

  case CSABreakKind::VariadicFunction:
    return "variadic functions are not inlined";
  case CSABreakKind::TemplateFunctionInliningDisabled:
    return "template function inlining is disabled by analyzer options";
  case CSABreakKind::CXXStdLibraryInliningDisabled:
    return "C++ standard library function inlining is disabled by analyzer options";
  case CSABreakKind::CXXContainerMethodInliningDisabled:
    return "C++ container method inlining is disabled by analyzer options";
  case CSABreakKind::CXXSharedPtrDtorInliningDisabled:
    return "C++ shared_ptr destructor inlining is disabled because reference-count modeling is imprecise";
  case CSABreakKind::CFGConstructionFailed:
    return "callee CFG could not be constructed";
  case CSABreakKind::HugeCallee:
    return "callee is too large to inline";
  case CSABreakKind::RelaxedLiveVariablesFailed:
    return "relaxed live variables analysis failed for the callee";

  case CSABreakKind::CXXMemberFunctionInliningDisabled:
    return "C++ member function inlining is disabled by analyzer options";
  case CSABreakKind::CXXConstructorInliningDisabled:
    return "C++ constructor inlining is disabled by analyzer options";
  case CSABreakKind::NewAllocatedObjectConstructorAllocatorInliningDisabled:
    return "constructor for a newly allocated object is not inlined because allocator inlining is disabled";
  case CSABreakKind::ArrayConstructorOrDestructorUnsupported:
    return "array constructor/destructor is not precisely handled at this call site";
  case CSABreakKind::ConstructorRequiresDestructorInlining:
    return "constructor is not inlined because destructor inlining is disabled for this type";
  case CSABreakKind::TemporaryConstructorWithoutTemporaryDtorsInCFG:
    return "temporary constructor is not inlined because temporary destructors are not included in the CFG";
  case CSABreakKind::ImproperlyModeledCtorDtorTargetRegion:
    return "constructor/destructor target region is improperly modeled at this call site";
  case CSABreakKind::TemporaryLifetimeExtendedViaAggregateUnsupported:
    return "temporary lifetime extension through an aggregate is not precisely modeled for inlining";
  case CSABreakKind::CXXDestructorInliningDisabled:
    return "C++ destructor inlining is disabled by analyzer options";
  case CSABreakKind::ArrayDestructorUnsupported:
    return "array destructor is not precisely handled at this call site";
  case CSABreakKind::TemporaryDestructorInliningDisabled:
    return "temporary destructor inlining is disabled by analyzer options";
  case CSABreakKind::ImproperlyModeledDestructorTargetRegion:
    return "destructor target region is improperly modeled at this call site";
  case CSABreakKind::CXXAllocatorInliningDisabled:
    return "C++ allocator/deallocator inlining is disabled by analyzer options";
  case CSABreakKind::ObjCMethodInliningDisabled:
    return "Objective-C method inlining is disabled by analyzer options";
  case CSABreakKind::ObjCMethodRequiresDynamicDispatchIPA:
    return "Objective-C method requires dynamic-dispatch IPA mode for inlining";

  case CSABreakKind::DynamicDispatchNotEnabled:
    return "the runtime definition may have other definitions, but the current IPA mode does not allow dynamic-dispatch inlining";
  case CSABreakKind::DynamicDispatchInlineBranchConservative:
    return "dynamic-dispatch inline branch reached conservative evaluation after inlining was not continued";
  case CSABreakKind::DynamicDispatchConservativeBranch:
    return "dynamic-dispatch conservative branch assumes the receiver may resolve to another runtime definition";
  case CSABreakKind::DynamicDispatchConservativePathCreated:
    return "dynamic-dispatch bifurcation creates a conservative path for possible alternative runtime definitions";
  case CSABreakKind::CTUPhase1ConservativeEval:
    return "foreign function is queued for CTU phase 2, while the current path uses conservative evaluation in phase 1";
  case CSABreakKind::CTUDispatchBifurcationReused:
    return "CTU dispatch bifurcation was already used for this foreign function; current path uses conservative evaluation";
  }

  llvm_unreachable("unknown CSA break kind");
}

static std::string CSA_getDeclName(const Decl *D) {
  if (!D)
    return "<unknown callee>";

  if (const auto *ND = dyn_cast<NamedDecl>(D)) {
    std::string Name = ND->getQualifiedNameAsString();
    if (!Name.empty())
      return Name;
  }

  return "<unnamed callee>";
}

static const NoteTag *CSA_makeBreakNoteTag(ExprEngine &Eng,
                                           const CallEvent &Call) {
  if (!CSA_BREAK_CHECK())
    return nullptr;

  CSABreakInfo Info = CSA_LastBreakInfo;
  if (Info.Kind == CSABreakKind::None)
    Info.Kind = CSABreakKind::ConservativeEvalFallback;

  if (!Info.D)
    Info.D = Call.getDecl();

  std::string Msg;
  llvm::raw_string_ostream OS(Msg);

  OS << "[CSA analysis gap] This call was not precisely inlined and was "
        "modeled conservatively. ";
  OS << "Reason: " << CSA_getBreakReason(Info.Kind) << ".";

  if (Info.D)
    OS << " Callee: " << CSA_getDeclName(Info.D) << ".";

  OS << " Later data-flow in this report may miss details inside this call.";

  return Eng.getDataTags().make<NoteTag>(
      [Msg = OS.str()](BugReporterContext &,
                       PathSensitiveBugReport &) -> std::string {
        return Msg;
      },
      /*IsPrunable=*/false);
}

} // namespace

static const FieldDecl *findFieldInRecordForUvDbg(const RecordDecl *RD,
                                                  llvm::StringRef Name) {
  if (!RD)
    return nullptr;

  if (const RecordDecl *Def = RD->getDefinition())
    RD = Def;

  for (const FieldDecl *F : RD->fields()) {
    if (F->getName() == Name)
      return F;
  }

  return nullptr;
}

void ExprEngine::processPendingUvCallbackLogOnly(ExplodedNodeSet &Dst,
                                                 const ExplodedNodeSet &Src,
                                                 const CallEvent &Call) {
  for (ExplodedNode *N : Src) {
    ProgramStateRef State = N->getState();

    const FunctionDecl *CallbackFD = State->get<PendingUvCallbackFD>();
    const MemRegion *ReqR = State->get<PendingUvCallbackReq>();

    if (!CallbackFD || !ReqR) {
      Dst.insert(N);
      continue;
    }

    // 先只清除 pending callback，验证消费点是否正确。
    State = State->remove<PendingUvCallbackFD>();
    State = State->remove<PendingUvCallbackReq>();

    if (State == N->getState()) {
      Dst.insert(N);
      continue;
    }

    NodeBuilder Bldr(N, Dst, *currBldrCtx);
    Bldr.generateNode(N->getLocation(), State, N);
  }
}

void ExprEngine::processPendingUvCallback(ExplodedNodeSet &Dst,
                                          const ExplodedNodeSet &Src,
                                          const CallEvent &Call) {
  // PendingUvCallback* is only produced while evaluating this API. Avoid two
  // ProgramState GDM lookups for every unrelated conservative call.
  if (!CSAIsUvQueueWorkWithQos(Call)) {
    ++NumUvPostCallFastPathSkips;
    Dst.insert(Src);
    return;
  }
  ++NumUvPostCallStateLookups;

  for (ExplodedNode *N : Src) {
    ProgramStateRef State = N->getState();

    const FunctionDecl *CallbackFD = State->get<PendingUvCallbackFD>();
    const MemRegion *ReqR = State->get<PendingUvCallbackReq>();

    if (!CallbackFD || !ReqR) {
      Dst.insert(N);
      continue;
    }

    const FunctionDecl *BodyFD = nullptr;
    if (!CallbackFD->hasBody(BodyFD)) {

      State = State->remove<PendingUvCallbackFD>();
      State = State->remove<PendingUvCallbackReq>();

      NodeBuilder Bldr(N, Dst, *currBldrCtx);
      Bldr.generateNode(N->getLocation(), State, N);
      continue;
    }

    // 只清掉当前 callback。
    // 注意：不要清 PendingUvAfterCallbackFD / Req。
    // after_work_cb 要等 work_cb 返回后再触发。
    State = State->remove<PendingUvCallbackFD>();
    State = State->remove<PendingUvCallbackReq>();

    inlinePendingUvCallback(Dst, N, State, Call.getOriginExpr(), BodyFD, ReqR);
  }
}


void ExprEngine::processPendingUvAfterCallbackOnWorkExit(
    ExplodedNodeSet &Dst,
    const ExplodedNodeSet &Src,
    const FunctionDecl *ExitedFD,
    const Expr *CallE) {
  for (ExplodedNode *N : Src) {
    ProgramStateRef State = N->getState();

    const FunctionDecl *ActiveWorkFD =
        State->get<ActiveUvWorkCallbackFD>();
    const MemRegion *ActiveWorkReq =
        State->get<ActiveUvWorkCallbackReq>();

    const FunctionDecl *AfterFD =
        State->get<PendingUvAfterCallbackFD>();
    const MemRegion *AfterReq =
        State->get<PendingUvAfterCallbackReq>();

    bool IsTargetWorkExit = false;
    if (ActiveWorkFD && ExitedFD) {
      IsTargetWorkExit =
          ActiveWorkFD->getCanonicalDecl() == ExitedFD->getCanonicalDecl();
    }

    if (!ActiveWorkFD || !ActiveWorkReq || !AfterFD || !AfterReq ||
        !ExitedFD || !IsTargetWorkExit) {
      Dst.insert(N);
      continue;
    }

    const FunctionDecl *AfterBodyFD = nullptr;
    if (!AfterFD->hasBody(AfterBodyFD)) {
      State = State->remove<ActiveUvWorkCallbackFD>();
      State = State->remove<ActiveUvWorkCallbackReq>();
      State = State->remove<PendingUvAfterCallbackFD>();
      State = State->remove<PendingUvAfterCallbackReq>();

      NodeBuilder Bldr(N, Dst, *currBldrCtx);
      Bldr.generateNode(N->getLocation(), State, N);
      continue;
    }


    State = State->remove<ActiveUvWorkCallbackFD>();
    State = State->remove<ActiveUvWorkCallbackReq>();
    State = State->remove<PendingUvAfterCallbackFD>();
    State = State->remove<PendingUvAfterCallbackReq>();

    // 注意：这里用的是 work_cb 返回后的 State。
    // 因此 after_work_cb 会看到 work_cb 中 delete pair 之后的状态。
    inlinePendingUvCallback(Dst, N, State, CallE, AfterBodyFD, AfterReq);
  }
}

void ExprEngine::inlinePendingUvCallback(ExplodedNodeSet &Dst,
                                         ExplodedNode *Pred,
                                         ProgramStateRef State,
                                         const Expr *CallE,
                                         const FunctionDecl *CallbackFD,
                                         const MemRegion *ReqR) {
  assert(CallbackFD);
  assert(ReqR);

  if (CallbackFD->param_size() < 1) {
    Dst.insert(Pred);
    return;
  }

  WorkList *WList = Engine.getWorkList();
  if (!WList) {
    Dst.insert(Pred);
    return;
  }

  const LocationContext *CurLC = Pred->getLocationContext();
  const StackFrameContext *CallerSFC = CurLC->getStackFrame();
  const LocationContext *ParentOfCallee = CallerSFC;


  AnalysisDeclContext *CalleeADC =
      AMgr.getAnalysisDeclContext(CallbackFD);

  const StackFrameContext *CalleeSFC =
      CalleeADC->getStackFrame(ParentOfCallee, CallE,
                               currBldrCtx->getBlock(),
                               currBldrCtx->blockCount(),
                               currStmtIdx);

  const ParmVarDecl *WorkParam = CallbackFD->getParamDecl(0);

  SValBuilder &SVB = getSValBuilder();

  SVal WorkV = loc::MemRegionVal(ReqR);

  SVal WorkParamLoc = State->getLValue(WorkParam, CalleeSFC);
  std::optional<Loc> WorkParamL = WorkParamLoc.getAs<Loc>();

  if (!WorkParamL) {
    Dst.insert(Pred);
    return;
  }

  State = State->bindLoc(*WorkParamL, WorkV, CalleeSFC);

  const ParmVarDecl *StatusParam = nullptr;
  SVal StatusV = UnknownVal();

  if (CallbackFD->param_size() >= 2) {
    StatusParam = CallbackFD->getParamDecl(1);
    StatusV = SVB.makeIntVal(0, StatusParam->getType());

    SVal StatusParamLoc = State->getLValue(StatusParam, CalleeSFC);
    std::optional<Loc> StatusParamL = StatusParamLoc.getAs<Loc>();

    if (!StatusParamL) {
      Dst.insert(Pred);
      return;
    }

    State = State->bindLoc(*StatusParamL, StatusV, CalleeSFC);
  }


  const FunctionDecl *AfterFD = State->get<PendingUvAfterCallbackFD>();
  const MemRegion *AfterReq = State->get<PendingUvAfterCallbackReq>();

  // If a pending after_work_cb exists while we are inlining a 1-argument
  // callback, this callback is the synthetic work_cb.
  // Mark it as active so that CallExit can trigger after_work_cb later.
  if (AfterFD && AfterReq && CallbackFD->param_size() == 1) {
    State = State->set<ActiveUvWorkCallbackFD>(CallbackFD);
    State = State->set<ActiveUvWorkCallbackReq>(ReqR);
    UvWorkCallbackDecls.insert(CallbackFD->getCanonicalDecl());
  }

  CallEnter Loc(CallE, CalleeSFC, CurLC);

  bool IsNew = false;
  ExplodedNode *NewNode = G.getNode(Loc, State, false, &IsNew);

  if (!NewNode) {

    Dst.insert(Pred);
    return;
  }

  NewNode->addPredecessor(Pred, G);



  if (IsNew) {

    WList->enqueue(NewNode);
  } else {
  }

  // 这里不要 Dst.insert(Pred)。
  // 成功创建 CallEnter 后，当前路径应该转入 callback，
  // 而不是继续作为 uv_queue_work_with_qos 后的普通路径走下去。
  NumInlinedCalls++;
  AnalysisProgress::recordInlinedCall();
  ++NumUvCallbacksInlined;
  unsigned CallbackDepth = 0;
  for (const LocationContext *I = CalleeSFC; I; I = I->getParent())
    ++CallbackDepth;
  MaxUvCallbackDepth.updateMax(CallbackDepth);
  Engine.FunctionSummaries->bumpNumTimesInlined(CallbackFD);

  if (!isSecondPhaseCTU())
    if (VisitedCallees)
      VisitedCallees->insert(CallbackFD);
}

bool ExprEngine::inlineConfiguredCallback(
    ExplodedNode *Pred, ProgramStateRef State,
    const configured_callback::Sequence *Sequence, unsigned Index,
    const FunctionDecl *CallbackFD) {
  if (!Pred || !State || !Sequence || !CallbackFD ||
      Index >= Sequence->Count || !Sequence->Origin)
    return false;

  const configured_callback::Invocation &Invocation =
      Sequence->Invocations[Index];
  if (!Invocation.Call || CallbackFD->isVariadic() ||
      CallbackFD->getNumParams() != Invocation.ArgumentCount)
    return false;

  const LocationContext *CurrentLC = Pred->getLocationContext();
  const StackFrameContext *CallerSFC = CurrentLC->getStackFrame();

  // The synthetic argument expressions have durable, non-transparent
  // anchors.  Bind them in the caller context so generic CallEvent and
  // BugReporter code can recover the configured argument values and track
  // symbols across the synthetic call boundary.
  for (unsigned I = 0; I < Invocation.ArgumentCount; ++I)
    State = State->BindExpr(Invocation.Call->getArg(I), CurrentLC,
                            Invocation.Arguments[I]);

  CallEventManager &CallMgr =
      State->getStateManager().getCallEventManager();
  CallEventRef<> CallbackCall =
      CallMgr.getSimpleCall(Invocation.Call, State, CurrentLC,
                            getCFGElementRef());
  if (!CallbackCall || !CallbackCall->getDecl() ||
      CallbackCall->getDecl()->getCanonicalDecl() !=
          CallbackFD->getCanonicalDecl())
    return false;
  CallbackCall->setForeign(Invocation.IsForeign);

  AnalysisDeclContext *CalleeADC =
      AMgr.getAnalysisDeclContext(CallbackFD);
  WorkList *WList = Engine.getWorkList();
  if (Invocation.IsForeign && !isSecondPhaseCTU()) {
    const auto InliningKind = AMgr.options.getCTUPhase1Inlining();
    const bool InlineInPhaseOne =
        InliningKind == CTUPhase1InliningKind::All ||
        (InliningKind == CTUPhase1InliningKind::Small &&
         isSmall(CalleeADC));
    if (!InlineInPhaseOne)
      WList = Engine.getCTUWorkList();
  }
  if (!WList)
    return false;

  const StackFrameContext *CalleeSFC =
      CalleeADC->getStackFrame(CallerSFC, Invocation.Call,
                               currBldrCtx->getBlock(),
                               currBldrCtx->blockCount(), currStmtIdx);

  // Use the normal StoreManager frame initialization now that getArgSVal()
  // can safely recover every configured value from its anchored expression.
  // Besides binding parameters for callback-body evaluation, this preserves
  // the standard CallEvent relationship used by bug-report visitors to track
  // an interesting symbol across CallEnter and CallExit.
  State = State->enterStackFrame(*CallbackCall, CalleeSFC);
  State = State->set<ActiveConfiguredCallbackSequence>(Sequence);
  State = State->set<ActiveConfiguredCallbackIndex>(Index);
  State = State->set<ActiveConfiguredCallbackFrame>(CalleeSFC);

  CallEnter Location(Invocation.Call, CalleeSFC, CurrentLC);
  bool IsNew = false;
  ExplodedNode *NewNode = G.getNode(Location, State, false, &IsNew);
  if (!NewNode)
    return false;
  NewNode->addPredecessor(Pred, G);
  if (IsNew)
    WList->enqueue(NewNode);

  ConfiguredCallbackDecls.insert(CallbackFD->getCanonicalDecl());
  NumInlinedCalls++;
  AnalysisProgress::recordInlinedCall();
  Engine.FunctionSummaries->bumpNumTimesInlined(CallbackFD);
  if (!isSecondPhaseCTU() && VisitedCallees)
    VisitedCallees->insert(CallbackFD);
  return true;
}

void ExprEngine::scheduleNextConfiguredCallback(
    ExplodedNodeSet &Dst, ExplodedNode *Pred, ProgramStateRef State,
    const configured_callback::Sequence *Sequence, unsigned StartIndex) {
  if (!Pred || !State || !Sequence) {
    if (Pred)
      Dst.insert(Pred);
    return;
  }

  State = State->remove<PendingConfiguredCallbackSequence>();

  for (unsigned I = StartIndex; I < Sequence->Count; ++I) {
    const configured_callback::Invocation &Invocation =
        Sequence->Invocations[I];
    const FunctionDecl *CallbackFD = Invocation.Callee;
    const FunctionDecl *BodyFD = nullptr;
    const bool HasBody = CallbackFD && CallbackFD->hasBody(BodyFD);
    if (!HasBody)
      continue;
    if (inlineConfiguredCallback(Pred, State, Sequence, I, BodyFD))
      return;
  }

  State = State->remove<ActiveConfiguredCallbackSequence>();
  State = State->remove<ActiveConfiguredCallbackIndex>();
  State = State->remove<ActiveConfiguredCallbackFrame>();
  if (Sequence->Parent) {
    State =
        State->set<ActiveConfiguredCallbackSequence>(Sequence->Parent);
    State =
        State->set<ActiveConfiguredCallbackIndex>(Sequence->ParentIndex);
    if (Sequence->ParentFrame)
      State =
          State->set<ActiveConfiguredCallbackFrame>(Sequence->ParentFrame);
  } else {
    // The default unexplored-first worklists lower the priority of a loop
    // entrance every time that location is reached. A callback may mutate
    // ownership state and then resume at such an entrance; without a marker,
    // the semantically new state can remain queued until the global analysis
    // budget expires. Keep completed outermost callback continuations eager.
    State = State->set<ConfiguredCallbackContinuation>(true);
  }

  if (State == Pred->getState()) {
    Dst.insert(Pred);
    return;
  }

  // Pred may be the CallExitEnd of the last configured callback.  Reusing
  // that program point for the state-only cleanup below would create two
  // consecutive CallExitEnd nodes for a single CallEnter.  BugReporter then
  // builds an unbalanced call path and can discard otherwise valid allocation
  // and release notes emitted from inside the callback.
  //
  // Represent completion at the modeled call expression instead.  The tag
  // keeps this state-only node distinct from the ordinary PostStmt generated
  // while evaluating the modeled call.
  static SimpleProgramPointTag SequenceCompleteTag(
      "ExprEngine", "Configured callback sequence complete");
  PostStmt CompletionPoint(Sequence->Origin, Pred->getLocationContext(),
                           &SequenceCompleteTag);
  NodeBuilder Builder(Pred, Dst, *currBldrCtx);
  Builder.generateNode(CompletionPoint, State, Pred);
}

void ExprEngine::processPendingConfiguredCallbacks(
    ExplodedNodeSet &Dst, const ExplodedNodeSet &Src) {
  for (ExplodedNode *N : Src) {
    ProgramStateRef State = N->getState();
    const configured_callback::Sequence *Sequence =
        State->get<PendingConfiguredCallbackSequence>();
    if (!Sequence || Sequence->Count == 0) {
      Dst.insert(N);
      continue;
    }
    scheduleNextConfiguredCallback(Dst, N, State, Sequence, 0);
  }
}

void ExprEngine::processConfiguredCallbackExit(
    ExplodedNodeSet &Dst, const ExplodedNodeSet &Src,
    const FunctionDecl *ExitedFD,
    const StackFrameContext *ExitedFrame) {
  for (ExplodedNode *N : Src) {
    ProgramStateRef State = N->getState();
    const configured_callback::Sequence *Sequence =
        State->get<ActiveConfiguredCallbackSequence>();
    unsigned Index = State->get<ActiveConfiguredCallbackIndex>();
    const StackFrameContext *ActiveFrame =
        State->get<ActiveConfiguredCallbackFrame>();
    if (!Sequence || Index >= Sequence->Count || !ExitedFD ||
        ActiveFrame != ExitedFrame) {
      Dst.insert(N);
      continue;
    }

    const FunctionDecl *Expected = Sequence->Invocations[Index].Callee;
    if (!Expected ||
        Expected->getCanonicalDecl() != ExitedFD->getCanonicalDecl()) {
      Dst.insert(N);
      continue;
    }
    scheduleNextConfiguredCallback(Dst, N, State, Sequence, Index + 1);
  }
}

void ExprEngine::processCallEnter(NodeBuilderContext& BC, CallEnter CE,
                                  ExplodedNode *Pred) {
  // Get the entry block in the CFG of the callee.
  const StackFrameContext *calleeCtx = CE.getCalleeContext();
  PrettyStackTraceLocationContext CrashInfo(calleeCtx);
  const CFGBlock *Entry = CE.getEntry();

  // Validate the CFG.
  assert(Entry->empty());
  assert(Entry->succ_size() == 1);

  // Get the solitary successor.
  const CFGBlock *Succ = *(Entry->succ_begin());

  // Construct an edge representing the starting location in the callee.
  BlockEdge Loc(Entry, Succ, calleeCtx);

  ProgramStateRef state = Pred->getState();

  // Construct a new node, notify checkers that analysis of the function has
  // begun, and add the resultant nodes to the worklist.
  bool isNew;
  ExplodedNode *Node = G.getNode(Loc, state, false, &isNew);
  Node->addPredecessor(Pred, G);
  if (isNew) {
    ExplodedNodeSet DstBegin;
    processBeginOfFunction(BC, Node, DstBegin, Loc);
    Engine.enqueue(DstBegin);
  }
}

// Find the last statement on the path to the exploded node and the
// corresponding Block.
static std::pair<const Stmt*,
                 const CFGBlock*> getLastStmt(const ExplodedNode *Node) {
  const Stmt *S = nullptr;
  const CFGBlock *Blk = nullptr;
  const StackFrameContext *SF = Node->getStackFrame();

  // Back up through the ExplodedGraph until we reach a statement node in this
  // stack frame.
  while (Node) {
    const ProgramPoint &PP = Node->getLocation();

    if (PP.getStackFrame() == SF) {
      if (std::optional<StmtPoint> SP = PP.getAs<StmtPoint>()) {
        S = SP->getStmt();
        break;
      } else if (std::optional<CallExitEnd> CEE = PP.getAs<CallExitEnd>()) {
        S = CEE->getCalleeContext()->getCallSite();
        if (S)
          break;

        // If there is no statement, this is an implicitly-generated call.
        // We'll walk backwards over it and then continue the loop to find
        // an actual statement.
        std::optional<CallEnter> CE;
        do {
          Node = Node->getFirstPred();
          CE = Node->getLocationAs<CallEnter>();
        } while (!CE || CE->getCalleeContext() != CEE->getCalleeContext());

        // Continue searching the graph.
      } else if (std::optional<BlockEdge> BE = PP.getAs<BlockEdge>()) {
        Blk = BE->getSrc();
      }
    } else if (std::optional<CallEnter> CE = PP.getAs<CallEnter>()) {
      // If we reached the CallEnter for this function, it has no statements.
      if (CE->getCalleeContext() == SF)
        break;
    }

    if (Node->pred_empty())
      return std::make_pair(nullptr, nullptr);

    Node = *Node->pred_begin();
  }

  return std::make_pair(S, Blk);
}

/// Adjusts a return value when the called function's return type does not
/// match the caller's expression type. This can happen when a dynamic call
/// is devirtualized, and the overriding method has a covariant (more specific)
/// return type than the parent's method. For C++ objects, this means we need
/// to add base casts.
static SVal adjustReturnValue(SVal V, QualType ExpectedTy, QualType ActualTy,
                              StoreManager &StoreMgr) {
  // For now, the only adjustments we handle apply only to locations.
  if (!isa<Loc>(V))
    return V;

  // If the types already match, don't do any unnecessary work.
  ExpectedTy = ExpectedTy.getCanonicalType();
  ActualTy = ActualTy.getCanonicalType();
  if (ExpectedTy == ActualTy)
    return V;

  // No adjustment is needed between Objective-C pointer types.
  if (ExpectedTy->isObjCObjectPointerType() &&
      ActualTy->isObjCObjectPointerType())
    return V;

  // C++ object pointers may need "derived-to-base" casts.
  const CXXRecordDecl *ExpectedClass = ExpectedTy->getPointeeCXXRecordDecl();
  const CXXRecordDecl *ActualClass = ActualTy->getPointeeCXXRecordDecl();
  if (ExpectedClass && ActualClass) {
    CXXBasePaths Paths(/*FindAmbiguities=*/true, /*RecordPaths=*/true,
                       /*DetectVirtual=*/false);
    if (ActualClass->isDerivedFrom(ExpectedClass, Paths) &&
        !Paths.isAmbiguous(ActualTy->getCanonicalTypeUnqualified())) {
      return StoreMgr.evalDerivedToBase(V, Paths.front());
    }
  }

  // Unfortunately, Objective-C does not enforce that overridden methods have
  // covariant return types, so we can't assert that that never happens.
  // Be safe and return UnknownVal().
  return UnknownVal();
}

void ExprEngine::removeDeadOnEndOfFunction(NodeBuilderContext& BC,
                                           ExplodedNode *Pred,
                                           ExplodedNodeSet &Dst) {
  // Find the last statement in the function and the corresponding basic block.
  const Stmt *LastSt = nullptr;
  const CFGBlock *Blk = nullptr;
  std::tie(LastSt, Blk) = getLastStmt(Pred);
  if (!Blk || !LastSt) {
    Dst.Add(Pred);
    return;
  }

  // Here, we destroy the current location context. We use the current
  // function's entire body as a diagnostic statement, with which the program
  // point will be associated. However, we only want to use LastStmt as a
  // reference for what to clean up if it's a ReturnStmt; otherwise, everything
  // is dead.
  SaveAndRestore<const NodeBuilderContext *> NodeContextRAII(currBldrCtx, &BC);
  const LocationContext *LCtx = Pred->getLocationContext();
  removeDead(Pred, Dst, dyn_cast<ReturnStmt>(LastSt), LCtx,
             LCtx->getAnalysisDeclContext()->getBody(),
             ProgramPoint::PostStmtPurgeDeadSymbolsKind);
}

static bool wasDifferentDeclUsedForInlining(CallEventRef<> Call,
    const StackFrameContext *calleeCtx) {
  const Decl *RuntimeCallee = calleeCtx->getDecl();
  const Decl *StaticDecl = Call->getDecl();
  assert(RuntimeCallee);
  if (!StaticDecl)
    return true;
  return RuntimeCallee->getCanonicalDecl() != StaticDecl->getCanonicalDecl();
}

// Returns the number of elements in the array currently being destructed.
// If the element count is not found 0 will be returned.
static unsigned getElementCountOfArrayBeingDestructed(
    const CallEvent &Call, const ProgramStateRef State, SValBuilder &SVB) {
  assert(isa<CXXDestructorCall>(Call) &&
         "The call event is not a destructor call!");

  const auto &DtorCall = cast<CXXDestructorCall>(Call);

  auto ThisVal = DtorCall.getCXXThisVal();

  if (auto ThisElementRegion = dyn_cast<ElementRegion>(ThisVal.getAsRegion())) {
    auto ArrayRegion = ThisElementRegion->getAsArrayOffset().getRegion();
    auto ElementType = ThisElementRegion->getElementType();

    auto ElementCount =
        getDynamicElementCount(State, ArrayRegion, SVB, ElementType);

    if (!ElementCount.isConstant())
      return 0;

    return ElementCount.getAsInteger()->getLimitedValue();
  }

  return 0;
}

ProgramStateRef ExprEngine::removeStateTraitsUsedForArrayEvaluation(
    ProgramStateRef State, const CXXConstructExpr *E,
    const LocationContext *LCtx) {

  assert(LCtx && "Location context must be provided!");

  if (E) {
    if (getPendingInitLoop(State, E, LCtx))
      State = removePendingInitLoop(State, E, LCtx);

    if (getIndexOfElementToConstruct(State, E, LCtx))
      State = removeIndexOfElementToConstruct(State, E, LCtx);
  }

  if (getPendingArrayDestruction(State, LCtx))
    State = removePendingArrayDestruction(State, LCtx);

  return State;
}

/// The call exit is simulated with a sequence of nodes, which occur between
/// CallExitBegin and CallExitEnd. The following operations occur between the
/// two program points:
/// 1. CallExitBegin (triggers the start of call exit sequence)
/// 2. Bind the return value
/// 3. Run Remove dead bindings to clean up the dead symbols from the callee.
/// 4. CallExitEnd (switch to the caller context)
/// 5. PostStmt<CallExpr>
void ExprEngine::processCallExit(ExplodedNode *CEBNode) {
  // Step 1 CEBNode was generated before the call.
  PrettyStackTraceLocationContext CrashInfo(CEBNode->getLocationContext());
  const StackFrameContext *calleeCtx = CEBNode->getStackFrame();

  // The parent context might not be a stack frame, so make sure we
  // look up the first enclosing stack frame.
  const StackFrameContext *callerCtx =
    calleeCtx->getParent()->getStackFrame();

  const Stmt *CE = calleeCtx->getCallSite();
  ProgramStateRef state = CEBNode->getState();

  const Decl *ExitedD = calleeCtx ? calleeCtx->getDecl() : nullptr;
  const FunctionDecl *ExitedFD =
      dyn_cast_or_null<FunctionDecl>(ExitedD);

  const Expr *CallE = dyn_cast_or_null<Expr>(CE);

  // Find the last statement in the function and the corresponding basic block.
  const Stmt *LastSt = nullptr;
  const CFGBlock *Blk = nullptr;
  std::tie(LastSt, Blk) = getLastStmt(CEBNode);

  // Generate a CallEvent /before/ cleaning the state, so that we can get the
  // correct value for 'this' (if necessary).
  CallEventManager &CEMgr = getStateManager().getCallEventManager();
  CallEventRef<> Call = CEMgr.getCaller(calleeCtx, state);

  // Step 2: generate node with bound return value: CEBNode -> BindedRetNode.

  // A configured callback uses the modeled API expression as a synthetic
  // call site.  Its return value has no destination in the YAML invocation
  // and must not overwrite the already modeled result of that API.
  const configured_callback::Sequence *ActiveConfiguredSequence =
      state->get<ActiveConfiguredCallbackSequence>();
  unsigned ActiveConfiguredIndex =
      state->get<ActiveConfiguredCallbackIndex>();
  const StackFrameContext *ActiveConfiguredFrame =
      state->get<ActiveConfiguredCallbackFrame>();
  bool IsConfiguredCallbackExit =
      ExitedFD && ActiveConfiguredSequence &&
      ActiveConfiguredIndex < ActiveConfiguredSequence->Count &&
      ActiveConfiguredFrame == calleeCtx &&
      ActiveConfiguredSequence->Invocations[ActiveConfiguredIndex].Callee &&
      ActiveConfiguredSequence->Invocations[ActiveConfiguredIndex]
              .Callee->getCanonicalDecl() ==
          ExitedFD->getCanonicalDecl();

  // If this variable is set to 'true' the analyzer will evaluate the call
  // statement we are about to exit again, instead of continuing the execution
  // from the statement after the call. This is useful for non-POD type array
  // construction where the CXXConstructExpr is referenced only once in the CFG,
  // but we want to evaluate it as many times as many elements the array has.
  bool ShouldRepeatCall = false;

  if (const auto *DtorDecl =
          dyn_cast_or_null<CXXDestructorDecl>(Call->getDecl())) {
    if (auto Idx = getPendingArrayDestruction(state, callerCtx)) {
      ShouldRepeatCall = *Idx > 0;

      auto ThisVal = svalBuilder.getCXXThis(DtorDecl->getParent(), calleeCtx);
      state = state->killBinding(ThisVal);
    }
  }

  // If the callee returns an expression, bind its value to CallExpr.
  if (CE) {
    if (!IsConfiguredCallbackExit) {
      if (const ReturnStmt *RS = dyn_cast_or_null<ReturnStmt>(LastSt)) {
        const LocationContext *LCtx = CEBNode->getLocationContext();
        SVal V = state->getSVal(RS, LCtx);

        // Ensure that the return type matches the type of the returned Expr.
        if (wasDifferentDeclUsedForInlining(Call, calleeCtx)) {
          QualType ReturnedTy =
              CallEvent::getDeclaredResultType(calleeCtx->getDecl());
          if (!ReturnedTy.isNull()) {
            if (const Expr *Ex = dyn_cast<Expr>(CE)) {
              V = adjustReturnValue(V, Ex->getType(), ReturnedTy,
                                    getStoreManager());
            }
          }
        }

        state = state->BindExpr(CE, callerCtx, V);
      }
    }

    // Bind the constructed object value to CXXConstructExpr.
    if (const CXXConstructExpr *CCE = dyn_cast<CXXConstructExpr>(CE)) {
      loc::MemRegionVal This =
        svalBuilder.getCXXThis(CCE->getConstructor()->getParent(), calleeCtx);
      SVal ThisV = state->getSVal(This);
      ThisV = state->getSVal(ThisV.castAs<Loc>());
      state = state->BindExpr(CCE, callerCtx, ThisV);

      ShouldRepeatCall = shouldRepeatCtorCall(state, CCE, callerCtx);
    }

    if (const auto *CNE = dyn_cast<CXXNewExpr>(CE)) {
      // We are currently evaluating a CXXNewAllocator CFGElement. It takes a
      // while to reach the actual CXXNewExpr element from here, so keep the
      // region for later use.
      // Additionally cast the return value of the inlined operator new
      // (which is of type 'void *') to the correct object type.
      SVal AllocV = state->getSVal(CNE, callerCtx);
      AllocV = svalBuilder.evalCast(
          AllocV, CNE->getType(),
          getContext().getPointerType(getContext().VoidTy));

      state = addObjectUnderConstruction(state, CNE, calleeCtx->getParent(),
                                         AllocV);
    }
  }

  if (!ShouldRepeatCall) {
    state = removeStateTraitsUsedForArrayEvaluation(
        state, dyn_cast_or_null<CXXConstructExpr>(CE), callerCtx);
  }

  // Step 3: BindedRetNode -> CleanedNodes
  // If we can find a statement and a block in the inlined function, run remove
  // dead bindings before returning from the call. This is important to ensure
  // that we report the issues such as leaks in the stack contexts in which
  // they occurred.
  ExplodedNodeSet CleanedNodes;
  if (LastSt && Blk && AMgr.options.AnalysisPurgeOpt != PurgeNone) {
    static SimpleProgramPointTag retValBind("ExprEngine", "Bind Return Value");
    PostStmt Loc(LastSt, calleeCtx, &retValBind);
    bool isNew;
    ExplodedNode *BindedRetNode = G.getNode(Loc, state, false, &isNew);
    BindedRetNode->addPredecessor(CEBNode, G);
    if (!isNew)
      return;

    NodeBuilderContext Ctx(getCoreEngine(), Blk, BindedRetNode);
    currBldrCtx = &Ctx;
    // Here, we call the Symbol Reaper with 0 statement and callee location
    // context, telling it to clean up everything in the callee's context
    // (and its children). We use the callee's function body as a diagnostic
    // statement, with which the program point will be associated.
    removeDead(BindedRetNode, CleanedNodes, nullptr, calleeCtx,
               calleeCtx->getAnalysisDeclContext()->getBody(),
               ProgramPoint::PostStmtPurgeDeadSymbolsKind);
    currBldrCtx = nullptr;
  } else {
    CleanedNodes.Add(CEBNode);
  }

  for (ExplodedNode *N : CleanedNodes) {
    // Step 4: Generate the CallExit and leave the callee's context.
    // CleanedNodes -> CEENode
    CallExitEnd Loc(calleeCtx, callerCtx);
    bool isNew;
    ProgramStateRef CEEState = (N == CEBNode) ? state : N->getState();

    ExplodedNode *CEENode = G.getNode(Loc, CEEState, false, &isNew);
    CEENode->addPredecessor(N, G);
    if (!isNew)
      return;

    // Step 5: Perform the post-condition check of the CallExpr and enqueue the
    // result onto the work list.
    // CEENode -> Dst -> WorkList
    NodeBuilderContext Ctx(Engine, calleeCtx->getCallSiteBlock(), CEENode);
    SaveAndRestore<const NodeBuilderContext *> NBCSave(currBldrCtx, &Ctx);
    SaveAndRestore CBISave(currStmtIdx, calleeCtx->getIndex());

    CallEventRef<> UpdatedCall = Call.cloneWithState(CEEState);

    ExplodedNodeSet DstPostCall;
    if (llvm::isa_and_nonnull<CXXNewExpr>(CE)) {
      ExplodedNodeSet DstPostPostCallCallback;
      getCheckerManager().runCheckersForPostCall(DstPostPostCallCallback,
                                                 CEENode, *UpdatedCall, *this,
                                                 /*wasInlined=*/true);
      for (ExplodedNode *I : DstPostPostCallCallback) {
        getCheckerManager().runCheckersForNewAllocator(
            cast<CXXAllocatorCall>(*UpdatedCall), DstPostCall, I, *this,
            /*wasInlined=*/true);
      }
    } else {
      getCheckerManager().runCheckersForPostCall(DstPostCall, CEENode,
                                                 *UpdatedCall, *this,
                                                 /*wasInlined=*/true);
    }
    ExplodedNodeSet Dst;
    if (const ObjCMethodCall *Msg = dyn_cast<ObjCMethodCall>(Call)) {
      getCheckerManager().runCheckersForPostObjCMessage(Dst, DstPostCall, *Msg,
                                                        *this,
                                                        /*wasInlined=*/true);
    } else if (CE &&
               !(isa<CXXNewExpr>(CE) && // Called when visiting CXXNewExpr.
                 AMgr.getAnalyzerOptions().MayInlineCXXAllocator)) {
      getCheckerManager().runCheckersForPostStmt(Dst, DstPostCall, CE,
                                                 *this, /*wasInlined=*/true);
    } else {
      Dst.insert(DstPostCall);
    }

    // Enqueue the next element in the block.
    // If we are returning from a synthetic uv work_cb, schedule the saved
    // after_work_cb on the state after work_cb has returned.
    ExplodedNodeSet DstAfterUvAfterCallback;
    if (!ExitedFD || !UvWorkCallbackDecls.count(ExitedFD->getCanonicalDecl())) {
      ++NumUvWorkExitFastPathSkips;
      DstAfterUvAfterCallback.insert(Dst);
    } else {
      ++NumUvWorkExitStateLookups;
      processPendingUvAfterCallbackOnWorkExit(DstAfterUvAfterCallback, Dst,
                                              ExitedFD, CallE);
    }

    ExplodedNodeSet DstAfterConfiguredCallback;
    if (!ExitedFD ||
        !ConfiguredCallbackDecls.count(ExitedFD->getCanonicalDecl())) {
      DstAfterConfiguredCallback.insert(DstAfterUvAfterCallback);
    } else {
      processConfiguredCallbackExit(DstAfterConfiguredCallback,
                                    DstAfterUvAfterCallback, ExitedFD,
                                    calleeCtx);
    }

    // Enqueue the next element in the block.
    for (ExplodedNodeSet::iterator PSI = DstAfterConfiguredCallback.begin(),
                                   PSE = DstAfterConfiguredCallback.end();
         PSI != PSE; ++PSI) {
      unsigned Idx = calleeCtx->getIndex() + (ShouldRepeatCall ? 0 : 1);

      Engine.getWorkList()->enqueue(*PSI, calleeCtx->getCallSiteBlock(), Idx);
    }
  }
}

bool ExprEngine::isSmall(AnalysisDeclContext *ADC) const {
  // When there are no branches in the function, it means that there's no
  // exponential complexity introduced by inlining such function.
  // Such functions also don't trigger various fundamental problems
  // with our inlining mechanism, such as the problem of
  // inlined defensive checks. Hence isLinear().
  const CFG *Cfg = ADC->getCFG();
  return Cfg->isLinear() || Cfg->size() <= AMgr.options.AlwaysInlineSize;
}

bool ExprEngine::isLarge(AnalysisDeclContext *ADC) const {
  const CFG *Cfg = ADC->getCFG();
  return Cfg->size() >= AMgr.options.MinCFGSizeTreatFunctionsAsLarge;
}

bool ExprEngine::isHuge(AnalysisDeclContext *ADC) const {
  const CFG *Cfg = ADC->getCFG();
  return Cfg->getNumBlockIDs() > AMgr.options.MaxInlinableSize;
}

void ExprEngine::examineStackFrames(const Decl *D, const LocationContext *LCtx,
                               bool &IsRecursive, unsigned &StackDepth) {
  IsRecursive = false;
  StackDepth = 0;

  while (LCtx) {
    if (const StackFrameContext *SFC = dyn_cast<StackFrameContext>(LCtx)) {
      const Decl *DI = SFC->getDecl();

      // Mark recursive (and mutually recursive) functions and always count
      // them when measuring the stack depth.
      if (DI == D) {
        IsRecursive = true;
        ++StackDepth;
        LCtx = LCtx->getParent();
        continue;
      }

      // Do not count the small functions when determining the stack depth.
      AnalysisDeclContext *CalleeADC = AMgr.getAnalysisDeclContext(DI);
      if (!isSmall(CalleeADC))
        ++StackDepth;
    }
    LCtx = LCtx->getParent();
  }
}

// The GDM component containing the dynamic dispatch bifurcation info. When
// the exact type of the receiver is not known, we want to explore both paths -
// one on which we do inline it and the other one on which we don't. This is
// done to ensure we do not drop coverage.
// This is the map from the receiver region to a bool, specifying either we
// consider this region's information precise or not along the given path.
namespace {
  enum DynamicDispatchMode {
    DynamicDispatchModeInlined = 1,
    DynamicDispatchModeConservative
  };
} // end anonymous namespace

REGISTER_MAP_WITH_PROGRAMSTATE(DynamicDispatchBifurcationMap,
                               const MemRegion *, unsigned)
REGISTER_TRAIT_WITH_PROGRAMSTATE(CTUDispatchBifurcation, bool)

void ExprEngine::ctuBifurcate(const CallEvent &Call, const Decl *D,
                              NodeBuilder &Bldr, ExplodedNode *Pred,
                              ProgramStateRef State) {
  ProgramStateRef ConservativeEvalState = nullptr;
  if (Call.isForeign() && !isSecondPhaseCTU()) {
    const auto IK = AMgr.options.getCTUPhase1Inlining();
    const bool DoInline = IK == CTUPhase1InliningKind::All ||
                          (IK == CTUPhase1InliningKind::Small &&
                           isSmall(AMgr.getAnalysisDeclContext(D)));
    if (DoInline) {
      inlineCall(Engine.getWorkList(), Call, D, Bldr, Pred, State);
      return;
    }
    const bool BState = State->get<CTUDispatchBifurcation>();
    if (!BState) { // This is the first time we see this foreign function.
      // Enqueue it to be analyzed in the second (ctu) phase.
      inlineCall(Engine.getCTUWorkList(), Call, D, Bldr, Pred, State);
      // Conservatively evaluate in the first phase.
      ConservativeEvalState = State->set<CTUDispatchBifurcation>(true);
      CSA_setBreakInfo(D, CSABreakKind::CTUPhase1ConservativeEval);
      conservativeEvalCall(Call, Bldr, Pred, ConservativeEvalState);
    } else {
      CSA_setBreakInfo(D, CSABreakKind::CTUDispatchBifurcationReused);
      conservativeEvalCall(Call, Bldr, Pred, State);
    }
    return;
  }
  inlineCall(Engine.getWorkList(), Call, D, Bldr, Pred, State);
}

void ExprEngine::inlineCall(WorkList *WList, const CallEvent &Call,
                            const Decl *D, NodeBuilder &Bldr,
                            ExplodedNode *Pred, ProgramStateRef State) {
  assert(D);

  const LocationContext *CurLC = Pred->getLocationContext();
  const StackFrameContext *CallerSFC = CurLC->getStackFrame();
  const LocationContext *ParentOfCallee = CallerSFC;
  if (Call.getKind() == CE_Block &&
      !cast<BlockCall>(Call).isConversionFromLambda()) {
    const BlockDataRegion *BR = cast<BlockCall>(Call).getBlockRegion();
    assert(BR && "If we have the block definition we should have its region");
    AnalysisDeclContext *BlockCtx = AMgr.getAnalysisDeclContext(D);
    ParentOfCallee = BlockCtx->getBlockInvocationContext(CallerSFC,
                                                         cast<BlockDecl>(D),
                                                         BR);
  }

  // This may be NULL, but that's fine.
  const Expr *CallE = Call.getOriginExpr();

  // Construct a new stack frame for the callee.
  AnalysisDeclContext *CalleeADC = AMgr.getAnalysisDeclContext(D);
  const StackFrameContext *CalleeSFC =
      CalleeADC->getStackFrame(ParentOfCallee, CallE, currBldrCtx->getBlock(),
                               currBldrCtx->blockCount(), currStmtIdx);

  CallEnter Loc(CallE, CalleeSFC, CurLC);

  // Construct a new state which contains the mapping from actual to
  // formal arguments.
  State = State->enterStackFrame(Call, CalleeSFC);

  bool isNew;
  if (ExplodedNode *N = G.getNode(Loc, State, false, &isNew)) {
    N->addPredecessor(Pred, G);
    if (isNew)
      WList->enqueue(N);
  }

  // If we decided to inline the call, the successor has been manually
  // added onto the work list so remove it from the node builder.
  Bldr.takeNodes(Pred);

  NumInlinedCalls++;
  AnalysisProgress::recordInlinedCall();
  Engine.FunctionSummaries->bumpNumTimesInlined(D);

  // Do not mark as visited in the 2nd run (CTUWList), so the function will
  // be visited as top-level, this way we won't loose reports in non-ctu
  // mode. Considering the case when a function in a foreign TU calls back
  // into the main TU.
  // Note, during the 1st run, it doesn't matter if we mark the foreign
  // functions as visited (or not) because they can never appear as a top level
  // function in the main TU.
  if (!isSecondPhaseCTU())
    // Mark the decl as visited.
    if (VisitedCallees)
      VisitedCallees->insert(D);
}

static ProgramStateRef getInlineFailedState(ProgramStateRef State,
                                            const Stmt *CallE) {
  const void *ReplayState = State->get<ReplayWithoutInlining>();
  if (!ReplayState)
    return nullptr;

  assert(ReplayState == CallE && "Backtracked to the wrong call.");
  (void)CallE;

  return State->remove<ReplayWithoutInlining>();
}

void ExprEngine::VisitCallExpr(const CallExpr *CE, ExplodedNode *Pred,
                               ExplodedNodeSet &dst) {
  // Perform the previsit of the CallExpr.
  ExplodedNodeSet dstPreVisit;
  getCheckerManager().runCheckersForPreStmt(dstPreVisit, Pred, CE, *this);

  // Get the call in its initial state. We use this as a template to perform
  // all the checks.
  CallEventManager &CEMgr = getStateManager().getCallEventManager();
  CallEventRef<> CallTemplate = CEMgr.getSimpleCall(
      CE, Pred->getState(), Pred->getLocationContext(), getCFGElementRef());

  if (CSA_VECTOR_shouldLogCall(*CallTemplate)) {
    SourceManager &SM = AMgr.getASTContext().getSourceManager();

    llvm::errs() << "[CSA_VECTOR][VisitCallExpr] loc=";
    CSA_VECTOR_printLoc(SM, CE->getExprLoc());

    llvm::errs() << " callee="
                 << CSA_VECTOR_getDeclName(CallTemplate->getDecl());

    llvm::errs() << " kind="
                 << CallTemplate->getKindAsString();

    llvm::errs() << " expr=\"";
    CE->printPretty(llvm::errs(), nullptr,
                    AMgr.getASTContext().getPrintingPolicy());
    llvm::errs() << "\"\n";

    for (unsigned I = 0; I < CallTemplate->getNumArgs(); ++I) {
      llvm::errs() << "[CSA_VECTOR][VisitCallExpr]   arg" << I << "=";
      CallTemplate->getArgSVal(I).dumpToStream(llvm::errs());
      llvm::errs() << "\n";
    }
  }

  // Evaluate the function call.  We try each of the checkers
  // to see if the can evaluate the function call.
  ExplodedNodeSet dstCallEvaluated;
  for (ExplodedNode *N : dstPreVisit) {
    evalCall(dstCallEvaluated, N, *CallTemplate);
  }

  // Finally, perform the post-condition check of the CallExpr and store
  // the created nodes in 'Dst'.
  // Note that if the call was inlined, dstCallEvaluated will be empty.
  // The post-CallExpr check will occur in processCallExit.
  getCheckerManager().runCheckersForPostStmt(dst, dstCallEvaluated, CE,
                                             *this);
}

ProgramStateRef ExprEngine::finishArgumentConstruction(ProgramStateRef State,
                                                       const CallEvent &Call) {
  const Expr *E = Call.getOriginExpr();
  // FIXME: Constructors to placement arguments of operator new
  // are not supported yet.
  if (!E || isa<CXXNewExpr>(E))
    return State;

  const LocationContext *LC = Call.getLocationContext();
  for (unsigned CallI = 0, CallN = Call.getNumArgs(); CallI != CallN; ++CallI) {
    unsigned I = Call.getASTArgumentIndex(CallI);
    if (std::optional<SVal> V = getObjectUnderConstruction(State, {E, I}, LC)) {
      SVal VV = *V;
      (void)VV;
      assert(cast<VarRegion>(VV.castAs<loc::MemRegionVal>().getRegion())
                 ->getStackFrame()->getParent()
                 ->getStackFrame() == LC->getStackFrame());
      State = finishObjectConstruction(State, {E, I}, LC);
    }
  }

  return State;
}

void ExprEngine::finishArgumentConstruction(ExplodedNodeSet &Dst,
                                            ExplodedNode *Pred,
                                            const CallEvent &Call) {
  ProgramStateRef State = Pred->getState();
  ProgramStateRef CleanedState = finishArgumentConstruction(State, Call);
  if (CleanedState == State) {
    Dst.insert(Pred);
    return;
  }

  const Expr *E = Call.getOriginExpr();
  const LocationContext *LC = Call.getLocationContext();
  NodeBuilder B(Pred, Dst, *currBldrCtx);
  static SimpleProgramPointTag Tag("ExprEngine",
                                   "Finish argument construction");
  PreStmt PP(E, LC, &Tag);
  B.generateNode(PP, CleanedState, Pred);
}

void ExprEngine::evalCall(ExplodedNodeSet &Dst, ExplodedNode *Pred,
                          const CallEvent &Call) {
  // WARNING: At this time, the state attached to 'Call' may be older than the
  // state in 'Pred'. This is a minor optimization since CheckerManager will
  // use an updated CallEvent instance when calling checkers, but if 'Call' is
  // ever used directly in this function all callers should be updated to pass
  // the most recent state. (It is probably not worth doing the work here since
  // for some callers this will not be necessary.)

  // Run any pre-call checks using the generic call interface.
  ExplodedNodeSet dstPreVisit;
  getCheckerManager().runCheckersForPreCall(dstPreVisit, Pred,
                                            Call, *this);

  // Actually evaluate the function call.  We try each of the checkers
  // to see if the can evaluate the function call, and get a callback at
  // defaultEvalCall if all of them fail.
  ExplodedNodeSet dstCallEvaluated;
  getCheckerManager().runCheckersForEvalCall(dstCallEvaluated, dstPreVisit,
                                             Call, *this, EvalCallOptions());

  // If there were other constructors called for object-type arguments
  // of this call, clean them up.
  ExplodedNodeSet dstArgumentCleanup;
  for (ExplodedNode *I : dstCallEvaluated)
    finishArgumentConstruction(dstArgumentCleanup, I, Call);

  ExplodedNodeSet dstPostCall;
  getCheckerManager().runCheckersForPostCall(dstPostCall, dstArgumentCleanup,
                                             Call, *this);

  ExplodedNodeSet dstAfterPendingUvCallback;
  processPendingUvCallback(dstAfterPendingUvCallback, dstPostCall, Call);

  // Escaping symbols conjured during invalidating the regions above.
  // Note that, for inlined calls the nodes were put back into the worklist,
  // so we can assume that every node belongs to a conservative call at this
  // point.

  // Run pointerEscape callback with the newly conjured symbols.
  SmallVector<std::pair<SVal, SVal>, 8> Escaped;
  ExplodedNodeSet dstAfterPointerEscape;
  for (ExplodedNode *I : dstAfterPendingUvCallback) {
    NodeBuilder B(I, dstAfterPointerEscape, *currBldrCtx);
    ProgramStateRef State = I->getState();
    Escaped.clear();
    {
      unsigned Arg = -1;
      for (const ParmVarDecl *PVD : Call.parameters()) {
        ++Arg;
        QualType ParamTy = PVD->getType();
        if (ParamTy.isNull() ||
            (!ParamTy->isPointerType() && !ParamTy->isReferenceType()))
          continue;
        QualType Pointee = ParamTy->getPointeeType();
        if (Pointee.isConstQualified() || Pointee->isVoidType())
          continue;
        if (const MemRegion *MR = Call.getArgSVal(Arg).getAsRegion())
          Escaped.emplace_back(loc::MemRegionVal(MR), State->getSVal(MR, Pointee));
      }
    }

    State = processPointerEscapedOnBind(State, Escaped, I->getLocationContext(),
                                        PSK_EscapeOutParameters, &Call);

    if (State == I->getState())
      dstAfterPointerEscape.insert(I);
    else
      B.generateNode(I->getLocation(), State, I);
  }

  // Configured callbacks run after the modeled call's ordinary post-call and
  // pointer-escape processing. Paths with a pending sequence enter the first
  // synthetic callback; all other paths flow directly into Dst.
  processPendingConfiguredCallbacks(Dst, dstAfterPointerEscape);
}

ProgramStateRef ExprEngine::bindReturnValue(const CallEvent &Call,
                                            const LocationContext *LCtx,
                                            ProgramStateRef State) {
  const Expr *E = Call.getOriginExpr();
  if (!E)
    return State;

  // Some method families have known return values.
  if (const ObjCMethodCall *Msg = dyn_cast<ObjCMethodCall>(&Call)) {
    switch (Msg->getMethodFamily()) {
    default:
      break;
    case OMF_autorelease:
    case OMF_retain:
    case OMF_self: {
      // These methods return their receivers.
      return State->BindExpr(E, LCtx, Msg->getReceiverSVal());
    }
    }
  } else if (const CXXConstructorCall *C = dyn_cast<CXXConstructorCall>(&Call)){
    SVal ThisV = C->getCXXThisVal();
    ThisV = State->getSVal(ThisV.castAs<Loc>());
    return State->BindExpr(E, LCtx, ThisV);
  }

  SVal R;
  QualType ResultTy = Call.getResultType();
  unsigned Count = currBldrCtx->blockCount();
  if (auto RTC = getCurrentCFGElement().getAs<CFGCXXRecordTypedCall>()) {
    // Conjure a temporary if the function returns an object by value.
    SVal Target;
    assert(RTC->getStmt() == Call.getOriginExpr());
    EvalCallOptions CallOpts; // FIXME: We won't really need those.
    std::tie(State, Target) = handleConstructionContext(
        Call.getOriginExpr(), State, currBldrCtx, LCtx,
        RTC->getConstructionContext(), CallOpts);
    const MemRegion *TargetR = Target.getAsRegion();
    assert(TargetR);
    // Invalidate the region so that it didn't look uninitialized. If this is
    // a field or element constructor, we do not want to invalidate
    // the whole structure. Pointer escape is meaningless because
    // the structure is a product of conservative evaluation
    // and therefore contains nothing interesting at this point.
    RegionAndSymbolInvalidationTraits ITraits;
    ITraits.setTrait(TargetR,
        RegionAndSymbolInvalidationTraits::TK_DoNotInvalidateSuperRegion);
    State = State->invalidateRegions(TargetR, E, Count, LCtx,
                                     /* CausesPointerEscape=*/false, nullptr,
                                     &Call, &ITraits);

    R = State->getSVal(Target.castAs<Loc>(), E->getType());
  } else {
    // Conjure a symbol if the return value is unknown.

    // See if we need to conjure a heap pointer instead of
    // a regular unknown pointer.
    const auto *CNE = dyn_cast<CXXNewExpr>(E);
    if (CNE && CNE->getOperatorNew()->isReplaceableGlobalAllocationFunction()) {
      R = svalBuilder.getConjuredHeapSymbolVal(E, LCtx, Count);
      const MemRegion *MR = R.getAsRegion()->StripCasts();

      // Store the extent of the allocated object(s).
      SVal ElementCount;
      if (const Expr *SizeExpr = CNE->getArraySize().value_or(nullptr)) {
        ElementCount = State->getSVal(SizeExpr, LCtx);
      } else {
        ElementCount = svalBuilder.makeIntVal(1, /*IsUnsigned=*/true);
      }

      SVal ElementSize = getElementExtent(CNE->getAllocatedType(), svalBuilder);

      SVal Size =
          svalBuilder.evalBinOp(State, BO_Mul, ElementCount, ElementSize,
                                svalBuilder.getArrayIndexType());

      // FIXME: This line is to prevent a crash. For more details please check
      // issue #56264.
      if (Size.isUndef())
        Size = UnknownVal();

      State = setDynamicExtent(State, MR, Size.castAs<DefinedOrUnknownSVal>(),
                               svalBuilder);
    } else {
      R = svalBuilder.conjureSymbolVal(nullptr, E, LCtx, ResultTy, Count);
    }
  }
  return State->BindExpr(E, LCtx, R);
}

// Conservatively evaluate call by invalidating regions and binding
// a conjured return value.
void ExprEngine::conservativeEvalCall(const CallEvent &Call, NodeBuilder &Bldr,
                                      ExplodedNode *Pred, ProgramStateRef State) {
  State = Call.invalidateRegions(currBldrCtx->blockCount(), State);
  State = bindReturnValue(Call, Pred->getLocationContext(), State);

  // And make the result node.
  static SimpleProgramPointTag PT("ExprEngine", "Conservative eval call");
  const ProgramPointTag *Tag = CSA_BREAK_CHECK()
                                   ? static_cast<const ProgramPointTag *>(
                                         CSA_makeBreakNoteTag(*this, Call))
                                   : &PT;
  Bldr.generateNode(Call.getProgramPoint(/*IsPreVisit=*/false, Tag), State,
                    Pred);
  CSA_clearBreakInfo();
}

ExprEngine::CallInlinePolicy
ExprEngine::mayInlineCallKind(const CallEvent &Call, const ExplodedNode *Pred,
                              AnalyzerOptions &Opts,
                              const EvalCallOptions &CallOpts) {
  const Decl *D = Call.getDecl();
  const LocationContext *CurLC = Pred->getLocationContext();
  const StackFrameContext *CallerSFC = CurLC->getStackFrame();
  switch (Call.getKind()) {
  case CE_Function:
  case CE_Block:
    break;
  case CE_CXXMember:
  case CE_CXXMemberOperator:
    if (!Opts.mayInlineCXXMemberFunction(CIMK_MemberFunctions)) {
      CSA_setBreakInfo(D, CSABreakKind::CXXMemberFunctionInliningDisabled);
      return CIP_DisallowedAlways;
    }
    break;
  case CE_CXXConstructor: {
    if (!Opts.mayInlineCXXMemberFunction(CIMK_Constructors)) {
      CSA_setBreakInfo(D, CSABreakKind::CXXConstructorInliningDisabled);
      return CIP_DisallowedAlways;
    }

    const CXXConstructorCall &Ctor = cast<CXXConstructorCall>(Call);

    const CXXConstructExpr *CtorExpr = Ctor.getOriginExpr();

    auto CCE = getCurrentCFGElement().getAs<CFGConstructor>();
    const ConstructionContext *CC = CCE ? CCE->getConstructionContext()
                                        : nullptr;

    if (llvm::isa_and_nonnull<NewAllocatedObjectConstructionContext>(CC) &&
        !Opts.MayInlineCXXAllocator) {
      CSA_setBreakInfo(
          D,
          CSABreakKind::NewAllocatedObjectConstructorAllocatorInliningDisabled);
      return CIP_DisallowedOnce;
    }

    if (CallOpts.IsArrayCtorOrDtor) {
      if (!shouldInlineArrayConstruction(Pred->getState(), CtorExpr, CurLC)) {
        CSA_setBreakInfo(D,
                         CSABreakKind::ArrayConstructorOrDestructorUnsupported);
        return CIP_DisallowedOnce;
      }
    }

    // Inlining constructors requires including initializers in the CFG.
    const AnalysisDeclContext *ADC = CallerSFC->getAnalysisDeclContext();
    assert(ADC->getCFGBuildOptions().AddInitializers && "No CFG initializers");
    (void)ADC;

    // If the destructor is trivial, it's always safe to inline the constructor.
    if (Ctor.getDecl()->getParent()->hasTrivialDestructor())
      break;

    // For other types, only inline constructors if destructor inlining is
    // also enabled.
    if (!Opts.mayInlineCXXMemberFunction(CIMK_Destructors)) {
      CSA_setBreakInfo(D, CSABreakKind::ConstructorRequiresDestructorInlining);
      return CIP_DisallowedAlways;
    }

    if (CtorExpr->getConstructionKind() == CXXConstructionKind::Complete) {
      // If we don't handle temporary destructors, we shouldn't inline
      // their constructors.
      if (CallOpts.IsTemporaryCtorOrDtor &&
          !Opts.ShouldIncludeTemporaryDtorsInCFG) {
        CSA_setBreakInfo(
            D,
            CSABreakKind::TemporaryConstructorWithoutTemporaryDtorsInCFG);
        return CIP_DisallowedOnce;
      }

      // If we did not find the correct this-region, it would be pointless
      // to inline the constructor. Instead we will simply invalidate
      // the fake temporary target.
      if (CallOpts.IsCtorOrDtorWithImproperlyModeledTargetRegion) {
        CSA_setBreakInfo(D,
                         CSABreakKind::ImproperlyModeledCtorDtorTargetRegion);
        return CIP_DisallowedOnce;
      }

      // If the temporary is lifetime-extended by binding it to a reference-type
      // field within an aggregate, automatic destructors don't work properly.
      if (CallOpts.IsTemporaryLifetimeExtendedViaAggregate) {
        CSA_setBreakInfo(
            D,
            CSABreakKind::TemporaryLifetimeExtendedViaAggregateUnsupported);
        return CIP_DisallowedOnce;
      }
    }

    break;
  }
  case CE_CXXInheritedConstructor: {
    // This doesn't really increase the cost of inlining ever, because
    // the stack frame of the inherited constructor is trivial.
    return CIP_Allowed;
  }
  case CE_CXXDestructor: {
    if (!Opts.mayInlineCXXMemberFunction(CIMK_Destructors)) {
      CSA_setBreakInfo(D, CSABreakKind::CXXDestructorInliningDisabled);
      return CIP_DisallowedAlways;
    }

    // Inlining destructors requires building the CFG correctly.
    const AnalysisDeclContext *ADC = CallerSFC->getAnalysisDeclContext();
    assert(ADC->getCFGBuildOptions().AddImplicitDtors && "No CFG destructors");
    (void)ADC;

    if (CallOpts.IsArrayCtorOrDtor) {
      if (!shouldInlineArrayDestruction(getElementCountOfArrayBeingDestructed(
              Call, Pred->getState(), svalBuilder))) {
        CSA_setBreakInfo(D, CSABreakKind::ArrayDestructorUnsupported);
        return CIP_DisallowedOnce;
      }
    }

    // Allow disabling temporary destructor inlining with a separate option.
    if (CallOpts.IsTemporaryCtorOrDtor &&
        !Opts.MayInlineCXXTemporaryDtors) {
      CSA_setBreakInfo(D, CSABreakKind::TemporaryDestructorInliningDisabled);
      return CIP_DisallowedOnce;
    }

    // If we did not find the correct this-region, it would be pointless
    // to inline the destructor. Instead we will simply invalidate
    // the fake temporary target.
    if (CallOpts.IsCtorOrDtorWithImproperlyModeledTargetRegion) {
      CSA_setBreakInfo(D,
                       CSABreakKind::ImproperlyModeledDestructorTargetRegion);
      return CIP_DisallowedOnce;
    }
    break;
  }
  case CE_CXXDeallocator:
    [[fallthrough]];
  case CE_CXXAllocator:
    if (Opts.MayInlineCXXAllocator)
      break;
    // Do not inline allocators until we model deallocators.
    // This is unfortunate, but basically necessary for smart pointers and such.
    CSA_setBreakInfo(D, CSABreakKind::CXXAllocatorInliningDisabled);
    return CIP_DisallowedAlways;
  case CE_ObjCMessage:
    if (!Opts.MayInlineObjCMethod) {
      CSA_setBreakInfo(D, CSABreakKind::ObjCMethodInliningDisabled);
      return CIP_DisallowedAlways;
    }
    if (!(Opts.getIPAMode() == IPAK_DynamicDispatch ||
          Opts.getIPAMode() == IPAK_DynamicDispatchBifurcate)) {
      CSA_setBreakInfo(D, CSABreakKind::ObjCMethodRequiresDynamicDispatchIPA);
      return CIP_DisallowedAlways;
    }
    break;
  }

  return CIP_Allowed;
}

/// Returns true if the given C++ class contains a member with the given name.
static bool hasMember(const ASTContext &Ctx, const CXXRecordDecl *RD,
                      StringRef Name) {
  const IdentifierInfo &II = Ctx.Idents.get(Name);
  return RD->hasMemberName(Ctx.DeclarationNames.getIdentifier(&II));
}

/// Returns true if the given C++ class is a container or iterator.
///
/// Our heuristic for this is whether it contains a method named 'begin()' or a
/// nested type named 'iterator' or 'iterator_category'.
static bool isContainerClass(const ASTContext &Ctx, const CXXRecordDecl *RD) {
  return hasMember(Ctx, RD, "begin") ||
         hasMember(Ctx, RD, "iterator") ||
         hasMember(Ctx, RD, "iterator_category");
}

/// Returns true if the given function refers to a method of a C++ container
/// or iterator.
///
/// We generally do a poor job modeling most containers right now, and might
/// prefer not to inline their methods.
static bool isContainerMethod(const ASTContext &Ctx,
                              const FunctionDecl *FD) {
  if (const CXXMethodDecl *MD = dyn_cast<CXXMethodDecl>(FD))
    return isContainerClass(Ctx, MD->getParent());
  return false;
}

/// Returns true if the given function is the destructor of a class named
/// "shared_ptr".
static bool isCXXSharedPtrDtor(const FunctionDecl *FD) {
  const CXXDestructorDecl *Dtor = dyn_cast<CXXDestructorDecl>(FD);
  if (!Dtor)
    return false;

  const CXXRecordDecl *RD = Dtor->getParent();
  if (const IdentifierInfo *II = RD->getDeclName().getAsIdentifierInfo())
    if (II->isStr("shared_ptr"))
        return true;

  return false;
}

/// Returns true if the function in \p CalleeADC may be inlined in general.
///
/// This checks static properties of the function, such as its signature and
/// CFG, to determine whether the analyzer should ever consider inlining it,
/// in any context.
bool ExprEngine::mayInlineDecl(AnalysisDeclContext *CalleeADC) const {
  AnalyzerOptions &Opts = AMgr.getAnalyzerOptions();
  const Decl *D = CalleeADC->getDecl();

  // FIXME: Do not inline variadic calls.
  if (CallEvent::isVariadic(D)) {
    CSA_setBreakInfo(D, CSABreakKind::VariadicFunction);
    return false;
  }

  // Check certain C++-related inlining policies.
  ASTContext &Ctx = CalleeADC->getASTContext();
  if (Ctx.getLangOpts().CPlusPlus) {
    if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(CalleeADC->getDecl())) {
      // Conditionally control the inlining of template functions.
      if (!Opts.MayInlineTemplateFunctions)
        if (FD->getTemplatedKind() != FunctionDecl::TK_NonTemplate) {
          if (CSA_VECTOR_shouldLogDecl(FD)) {
            llvm::errs() << "[CSA_VECTOR][mayInlineDecl] reject=TemplateFunctionInliningDisabled"
                        << " callee=" << CSA_VECTOR_getDeclName(FD)
                        << "\n";
          }

          CSA_setBreakInfo(D, CSABreakKind::TemplateFunctionInliningDisabled);
          return false;
        }

      // Conditionally control the inlining of C++ standard library functions.
      if (!Opts.MayInlineCXXStandardLibrary)
        if (Ctx.getSourceManager().isInSystemHeader(FD->getLocation()))
          if (AnalysisDeclContext::isInStdNamespace(FD)) {
            if (CSA_VECTOR_shouldLogDecl(FD)) {
              llvm::errs() << "[CSA_VECTOR][mayInlineDecl] reject=CXXStdLibraryInliningDisabled"
                          << " callee=" << CSA_VECTOR_getDeclName(FD)
                          << "\n";
            }

            CSA_setBreakInfo(D, CSABreakKind::CXXStdLibraryInliningDisabled);
            return false;
          }

      // Conditionally control the inlining of methods on objects that look
      // like C++ containers.
      if (!Opts.MayInlineCXXContainerMethods)
        if (!AMgr.isInCodeFile(FD->getLocation()))
          if (isContainerMethod(Ctx, FD)) {
            if (CSA_VECTOR_shouldLogDecl(FD)) {
              llvm::errs() << "[CSA_VECTOR][mayInlineDecl] reject=CXXContainerMethodInliningDisabled"
                          << " callee=" << CSA_VECTOR_getDeclName(FD)
                          << " MayInlineCXXContainerMethods="
                          << Opts.MayInlineCXXContainerMethods
                          << " isInCodeFile="
                          << AMgr.isInCodeFile(FD->getLocation())
                          << "\n";
            }

            CSA_setBreakInfo(D, CSABreakKind::CXXContainerMethodInliningDisabled);
            return false;
          }

      // Conditionally control the inlining of the destructor of C++ shared_ptr.
      // We don't currently do a good job modeling shared_ptr because we can't
      // see the reference count, so treating as opaque is probably the best
      // idea.
      if (!Opts.MayInlineCXXSharedPtrDtor)
        if (isCXXSharedPtrDtor(FD)) {
          CSA_setBreakInfo(D, CSABreakKind::CXXSharedPtrDtorInliningDisabled);
          return false;
        }
    }
  }

  // It is possible that the CFG cannot be constructed.
  // Be safe, and check if the CalleeCFG is valid.
  const CFG *CalleeCFG = CalleeADC->getCFG();
  if (!CalleeCFG) {
    if (CSA_VECTOR_shouldLogDecl(D)) {
      llvm::errs() << "[CSA_VECTOR][mayInlineDecl] reject=CFGConstructionFailed"
                  << " callee=" << CSA_VECTOR_getDeclName(D)
                  << "\n";
    }

    CSA_setBreakInfo(D, CSABreakKind::CFGConstructionFailed);
    return false;
  }

  // Do not inline large functions.
  if (isHuge(CalleeADC)) {
    if (CSA_VECTOR_shouldLogDecl(D)) {
      llvm::errs() << "[CSA_VECTOR][mayInlineDecl] reject=HugeCallee"
                  << " callee=" << CSA_VECTOR_getDeclName(D)
                  << "\n";
    }

    CSA_setBreakInfo(D, CSABreakKind::HugeCallee);
    return false;
  }

  // It is possible that the live variables analysis cannot be
  // run.  If so, bail out.
  if (!CalleeADC->getAnalysis<RelaxedLiveVariables>()) {
    if (CSA_VECTOR_shouldLogDecl(D)) {
      llvm::errs() << "[CSA_VECTOR][mayInlineDecl] reject=RelaxedLiveVariablesFailed"
                  << " callee=" << CSA_VECTOR_getDeclName(D)
                  << "\n";
    }

    CSA_setBreakInfo(D, CSABreakKind::RelaxedLiveVariablesFailed);
    return false;
  }

  return true;
}

bool ExprEngine::shouldInlineCall(const CallEvent &Call, const Decl *D,
                                  const ExplodedNode *Pred,
                                  const EvalCallOptions &CallOpts) {
  if (!D) {
    CSA_setBreakInfo(nullptr, CSABreakKind::UnknownCalleeDecl);
    return false;
  }

  AnalysisManager &AMgr = getAnalysisManager();
  AnalyzerOptions &Opts = AMgr.options;
  AnalysisDeclContextManager &ADCMgr = AMgr.getAnalysisDeclContextManager();
  AnalysisDeclContext *CalleeADC = ADCMgr.getContext(D);

  // The auto-synthesized bodies are essential to inline as they are
  // usually small and commonly used. Note: we should do this check early on to
  // ensure we always inline these calls.
  if (CalleeADC->isBodyAutosynthesized())
    return true;

  if (!AMgr.shouldInlineCall()) {
    CSA_setBreakInfo(D, CSABreakKind::GlobalInliningDisabled);
    return false;
  }

  // Check if this function has been marked as non-inlinable.
  std::optional<bool> MayInline = Engine.FunctionSummaries->mayInline(D);
  if (MayInline) {
    if (!*MayInline) {
      CSA_setBreakInfo(D, CSABreakKind::FunctionSummaryDisallowsInlining);
      return false;
    }

  } else {
    // We haven't actually checked the static properties of this function yet.
    // Do that now, and record our decision in the function summaries.
    if (mayInlineDecl(CalleeADC)) {
      Engine.FunctionSummaries->markMayInline(D);
    } else {
      if (!CSA_hasBreakInfo())
        CSA_setBreakInfo(D, CSABreakKind::StaticInlineabilityCheckFailed);
      Engine.FunctionSummaries->markShouldNotInline(D);
      return false;
    }
  }

  // Check if we should inline a call based on its kind.
  // FIXME: this checks both static and dynamic properties of the call, which
  // means we're redoing a bit of work that could be cached in the function
  // summary.
  CallInlinePolicy CIP = mayInlineCallKind(Call, Pred, Opts, CallOpts);
  if (CIP != CIP_Allowed) {
    if (!CSA_hasBreakInfo()) {
      if (CIP == CIP_DisallowedAlways)
        CSA_setBreakInfo(D, CSABreakKind::CallKindDisallowedAlways);
      else
        CSA_setBreakInfo(D, CSABreakKind::CallKindDisallowedOnce);
    }

    if (CIP == CIP_DisallowedAlways) {
      assert(!MayInline || *MayInline);
      Engine.FunctionSummaries->markShouldNotInline(D);
    }
    return false;
  }

  // Do not inline if recursive or we've reached max stack frame count.
  bool IsRecursive = false;
  unsigned StackDepth = 0;
  examineStackFrames(D, Pred->getLocationContext(), IsRecursive, StackDepth);
  if ((StackDepth >= Opts.InlineMaxStackDepth) &&
      (!isSmall(CalleeADC) || IsRecursive)) {
    CSA_setBreakInfo(D, CSABreakKind::RecursiveOrStackDepthLimitReached);
    return false;
  }

  // Do not inline large functions too many times.
  if ((Engine.FunctionSummaries->getNumTimesInlined(D) >
       Opts.MaxTimesInlineLarge) &&
      isLarge(CalleeADC)) {
    NumReachedInlineCountMax++;
    CSA_setBreakInfo(D, CSABreakKind::LargeCalleeInlineCountLimitReached);
    return false;
  }

  if (HowToInline == Inline_Minimal && (!isSmall(CalleeADC) || IsRecursive)) {
    CSA_setBreakInfo(D, CSABreakKind::MinimalInliningRejected);
    return false;
  }

  return true;
}

bool ExprEngine::shouldInlineArrayConstruction(const ProgramStateRef State,
                                               const CXXConstructExpr *CE,
                                               const LocationContext *LCtx) {
  if (!CE)
    return false;

  // FIXME: Handle other arrays types.
  if (const auto *CAT = dyn_cast<ConstantArrayType>(CE->getType())) {
    unsigned ArrSize = getContext().getConstantArrayElementCount(CAT);

    // This might seem conter-intuitive at first glance, but the functions are
    // closely related. Reasoning about destructors depends only on the type
    // of the expression that initialized the memory region, which is the
    // CXXConstructExpr. So to avoid code repetition, the work is delegated
    // to the function that reasons about destructor inlining. Also note that
    // if the constructors of the array elements are inlined, the destructors
    // can also be inlined and if the destructors can be inline, it's safe to
    // inline the constructors.
    return shouldInlineArrayDestruction(ArrSize);
  }

  // Check if we're inside an ArrayInitLoopExpr, and it's sufficiently small.
  if (auto Size = getPendingInitLoop(State, CE, LCtx))
    return shouldInlineArrayDestruction(*Size);

  return false;
}

bool ExprEngine::shouldInlineArrayDestruction(uint64_t Size) {

  uint64_t maxAllowedSize = AMgr.options.maxBlockVisitOnPath;

  // Declaring a 0 element array is also possible.
  return Size <= maxAllowedSize && Size > 0;
}

bool ExprEngine::shouldRepeatCtorCall(ProgramStateRef State,
                                      const CXXConstructExpr *E,
                                      const LocationContext *LCtx) {

  if (!E)
    return false;

  auto Ty = E->getType();

  // FIXME: Handle non constant array types
  if (const auto *CAT = dyn_cast<ConstantArrayType>(Ty)) {
    unsigned Size = getContext().getConstantArrayElementCount(CAT);
    return Size > getIndexOfElementToConstruct(State, E, LCtx);
  }

  if (auto Size = getPendingInitLoop(State, E, LCtx))
    return Size > getIndexOfElementToConstruct(State, E, LCtx);

  return false;
}

static bool isTrivialObjectAssignment(const CallEvent &Call) {
  const CXXInstanceCall *ICall = dyn_cast<CXXInstanceCall>(&Call);
  if (!ICall)
    return false;

  const CXXMethodDecl *MD = dyn_cast_or_null<CXXMethodDecl>(ICall->getDecl());
  if (!MD)
    return false;
  if (!(MD->isCopyAssignmentOperator() || MD->isMoveAssignmentOperator()))
    return false;

  return MD->isTrivial();
}

void ExprEngine::defaultEvalCall(NodeBuilder &Bldr, ExplodedNode *Pred,
                                 const CallEvent &CallTemplate,
                                 const EvalCallOptions &CallOpts) {
  CSA_clearBreakInfo();
  // Make sure we have the most recent state attached to the call.
  ProgramStateRef State = Pred->getState();
  CallEventRef<> Call = CallTemplate.cloneWithState(State);

  // Special-case trivial assignment operators.
  if (isTrivialObjectAssignment(*Call)) {
    performTrivialCopy(Bldr, Pred, *Call);
    return;
  }

  // Try to inline the call.
  // The origin expression here is just used as a kind of checksum;
  // this should still be safe even for CallEvents that don't come from exprs.
  const Expr *E = Call->getOriginExpr();

  ProgramStateRef InlinedFailedState = getInlineFailedState(State, E);
  if (InlinedFailedState) {
    // If we already tried once and failed, make sure we don't retry later.
    State = InlinedFailedState;
    CSA_setBreakInfo(nullptr, CSABreakKind::ReplayWithoutInlining);
  } else {
    RuntimeDefinition RD = Call->getRuntimeDefinition();
    Call->setForeign(RD.isForeign());
    const Decl *D = RD.getDecl();
    if (shouldInlineCall(*Call, D, Pred, CallOpts)) {
      if (RD.mayHaveOtherDefinitions()) {
        AnalyzerOptions &Options = getAnalysisManager().options;

        // Explore with and without inlining the call.
        if (Options.getIPAMode() == IPAK_DynamicDispatchBifurcate) {
          BifurcateCall(RD.getDispatchRegion(), *Call, D, Bldr, Pred);
          return;
        }

        // Don't inline if we're not in any dynamic dispatch mode.
        if (Options.getIPAMode() != IPAK_DynamicDispatch) {
          CSA_setBreakInfo(D, CSABreakKind::DynamicDispatchNotEnabled);
          conservativeEvalCall(*Call, Bldr, Pred, State);
          return;
        }
      }
      ctuBifurcate(*Call, D, Bldr, Pred, State);
      return;
    }
    if (!CSA_hasBreakInfo())
      CSA_setBreakInfo(D, CSABreakKind::ConservativeEvalFallback);
  }

  // If we can't inline it, clean up the state traits used only if the function
  // is inlined.
  State = removeStateTraitsUsedForArrayEvaluation(
      State, dyn_cast_or_null<CXXConstructExpr>(E), Call->getLocationContext());

  // Also handle the return value and invalidate the regions.
  conservativeEvalCall(*Call, Bldr, Pred, State);
}

void ExprEngine::BifurcateCall(const MemRegion *BifurReg,
                               const CallEvent &Call, const Decl *D,
                               NodeBuilder &Bldr, ExplodedNode *Pred) {
  assert(BifurReg);
  BifurReg = BifurReg->StripCasts();

  // Check if we've performed the split already - note, we only want
  // to split the path once per memory region.
  ProgramStateRef State = Pred->getState();
  const unsigned *BState =
                        State->get<DynamicDispatchBifurcationMap>(BifurReg);
  if (BState) {
    // If we are on "inline path", keep inlining if possible.
    if (*BState == DynamicDispatchModeInlined)
      ctuBifurcate(Call, D, Bldr, Pred, State);
    // If inline failed, or we are on the path where we assume we
    // don't have enough info about the receiver to inline, conjure the
    // return value and invalidate the regions.
    if (*BState == DynamicDispatchModeInlined)
      CSA_setBreakInfo(D, CSABreakKind::DynamicDispatchInlineBranchConservative);
    else
      CSA_setBreakInfo(D, CSABreakKind::DynamicDispatchConservativeBranch);

    conservativeEvalCall(Call, Bldr, Pred, State);
    return;
  }

  // If we got here, this is the first time we process a message to this
  // region, so split the path.
  ProgramStateRef IState =
      State->set<DynamicDispatchBifurcationMap>(BifurReg,
                                               DynamicDispatchModeInlined);
  ctuBifurcate(Call, D, Bldr, Pred, IState);

  ProgramStateRef NoIState =
      State->set<DynamicDispatchBifurcationMap>(BifurReg,
                                               DynamicDispatchModeConservative);
  CSA_setBreakInfo(D, CSABreakKind::DynamicDispatchConservativePathCreated);
  conservativeEvalCall(Call, Bldr, Pred, NoIState);

  NumOfDynamicDispatchPathSplits++;
}

void ExprEngine::VisitReturnStmt(const ReturnStmt *RS, ExplodedNode *Pred,
                                 ExplodedNodeSet &Dst) {
  ExplodedNodeSet dstPreVisit;
  getCheckerManager().runCheckersForPreStmt(dstPreVisit, Pred, RS, *this);

  StmtNodeBuilder B(dstPreVisit, Dst, *currBldrCtx);

  if (RS->getRetValue()) {
    for (ExplodedNodeSet::iterator it = dstPreVisit.begin(),
                                  ei = dstPreVisit.end(); it != ei; ++it) {
      B.generateNode(RS, *it, (*it)->getState());
    }
  }
}
