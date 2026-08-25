//===--- AnalysisConsumer.cpp - ASTConsumer for running Analyses ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// "Meta" ASTConsumer for running different source analyses.
//
//===----------------------------------------------------------------------===//


#include "clang/StaticAnalyzer/Frontend/AnalysisConsumer.h"
#include "ModelInjector.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Analysis/Analyses/LiveVariables.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/CallGraph.h"
#include "clang/Analysis/CodeInjector.h"
#include "clang/Analysis/MacroExpansionContext.h"
#include "clang/Analysis/PathDiagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/CustomAnalyzerFixes.h"
#include "clang/CrossTU/CrossTranslationUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/StaticAnalyzer/Checkers/LocalCheckers.h"
#include "clang/StaticAnalyzer/Core/AnalyzerOptions.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathDiagnosticConsumers.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/AnalysisManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ExprEngine.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>
#include <queue>
#include <utility>

using namespace clang;
using namespace ento;

#define DEBUG_TYPE "AnalysisConsumer"
static bool CSA_CTU_VERBOSE() {
  return std::getenv("CSA_CTU_VERBOSE") != nullptr && 0;
}
STATISTIC(NumFunctionTopLevel, "The # of functions at top level.");
STATISTIC(NumFunctionsAnalyzed,
          "The # of functions and blocks analyzed (as top level "
          "with inlining turned on).");
STATISTIC(NumBlocksInAnalyzedFunctions,
          "The # of basic blocks in the analyzed functions.");
STATISTIC(NumVisitedBlocksInAnalyzedFunctions,
          "The # of visited basic blocks in the analyzed functions.");
STATISTIC(PercentReachableBlocks, "The % of reachable basic blocks.");
STATISTIC(MaxCFGSize, "The maximum number of basic blocks in a function.");

//===----------------------------------------------------------------------===//
// AnalysisConsumer declaration.
//===----------------------------------------------------------------------===//

namespace {

static custom_analyzer_fixes::State
getCustomAnalyzerFixState(const AnalyzerOptionsRef &Opts) {
  const bool All = Opts->AllCustomFixes;
  custom_analyzer_fixes::State State;
  State.CXX14LambdaAssignment = All || Opts->CXX14LambdaAssignmentFix;
  State.CXX17LambdaAssignment = All || Opts->CXX17LambdaAssignmentFix;
  State.ForEachLambdaArgument = All || Opts->ForEachLambdaArgumentFix;
  State.CTUTemplateInstantiation =
      All || Opts->CTUTemplateInstantiationFix;
  State.StructSubobjectInvalidation =
      All || Opts->StructSubobjectInvalidationFix;
  State.ReportPath = All || Opts->ReportPathFix;
  return State;
}

class AnalysisConsumer : public AnalysisASTConsumer,
                         public RecursiveASTVisitor<AnalysisConsumer> {
  enum { AM_None = 0, AM_Syntax = 0x1, AM_Path = 0x2 };
  typedef unsigned AnalysisMode;

  /// Mode of the analyzes while recursively visiting Decls.
  AnalysisMode RecVisitorMode;
  /// Bug Reporter to use while recursively visiting Decls.
  BugReporter *RecVisitorBR;

  std::vector<std::function<void(CheckerRegistry &)>> CheckerRegistrationFns;

public:
  ASTContext *Ctx;
  Preprocessor &PP;
  const std::string OutDir;
  AnalyzerOptionsRef Opts;
  custom_analyzer_fixes::ScopedState CustomFixStateScope;
  ArrayRef<std::string> Plugins;
  CodeInjector *Injector;
  cross_tu::CrossTranslationUnitContext CTU;

  /// Stores the declarations from the local translation unit.
  /// Note, we pre-compute the local declarations at parse time as an
  /// optimization to make sure we do not deserialize everything from disk.
  /// The local declaration to all declarations ratio might be very small when
  /// working with a PCH file.
  SetOfDecls LocalTUDecls;

  MacroExpansionContext MacroExpansions;

  // Set of PathDiagnosticConsumers.  Owned by AnalysisManager.
  PathDiagnosticConsumers PathConsumers;

  StoreManagerCreator CreateStoreMgr;
  ConstraintManagerCreator CreateConstraintMgr;

  std::unique_ptr<CheckerManager> checkerMgr;
  std::unique_ptr<AnalysisManager> Mgr;

  /// Time the analyzes time of each translation unit.
  std::unique_ptr<llvm::TimerGroup> AnalyzerTimers;
  std::unique_ptr<llvm::Timer> SyntaxCheckTimer;
  std::unique_ptr<llvm::Timer> ExprEngineTimer;
  std::unique_ptr<llvm::Timer> BugReporterTimer;

  /// The information about analyzed functions shared throughout the
  /// translation unit.
  FunctionSummariesTy FunctionSummaries;

  AnalysisConsumer(CompilerInstance &CI, const std::string &outdir,
                   AnalyzerOptionsRef opts, ArrayRef<std::string> plugins,
                   CodeInjector *injector)
      : RecVisitorMode(0), RecVisitorBR(nullptr), Ctx(nullptr),
        PP(CI.getPreprocessor()), OutDir(outdir), Opts(std::move(opts)),
        CustomFixStateScope(getCustomAnalyzerFixState(Opts)), Plugins(plugins),
        Injector(injector), CTU(CI),
        MacroExpansions(CI.getLangOpts()) {
    DigestAnalyzerOptions();
    if (Opts->AnalyzerDisplayProgress || Opts->PrintStats ||
        Opts->ShouldSerializeStats) {
      AnalyzerTimers =
          std::make_unique<llvm::TimerGroup>("analyzer", "Analyzer timers");
      SyntaxCheckTimer = std::make_unique<llvm::Timer>(
          "syntaxchecks", "Syntax-based analysis time", *AnalyzerTimers);
      ExprEngineTimer = std::make_unique<llvm::Timer>(
          "exprengine", "Path exploration time", *AnalyzerTimers);
      BugReporterTimer = std::make_unique<llvm::Timer>(
          "bugreporter", "Path-sensitive report post-processing time",
          *AnalyzerTimers);
    }

    if (Opts->PrintStats || Opts->ShouldSerializeStats) {
      llvm::EnableStatistics(/* DoPrintOnExit= */ false);
    }

    if (Opts->ShouldDisplayMacroExpansions)
      MacroExpansions.registerForPreprocessor(PP);
  }

  ~AnalysisConsumer() override {
    if (Opts->PrintStats) {
      llvm::PrintStatistics();
    }
  }

  void DigestAnalyzerOptions() {
    switch (Opts->AnalysisDiagOpt) {
    case PD_NONE:
      break;
#define ANALYSIS_DIAGNOSTICS(NAME, CMDFLAG, DESC, CREATEFN)                    \
  case PD_##NAME:                                                              \
    CREATEFN(Opts->getDiagOpts(), PathConsumers, OutDir, PP, CTU,              \
             MacroExpansions);                                                 \
    break;
#include "clang/StaticAnalyzer/Core/Analyses.def"
    default:
      llvm_unreachable("Unknown analyzer output type!");
    }

    // Create the analyzer component creators.
    CreateStoreMgr = &CreateRegionStoreManager;

    switch (Opts->AnalysisConstraintsOpt) {
    default:
      llvm_unreachable("Unknown constraint manager.");
#define ANALYSIS_CONSTRAINTS(NAME, CMDFLAG, DESC, CREATEFN)                    \
  case NAME##Model:                                                            \
    CreateConstraintMgr = CREATEFN;                                            \
    break;
#include "clang/StaticAnalyzer/Core/Analyses.def"
    }
  }

  void DisplayTime(llvm::TimeRecord &Time) {
    if (!Opts->AnalyzerDisplayProgress) {
      return;
    }
    llvm::errs() << " : " << llvm::format("%1.1f", Time.getWallTime() * 1000)
                 << " ms\n";
  }

  void DisplayFunction(const Decl *D, AnalysisMode Mode,
                       ExprEngine::InliningModes IMode) {
    if (!Opts->AnalyzerDisplayProgress)
      return;

    SourceManager &SM = Mgr->getASTContext().getSourceManager();
    PresumedLoc Loc = SM.getPresumedLoc(D->getLocation());
    if (Loc.isValid()) {
      llvm::errs() << "ANALYZE";

      if (Mode == AM_Syntax)
        llvm::errs() << " (Syntax)";
      else if (Mode == AM_Path) {
        llvm::errs() << " (Path, ";
        switch (IMode) {
        case ExprEngine::Inline_Minimal:
          llvm::errs() << " Inline_Minimal";
          break;
        case ExprEngine::Inline_Regular:
          llvm::errs() << " Inline_Regular";
          break;
        }
        llvm::errs() << ")";
      } else
        assert(Mode == (AM_Syntax | AM_Path) && "Unexpected mode!");

      llvm::errs() << ": " << Loc.getFilename() << ' '
                   << AnalysisDeclContext::getFunctionName(D);
    }
  }

  void Initialize(ASTContext &Context) override {
    Ctx = &Context;
    checkerMgr = std::make_unique<CheckerManager>(*Ctx, *Opts, PP, Plugins,
                                                  CheckerRegistrationFns);

    Mgr = std::make_unique<AnalysisManager>(*Ctx, PP, PathConsumers,
                                            CreateStoreMgr, CreateConstraintMgr,
                                            checkerMgr.get(), *Opts, Injector);
  }

  /// Store the top level decls in the set to be processed later on.
  /// (Doing this pre-processing avoids deserialization of data from PCH.)
  bool HandleTopLevelDecl(DeclGroupRef D) override;
  void HandleTopLevelDeclInObjCContainer(DeclGroupRef D) override;

  void HandleTranslationUnit(ASTContext &C) override;

  /// Determine which inlining mode should be used when this function is
  /// analyzed. This allows to redefine the default inlining policies when
  /// analyzing a given function.
  ExprEngine::InliningModes
  getInliningModeForFunction(const Decl *D, const SetOfConstDecls &Visited);

  /// Build the call graph for all the top level decls of this TU and
  /// use it to define the order in which the functions should be visited.
  void HandleDeclsCallGraph(const unsigned LocalTUDeclsSize);

  /// Run analyzes(syntax or path sensitive) on the given function.
  /// \param Mode - determines if we are requesting syntax only or path
  /// sensitive only analysis.
  /// \param VisitedCallees - The output parameter, which is populated with the
  /// set of functions which should be considered analyzed after analyzing the
  /// given root function.
  void HandleCode(Decl *D, AnalysisMode Mode,
                  ExprEngine::InliningModes IMode = ExprEngine::Inline_Minimal,
                  SetOfConstDecls *VisitedCallees = nullptr);

  void RunPathSensitiveChecks(Decl *D, ExprEngine::InliningModes IMode,
                              SetOfConstDecls *VisitedCallees);

  /// Visitors for the RecursiveASTVisitor.
  bool shouldWalkTypesOfTypeLocs() const { return false; }

  /// Handle callbacks for arbitrary Decls.
  bool VisitDecl(Decl *D) {
    AnalysisMode Mode = getModeForDecl(D, RecVisitorMode);
    if (Mode & AM_Syntax) {
      if (SyntaxCheckTimer)
        SyntaxCheckTimer->startTimer();
      checkerMgr->runCheckersOnASTDecl(D, *Mgr, *RecVisitorBR);
      if (SyntaxCheckTimer)
        SyntaxCheckTimer->stopTimer();
    }
    return true;
  }

  bool VisitVarDecl(VarDecl *VD) {
    if (!Opts->IsNaiveCTUEnabled)
      return true;

    if (VD->hasExternalStorage() || VD->isStaticDataMember()) {
      if (!cross_tu::shouldImport(VD, *Ctx))
        return true;
    } else {
      // Cannot be initialized in another TU.
      return true;
    }

    if (VD->getAnyInitializer())
      return true;

    llvm::Expected<const VarDecl *> CTUDeclOrError = CTU.getCrossTUDefinition(
        VD, Opts->CTUDir, Opts->CTUIndexName, Opts->DisplayCTUProgress);

    if (!CTUDeclOrError) {
      handleAllErrors(CTUDeclOrError.takeError(),
                      [&](const cross_tu::IndexError &IE) {
                        CTU.emitCrossTUDiagnostics(IE);
                      });
    }

    return true;
  }

  bool VisitFunctionDecl(FunctionDecl *FD) {
    IdentifierInfo *II = FD->getIdentifier();
    if (II && II->getName().startswith("__inline"))
      return true;

    // We skip function template definitions, as their semantics is
    // only determined when they are instantiated.
    if (FD->isThisDeclarationADefinition() && !FD->isDependentContext()) {
      assert(RecVisitorMode == AM_Syntax || Mgr->shouldInlineCall() == false);
      HandleCode(FD, RecVisitorMode);
    }
    return true;
  }

  bool VisitObjCMethodDecl(ObjCMethodDecl *MD) {
    if (MD->isThisDeclarationADefinition()) {
      assert(RecVisitorMode == AM_Syntax || Mgr->shouldInlineCall() == false);
      HandleCode(MD, RecVisitorMode);
    }
    return true;
  }

  bool VisitBlockDecl(BlockDecl *BD) {
    if (BD->hasBody()) {
      assert(RecVisitorMode == AM_Syntax || Mgr->shouldInlineCall() == false);
      // Since we skip function template definitions, we should skip blocks
      // declared in those functions as well.
      if (!BD->isDependentContext()) {
        HandleCode(BD, RecVisitorMode);
      }
    }
    return true;
  }

  void AddDiagnosticConsumer(PathDiagnosticConsumer *Consumer) override {
    PathConsumers.push_back(Consumer);
  }

  void
  AddCheckerRegistrationFn(std::function<void(CheckerRegistry &)> Fn) override {
    CheckerRegistrationFns.push_back(std::move(Fn));
  }

private:
  void storeTopLevelDecls(DeclGroupRef DG);

  /// Check if we should skip (not analyze) the given function.
  AnalysisMode getModeForDecl(Decl *D, AnalysisMode Mode);
  void runAnalysisOnTranslationUnit(ASTContext &C);

  /// Print \p S to stderr if \c Opts->AnalyzerDisplayProgress is set.
  void reportAnalyzerProgress(StringRef S);
}; // namespace
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// AnalysisConsumer implementation.
//===----------------------------------------------------------------------===//
bool AnalysisConsumer::HandleTopLevelDecl(DeclGroupRef DG) {
  storeTopLevelDecls(DG);
  return true;
}

void AnalysisConsumer::HandleTopLevelDeclInObjCContainer(DeclGroupRef DG) {
  storeTopLevelDecls(DG);
}

void AnalysisConsumer::storeTopLevelDecls(DeclGroupRef DG) {
  for (auto &I : DG) {

    // Skip ObjCMethodDecl, wait for the objc container to avoid
    // analyzing twice.
    if (isa<ObjCMethodDecl>(I))
      continue;

    LocalTUDecls.push_back(I);
  }
}

//===----------------------------------------------------------------------===//
// NewDelete relevance pruning.
// This is a conservative top-level-entry pruning for cplusplus.NewDelete and
// cplusplus.NewDeleteLeaks. It only decides whether a Decl should be analyzed
// as a top-level path-sensitive entry. It does NOT change inlining, CTU import,
// unknown-call invalidation, or checker semantics.
//===----------------------------------------------------------------------===//

struct NewDeleteRelevanceSummary {
  bool HasNew = false;
  bool HasDelete = false;
  bool HasPointerParam = false;
  bool HasPointerReturn = false;
  bool HasPointerUse = false;
  bool HasUnknownCall = false;
  bool HasPointerReturningCall = false;
  bool HasPointerLikeMemberAccess = false;
  bool HasMethodOnPointerLikeRecord = false;

  bool isRelevant() const {
    return HasNew || HasDelete || HasPointerParam || HasPointerReturn ||
           HasPointerUse || HasUnknownCall || HasPointerReturningCall ||
           HasPointerLikeMemberAccess || HasMethodOnPointerLikeRecord;
  }
};

static bool isNewDeleteRelevancePruningEnabled(const AnalyzerOptionsRef &Opts) {


  //
  //   return Opts->NewDeleteRelevancePruning;
  //

  auto It = Opts->Config.find("newdelete-relevance-pruning");
  if (It == Opts->Config.end())
    return false;

  StringRef V = It->getValue();
  return V == "true" || V == "1" || V == "yes" || V == "on";
}

static bool isPointerLikeType(QualType QT) {
  if (QT.isNull())
    return false;

  QT = QT.getCanonicalType();

  if (QT->isPointerType() || QT->isObjCObjectPointerType() ||
      QT->isBlockPointerType() || QT->isMemberPointerType())
    return true;

  if (QT->isReferenceType()) {
    QualType PointeeTy = QT->getPointeeType().getCanonicalType();
    return PointeeTy->isPointerType() || PointeeTy->isObjCObjectPointerType() ||
           PointeeTy->isBlockPointerType() || PointeeTy->isMemberPointerType();
  }

  return false;
}

static bool recordHasPointerLikeField(const CXXRecordDecl *RD) {
  if (!RD)
    return false;

  const CXXRecordDecl *Def = RD->getDefinition();
  if (!Def)
    return false;

  for (const FieldDecl *F : Def->fields()) {
    if (isPointerLikeType(F->getType()))
      return true;
  }

  return false;
}

class NewDeleteRelevanceVisitor
    : public RecursiveASTVisitor<NewDeleteRelevanceVisitor> {
  NewDeleteRelevanceSummary Summary;

public:
  bool VisitCXXNewExpr(const CXXNewExpr *) {
    Summary.HasNew = true;
    return true;
  }

  bool VisitCXXDeleteExpr(const CXXDeleteExpr *) {
    Summary.HasDelete = true;
    return true;
  }

  bool VisitCallExpr(const CallExpr *CE) {
    if (!CE)
      return true;



    if (isPointerLikeType(CE->getType()))
      Summary.HasPointerReturningCall = true;

    const FunctionDecl *Callee = CE->getDirectCallee();
    if (!Callee) {

      Summary.HasUnknownCall = true;
      return true;
    }

    if (Callee->isOverloadedOperator()) {
      OverloadedOperatorKind OK = Callee->getOverloadedOperator();
      if (OK == OO_New || OK == OO_Array_New)
        Summary.HasNew = true;
      if (OK == OO_Delete || OK == OO_Array_Delete)
        Summary.HasDelete = true;
    }



    if (!Callee->hasBody())
      Summary.HasUnknownCall = true;

    return true;
  }

  bool VisitCXXMemberCallExpr(const CXXMemberCallExpr *CE) {
    if (!CE)
      return true;

    const CXXMethodDecl *MD = CE->getMethodDecl();
    if (!MD)
      return true;

    if (const CXXRecordDecl *RD = MD->getParent()) {
      if (recordHasPointerLikeField(RD))
        Summary.HasMethodOnPointerLikeRecord = true;
    }

    return true;
  }

  bool VisitUnaryOperator(const UnaryOperator *UO) {
    if (!UO)
      return true;

    if (UO->getOpcode() == UO_Deref)
      Summary.HasPointerUse = true;

    return true;
  }

  bool VisitArraySubscriptExpr(const ArraySubscriptExpr *) {
    Summary.HasPointerUse = true;
    return true;
  }

  bool VisitMemberExpr(const MemberExpr *ME) {
    if (!ME)
      return true;

    if (isPointerLikeType(ME->getType()))
      Summary.HasPointerLikeMemberAccess = true;

    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      if (isPointerLikeType(FD->getType()))
        Summary.HasPointerLikeMemberAccess = true;
    }

    return true;
  }

  const NewDeleteRelevanceSummary &getSummary() const { return Summary; }
};

static NewDeleteRelevanceSummary
computeNewDeleteRelevanceSummary(const Decl *D) {
  NewDeleteRelevanceSummary S;

  if (!D)
    return S;

  if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
    for (const ParmVarDecl *P : FD->parameters()) {
      if (isPointerLikeType(P->getType()))
        S.HasPointerParam = true;
    }

    if (isPointerLikeType(FD->getReturnType()))
      S.HasPointerReturn = true;

    if (const auto *MD = dyn_cast<CXXMethodDecl>(FD)) {
      if (!MD->isStatic()) {
        if (recordHasPointerLikeField(MD->getParent()))
          S.HasMethodOnPointerLikeRecord = true;
      }
    }
  } else if (const auto *MD = dyn_cast<ObjCMethodDecl>(D)) {
    for (const ParmVarDecl *P : MD->parameters()) {
      if (isPointerLikeType(P->getType()))
        S.HasPointerParam = true;
    }

    if (isPointerLikeType(MD->getReturnType()))
      S.HasPointerReturn = true;
  } else if (const auto *BD = dyn_cast<BlockDecl>(D)) {
    for (const ParmVarDecl *P : BD->parameters()) {
      if (isPointerLikeType(P->getType()))
        S.HasPointerParam = true;
    }
  }

  const Stmt *Body = D->getBody();
  if (!Body)
    return S;

  NewDeleteRelevanceVisitor V;
  V.TraverseStmt(const_cast<Stmt *>(Body));

  const NewDeleteRelevanceSummary &BodyS = V.getSummary();
  S.HasNew |= BodyS.HasNew;
  S.HasDelete |= BodyS.HasDelete;
  S.HasPointerUse |= BodyS.HasPointerUse;
  S.HasUnknownCall |= BodyS.HasUnknownCall;
  S.HasPointerReturningCall |= BodyS.HasPointerReturningCall;
  S.HasPointerLikeMemberAccess |= BodyS.HasPointerLikeMemberAccess;
  S.HasMethodOnPointerLikeRecord |= BodyS.HasMethodOnPointerLikeRecord;

  return S;
}

static void dumpNewDeleteRelevanceSummary(StringRef Tag, const Decl *D,
                                          const NewDeleteRelevanceSummary &S,
                                          bool Pruned) {
  if (CSA_CTU_VERBOSE()) {
    llvm::errs() << "[ND-PRUNE] " << Tag << " ";
    if (Pruned)
      llvm::errs() << "Skip";
    else
      llvm::errs() << "Keep";

    llvm::errs() << " Decl=";

    if (const auto *ND = dyn_cast_or_null<NamedDecl>(D))
      llvm::errs() << ND->getQualifiedNameAsString();
    else
      llvm::errs() << "<anonymous>";

    llvm::errs() << " HasNew=" << S.HasNew << " HasDelete=" << S.HasDelete
                 << " HasPointerParam=" << S.HasPointerParam
                 << " HasPointerReturn=" << S.HasPointerReturn
                 << " HasPointerUse=" << S.HasPointerUse
                 << " HasUnknownCall=" << S.HasUnknownCall
                 << " HasPointerReturningCall=" << S.HasPointerReturningCall
                 << " HasPointerLikeMemberAccess="
                 << S.HasPointerLikeMemberAccess
                 << " HasMethodOnPointerLikeRecord="
                 << S.HasMethodOnPointerLikeRecord << "\n";
  }
}


//===----------------------------------------------------------------------===//
// Explicit NewDelete entry filter.
//
// This optimization restricts top-level path-sensitive analysis to functions
// listed in a user-provided config file. It does NOT change inlining,
// CTU import, opaque-call invalidation, RegionStore, or checker semantics.
//===----------------------------------------------------------------------===//

struct ExplicitNewDeleteEntryConfig {
  bool Loaded = false;
  bool LoadFailed = false;
  std::string LoadedPath;

  // qname:OHOS::AAFwk::MissionListManager::OnRemoteRequest
  llvm::StringSet<> QNames;

  // OnRemoteRequest
  llvm::StringSet<> SimpleNames;

  // lookup:c:@N@OHOS@...
  llvm::StringSet<> LookupNames;
};

static std::string getExplicitNewDeleteEntryConfigPath(
    const AnalyzerOptionsRef &Opts) {
  if (!Opts->NewDeleteEntryConfig.empty())
    return std::string(Opts->NewDeleteEntryConfig);

  return "";
}

static bool isExplicitEntryLookupNameLine(StringRef Line) {
  if (Line.startswith("c:"))
    return true;

  // LLVM newer externalDefMap format may use:
  //   <length>:<lookup-name>
  // We support this form as a lookup name too.
  size_t Colon = Line.find(':');
  if (Colon == StringRef::npos || Colon == 0)
    return false;

  StringRef Prefix = Line.substr(0, Colon);
  for (char C : Prefix) {
    if (!llvm::isDigit(C))
      return false;
  }

  return true;
}

static bool isExplicitNewDeleteEntryFilterEnabled(
    const AnalyzerOptionsRef &Opts) {
  return !getExplicitNewDeleteEntryConfigPath(Opts).empty();
}


static void loadExplicitNewDeleteEntryConfig(
    const AnalyzerOptionsRef &Opts,
    ExplicitNewDeleteEntryConfig &Config) {
  std::string ConfigPath = getExplicitNewDeleteEntryConfigPath(Opts);

  if (Config.Loaded && Config.LoadedPath == ConfigPath)
    return;

  Config.Loaded = true;
  Config.LoadFailed = false;
  Config.LoadedPath = ConfigPath;
  Config.QNames.clear();
  Config.SimpleNames.clear();
  Config.LookupNames.clear();

  if (ConfigPath.empty())
    return;

  auto BufferOrErr = llvm::MemoryBuffer::getFile(ConfigPath);
  if (!BufferOrErr) {
    Config.LoadFailed = true;

    if (CSA_CTU_VERBOSE()) {
      llvm::errs() << "[ND-ENTRY] failed to load config path="
                   << ConfigPath << "\n";
    }

    return;
  }

  StringRef Content = BufferOrErr.get()->getBuffer();

  SmallVector<StringRef, 0> Lines;
  Content.split(Lines, '\n');

  unsigned QNames = 0;
  unsigned SimpleNames = 0;
  unsigned LookupNames = 0;

  for (StringRef Line : Lines) {
    Line = Line.trim();

    if (Line.empty())
      continue;

    // Only full-line comments are supported.
    // Do not strip trailing '#', because CTU lookup names may contain '#'.
    if (Line.startswith("#"))
      continue;

    if (Line.startswith("qname:")) {
      StringRef Name = Line.drop_front(StringRef("qname:").size()).trim();
      if (!Name.empty()) {
        Config.QNames.insert(Name);
        ++QNames;
      }
      continue;
    }

    if (Line.startswith("lookup:")) {
      StringRef Name = Line.drop_front(StringRef("lookup:").size()).trim();
      if (!Name.empty()) {
        Config.LookupNames.insert(Name);
        ++LookupNames;
      }
      continue;
    }

    if (Line.startswith("name:")) {
      StringRef Name = Line.drop_front(StringRef("name:").size()).trim();
      if (!Name.empty()) {
        Config.SimpleNames.insert(Name);
        ++SimpleNames;
      }
      continue;
    }

    // No prefix:
    //   OHOS::AAFwk::X::foo  -> qname
    //   OnRemoteRequest      -> simple name
    //   c:@N@OHOS@...        -> CTU lookup name
    if (isExplicitEntryLookupNameLine(Line)) {
      Config.LookupNames.insert(Line);
      ++LookupNames;
      continue;
    }

    if (Line.contains("::")) {
      Config.QNames.insert(Line);
      ++QNames;
      continue;
    }

    Config.SimpleNames.insert(Line);
    ++SimpleNames;
  }

  if (CSA_CTU_VERBOSE()) {
    llvm::errs() << "[ND-ENTRY] loaded config path=" << ConfigPath
                 << " qnames=" << QNames
                 << " simple-names=" << SimpleNames
                 << " lookup-names=" << LookupNames << "\n";
  }
}

static ExplicitNewDeleteEntryConfig &
getExplicitNewDeleteEntryConfig(const AnalyzerOptionsRef &Opts) {
  // One CSA process normally analyzes one action with one analyzer config.
  // CodeChecker parallelism uses multiple processes, so process-local cache is
  // enough here.
  static ExplicitNewDeleteEntryConfig Config;

  loadExplicitNewDeleteEntryConfig(Opts, Config);
  return Config;
}

static std::string getDeclQualifiedNameForEntryFilter(const Decl *D) {
  if (!D)
    return "";

  if (const auto *ND = dyn_cast<NamedDecl>(D))
    return ND->getQualifiedNameAsString();

  return "";
}


static std::string getDeclSimpleNameForEntryFilter(const Decl *D) {
  if (!D)
    return "";

  if (const auto *ND = dyn_cast<NamedDecl>(D))
    return ND->getNameAsString();

  return "";
}

static llvm::Optional<std::string>
getDeclLookupNameForEntryFilter(const Decl *D) {
  if (!D)
    return llvm::None;

  const auto *DD = dyn_cast<DeclaratorDecl>(D);
  if (!DD)
    return llvm::None;

  return cross_tu::CrossTranslationUnitContext::getLookupName(DD);
}


static bool isExplicitNewDeleteEntryAllowed(const Decl *D,
                                            const AnalyzerOptionsRef &Opts) {
  if (!isExplicitNewDeleteEntryFilterEnabled(Opts))
    return true;

  ExplicitNewDeleteEntryConfig &Config =
      getExplicitNewDeleteEntryConfig(Opts);

  // Fail-open: if config file cannot be loaded, keep original behavior.
  // This avoids accidentally suppressing all reports due to a wrong path.
  if (Config.LoadFailed)
    return true;

  std::string QName = getDeclQualifiedNameForEntryFilter(D);
  if (!QName.empty() && Config.QNames.count(QName))
    return true;

  std::string SimpleName = getDeclSimpleNameForEntryFilter(D);
  if (!SimpleName.empty() && Config.SimpleNames.count(SimpleName))
    return true;

  llvm::Optional<std::string> LookupName =
      getDeclLookupNameForEntryFilter(D);
  if (LookupName && Config.LookupNames.count(*LookupName))
    return true;

  return false;
}


static void dumpExplicitNewDeleteEntryDecision(StringRef Tag,
                                               const Decl *D,
                                               bool Allowed,
                                               StringRef Reason) {
  if (CSA_CTU_VERBOSE()) {
    llvm::errs() << "[ND-ENTRY] " << Tag
                 << " action=" << (Allowed ? "KeepEntry" : "SkipEntry")
                 << " reason=" << Reason;

    std::string QName = getDeclQualifiedNameForEntryFilter(D);
    if (!QName.empty())
      llvm::errs() << " qname=" << QName;
    else
      llvm::errs() << " qname=<anonymous>";

    std::string SimpleName = getDeclSimpleNameForEntryFilter(D);
    if (!SimpleName.empty())
      llvm::errs() << " simple=" << SimpleName;

    if (llvm::Optional<std::string> LookupName =
            getDeclLookupNameForEntryFilter(D)) {
      llvm::errs() << " lookup=" << *LookupName;
    }

    llvm::errs() << "\n";
  }
}



static bool
shouldPruneNewDeletePathEntry(const Decl *D,
                              NewDeleteRelevanceSummary *Out = nullptr) {
  NewDeleteRelevanceSummary S = computeNewDeleteRelevanceSummary(D);

  if (Out)
    *Out = S;

  return !S.isRelevant();
}

static bool shouldSkipFunction(const Decl *D, const SetOfConstDecls &Visited,
                               const SetOfConstDecls &VisitedAsTopLevel) {
  if (VisitedAsTopLevel.count(D))
    return true;

  // Skip analysis of inheriting constructors as top-level functions. These
  // constructors don't even have a body written down in the code, so even if
  // we find a bug, we won't be able to display it.
  if (const auto *CD = dyn_cast<CXXConstructorDecl>(D))
    if (CD->isInheritingConstructor())
      return true;

  // We want to re-analyse the functions as top level in the following cases:
  // - The 'init' methods should be reanalyzed because
  //   ObjCNonNilReturnValueChecker assumes that '[super init]' never returns
  //   'nil' and unless we analyze the 'init' functions as top level, we will
  //   not catch errors within defensive code.
  // - We want to reanalyze all ObjC methods as top level to report Retain
  //   Count naming convention errors more aggressively.
  if (isa<ObjCMethodDecl>(D))
    return false;
  // We also want to reanalyze all C++ copy and move assignment operators to
  // separately check the two cases where 'this' aliases with the parameter and
  // where it may not. (cplusplus.SelfAssignmentChecker)
  if (const auto *MD = dyn_cast<CXXMethodDecl>(D)) {
    if (MD->isCopyAssignmentOperator() || MD->isMoveAssignmentOperator())
      return false;
  }

  // Otherwise, if we visited the function before, do not reanalyze it.
  return Visited.count(D);
}

ExprEngine::InliningModes
AnalysisConsumer::getInliningModeForFunction(const Decl *D,
                                             const SetOfConstDecls &Visited) {
  // We want to reanalyze all ObjC methods as top level to report Retain
  // Count naming convention errors more aggressively. But we should tune down
  // inlining when reanalyzing an already inlined function.
  if (Visited.count(D) && isa<ObjCMethodDecl>(D)) {
    const ObjCMethodDecl *ObjCM = cast<ObjCMethodDecl>(D);
    if (ObjCM->getMethodFamily() != OMF_init)
      return ExprEngine::Inline_Minimal;
  }

  return ExprEngine::Inline_Regular;
}

void AnalysisConsumer::HandleDeclsCallGraph(const unsigned LocalTUDeclsSize) {
  // Build the Call Graph by adding all the top level declarations to the graph.
  // Note: CallGraph can trigger deserialization of more items from a pch
  // (though HandleInterestingDecl); triggering additions to LocalTUDecls.
  // We rely on random access to add the initially processed Decls to CG.
  CallGraph CG;
  for (unsigned i = 0; i < LocalTUDeclsSize; ++i) {
    CG.addToCallGraph(LocalTUDecls[i]);
  }

  // Walk over all of the call graph nodes in topological order, so that we
  // analyze parents before the children. Skip the functions inlined into
  // the previously processed functions. Use external Visited set to identify
  // inlined functions. The topological order allows the "do not reanalyze
  // previously inlined function" performance heuristic to be triggered more
  // often.
  SetOfConstDecls Visited;
  SetOfConstDecls VisitedAsTopLevel;
  llvm::ReversePostOrderTraversal<clang::CallGraph *> RPOT(&CG);
  for (auto &N : RPOT) {
    NumFunctionTopLevel++;

    Decl *D = N->getDecl();

    // Skip the abstract root node.
    if (!D)
      continue;

    // Skip the functions which have been processed already or previously
    // inlined.
  const bool ExplicitEntryMode = isExplicitNewDeleteEntryFilterEnabled(Opts);

  // In explicit-entry mode, the allowlist itself controls which functions are
  // analyzed as top-level roots. Do not use the global Visited heuristic here,
  // because Visited contains callees inlined from previous roots. Using it may
  // accidentally suppress configured entries or affect later inlining decisions.
  if (ExplicitEntryMode) {
    if (VisitedAsTopLevel.count(D))
      continue;

    if (const auto *CD = dyn_cast<CXXConstructorDecl>(D)) {
      if (CD->isInheritingConstructor())
        continue;
    }
  } else {
    // Skip the functions which have been processed already or previously
    // inlined.
    if (shouldSkipFunction(D, Visited, VisitedAsTopLevel))
      continue;
  }
    // The CallGraph might have declarations as callees. However, during CTU
    // the declaration might form a declaration chain with the newly imported
    // definition from another TU. In this case we don't want to analyze the
    // function definition as toplevel.
    if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
      // Calling 'hasBody' replaces 'FD' in place with the FunctionDecl
      // that has the body.
      FD->hasBody(FD);
      if (CTU.isImportedAsNew(FD))
        continue;
    }

    // Explicit entry filter has higher priority than memory-relevance pruning.
    // If a config file is provided, only listed functions are analyzed as
    // top-level entries. Listed functions must not be pruned again by the
    // heuristic relevance filter.
  if (ExplicitEntryMode) {
    if (!isExplicitNewDeleteEntryAllowed(D, Opts)) {
      dumpExplicitNewDeleteEntryDecision("CallGraphEntry", D,
                                        /*Allowed=*/false,
                                        "not-in-config-root-only");
      continue;
    }

    dumpExplicitNewDeleteEntryDecision("CallGraphEntry", D,
                                      /*Allowed=*/true,
                                      "in-config-root");
  } else if (isNewDeleteRelevancePruningEnabled(Opts)) {
    NewDeleteRelevanceSummary S;
    if (shouldPruneNewDeletePathEntry(D, &S)) {
      dumpNewDeleteRelevanceSummary("CallGraphEntry", D, S,
                                    /*Pruned=*/true);
      continue;
    }

    dumpNewDeleteRelevanceSummary("CallGraphEntry", D, S,
                                  /*Pruned=*/false);
  }

    // Analyze the function.
    SetOfConstDecls VisitedCallees;

    // In explicit-entry mode, do not collect inlined callees into the global
    // Visited set. The allowlist already decides top-level roots, and callees
    // must remain available for normal inline/CTU analysis when reached from an
    // allowed entry.
    SetOfConstDecls *VisitedCalleesPtr = nullptr;
    if (!ExplicitEntryMode && Mgr->options.InliningMode != All)
      VisitedCalleesPtr = &VisitedCallees;

    HandleCode(D, AM_Path, getInliningModeForFunction(D, Visited),
              VisitedCalleesPtr);

    // Add the visited callees to the global visited set only in the original
    // heuristic mode. Do not do this in explicit-entry mode.
    if (!ExplicitEntryMode) {
      for (const Decl *Callee : VisitedCallees)
        // Decls from CallGraph are already canonical. But Decls coming from
        // CallExprs may be not. We should canonicalize them manually.
        Visited.insert(isa<ObjCMethodDecl>(Callee) ? Callee
                                                  : Callee->getCanonicalDecl());
    }

    VisitedAsTopLevel.insert(D);
  }
}

static bool fileContainsString(StringRef Substring, ASTContext &C) {
  const SourceManager &SM = C.getSourceManager();
  FileID FID = SM.getMainFileID();
  StringRef Buffer = SM.getBufferOrFake(FID).getBuffer();
  return Buffer.contains(Substring);
}

static void reportAnalyzerFunctionMisuse(const AnalyzerOptions &Opts,
                                         const ASTContext &Ctx) {
  llvm::errs() << "Every top-level function was skipped.\n";

  if (!Opts.AnalyzerDisplayProgress)
    llvm::errs() << "Pass the -analyzer-display-progress for tracking which "
                    "functions are analyzed.\n";

  bool HasBrackets =
      Opts.AnalyzeSpecificFunction.find("(") != std::string::npos;

  if (Ctx.getLangOpts().CPlusPlus && !HasBrackets) {
    llvm::errs()
        << "For analyzing C++ code you need to pass the function parameter "
           "list: -analyze-function=\"foobar(int, _Bool)\"\n";
  } else if (!Ctx.getLangOpts().CPlusPlus && HasBrackets) {
    llvm::errs() << "For analyzing C code you shouldn't pass the function "
                    "parameter list, only the name of the function: "
                    "-analyze-function=foobar\n";
  }
}

void AnalysisConsumer::runAnalysisOnTranslationUnit(ASTContext &C) {
  BugReporter BR(*Mgr);
  TranslationUnitDecl *TU = C.getTranslationUnitDecl();
  if (SyntaxCheckTimer)
    SyntaxCheckTimer->startTimer();
  checkerMgr->runCheckersOnASTDecl(TU, *Mgr, BR);
  if (SyntaxCheckTimer)
    SyntaxCheckTimer->stopTimer();

  // Run the AST-only checks using the order in which functions are defined.
  // If inlining is not turned on, use the simplest function order for path
  // sensitive analyzes as well.
  RecVisitorMode = AM_Syntax;
  if (!Mgr->shouldInlineCall())
    RecVisitorMode |= AM_Path;
  RecVisitorBR = &BR;

  // Process all the top level declarations.
  //
  // Note: TraverseDecl may modify LocalTUDecls, but only by appending more
  // entries.  Thus we don't use an iterator, but rely on LocalTUDecls
  // random access.  By doing so, we automatically compensate for iterators
  // possibly being invalidated, although this is a bit slower.
  const unsigned LocalTUDeclsSize = LocalTUDecls.size();
  for (unsigned i = 0; i < LocalTUDeclsSize; ++i) {
    TraverseDecl(LocalTUDecls[i]);
  }

  if (Mgr->shouldInlineCall())
    HandleDeclsCallGraph(LocalTUDeclsSize);

  // After all decls handled, run checkers on the entire TranslationUnit.
  checkerMgr->runCheckersOnEndOfTranslationUnit(TU, *Mgr, BR);

  BR.FlushReports();
  RecVisitorBR = nullptr;

  // If the user wanted to analyze a specific function and the number of basic
  // blocks analyzed is zero, than the user might not specified the function
  // name correctly.
  // FIXME: The user might have analyzed the requested function in Syntax mode,
  // but we are unaware of that.
  if (!Opts->AnalyzeSpecificFunction.empty() && NumFunctionsAnalyzed == 0)
    reportAnalyzerFunctionMisuse(*Opts, *Ctx);
}

void AnalysisConsumer::reportAnalyzerProgress(StringRef S) {
  if (Opts->AnalyzerDisplayProgress)
    llvm::errs() << S;
}

void AnalysisConsumer::HandleTranslationUnit(ASTContext &C) {
  // Don't run the actions if an error has occurred with parsing the file.
  DiagnosticsEngine &Diags = PP.getDiagnostics();
  if (Diags.hasErrorOccurred() || Diags.hasFatalErrorOccurred())
    return;

  // Explicitly destroy the PathDiagnosticConsumer.  This will flush its output.
  // FIXME: This should be replaced with something that doesn't rely on
  // side-effects in PathDiagnosticConsumer's destructor. This is required when
  // used with option -disable-free.
  const auto DiagFlusherScopeExit =
      llvm::make_scope_exit([this] { Mgr.reset(); });

  if (Opts->ShouldIgnoreBisonGeneratedFiles &&
      fileContainsString("/* A Bison parser, made by", C)) {
    reportAnalyzerProgress("Skipping bison-generated file\n");
    return;
  }

  if (Opts->ShouldIgnoreFlexGeneratedFiles &&
      fileContainsString("/* A lexical scanner generated by flex", C)) {
    reportAnalyzerProgress("Skipping flex-generated file\n");
    return;
  }

  // Don't analyze if the user explicitly asked for no checks to be performed
  // on this file.
  if (Opts->DisableAllCheckers) {
    reportAnalyzerProgress("All checks are disabled using a supplied option\n");
    return;
  }

  // Otherwise, just run the analysis.
  runAnalysisOnTranslationUnit(C);

  // Count how many basic blocks we have not covered.
  NumBlocksInAnalyzedFunctions = FunctionSummaries.getTotalNumBasicBlocks();
  NumVisitedBlocksInAnalyzedFunctions =
      FunctionSummaries.getTotalNumVisitedBasicBlocks();
  if (NumBlocksInAnalyzedFunctions > 0)
    PercentReachableBlocks =
        (FunctionSummaries.getTotalNumVisitedBasicBlocks() * 100) /
        NumBlocksInAnalyzedFunctions;
}

AnalysisConsumer::AnalysisMode
AnalysisConsumer::getModeForDecl(Decl *D, AnalysisMode Mode) {
  if (!Opts->AnalyzeSpecificFunction.empty() &&
      AnalysisDeclContext::getFunctionName(D) != Opts->AnalyzeSpecificFunction)
    return AM_None;

  // Unless -analyze-all is specified, treat decls differently depending on
  // where they came from:
  // - Main source file: run both path-sensitive and non-path-sensitive checks.
  // - Header files: run non-path-sensitive checks only.
  // - System headers: don't run any checks.
  if (Opts->AnalyzeAll)
    return Mode;

  const SourceManager &SM = Ctx->getSourceManager();

  const SourceLocation Loc = [&SM](Decl *D) -> SourceLocation {
    const Stmt *Body = D->getBody();
    SourceLocation SL = Body ? Body->getBeginLoc() : D->getLocation();
    return SM.getExpansionLoc(SL);
  }(D);

  // Ignore system headers.
  if (Loc.isInvalid() || SM.isInSystemHeader(Loc))
    return AM_None;

  // Disable path sensitive analysis in user-headers.
  if (!Mgr->isInCodeFile(Loc))
    return Mode & ~AM_Path;

  return Mode;
}

void AnalysisConsumer::HandleCode(Decl *D, AnalysisMode Mode,
                                  ExprEngine::InliningModes IMode,
                                  SetOfConstDecls *VisitedCallees) {
  if (!D->hasBody())
    return;
  Mode = getModeForDecl(D, Mode);
  if (Mode == AM_None)
    return;

  // Clear the AnalysisManager of old AnalysisDeclContexts.
  Mgr->ClearContexts();
  // Ignore autosynthesized code.
  if (Mgr->getAnalysisDeclContext(D)->isBodyAutosynthesized())
    return;

  CFG *DeclCFG = Mgr->getCFG(D);
  if (DeclCFG)
    MaxCFGSize.updateMax(DeclCFG->size());

  DisplayFunction(D, Mode, IMode);
  BugReporter BR(*Mgr);

  if (Mode & AM_Syntax) {
    llvm::TimeRecord CheckerStartTime;
    if (SyntaxCheckTimer) {
      CheckerStartTime = SyntaxCheckTimer->getTotalTime();
      SyntaxCheckTimer->startTimer();
    }
    checkerMgr->runCheckersOnASTBody(D, *Mgr, BR);
    if (SyntaxCheckTimer) {
      SyntaxCheckTimer->stopTimer();
      llvm::TimeRecord CheckerEndTime = SyntaxCheckTimer->getTotalTime();
      CheckerEndTime -= CheckerStartTime;
      DisplayTime(CheckerEndTime);
    }
  }

  BR.FlushReports();

  if ((Mode & AM_Path) && checkerMgr->hasPathSensitiveCheckers()) {
    RunPathSensitiveChecks(D, IMode, VisitedCallees);
    if (IMode != ExprEngine::Inline_Minimal)
      NumFunctionsAnalyzed++;
  }
}

// void AnalysisConsumer::HandleCode(Decl *D, AnalysisMode Mode,
//                                   ExprEngine::InliningModes IMode,
//                                   SetOfConstDecls *VisitedCallees) {
//   if (!D->hasBody())
//     return;
//   Mode = getModeForDecl(D, Mode);
//   if (Mode == AM_None)
//     return;

//     // Explicit NewDelete entry filter.
//   // This is the final safety net for entries coming from VisitFunctionDecl(),
//   // VisitObjCMethodDecl(), VisitBlockDecl(), HandleDeclsCallGraph(), or any
//   // future caller of HandleCode().
//   if (isExplicitNewDeleteEntryFilterEnabled(Opts) && (Mode & AM_Path)) {
//     if (!isExplicitNewDeleteEntryAllowed(D, Opts)) {
//       dumpExplicitNewDeleteEntryDecision("HandleCode", D,
//                                          /*Allowed=*/false,
//                                          "not-in-config");

//       Mode &= ~AM_Path;

//       if (Mode == AM_None)
//         return;
//     } else {
//       dumpExplicitNewDeleteEntryDecision("HandleCode", D,
//                                          /*Allowed=*/true,
//                                          "in-config");
//     }
//   } else if (isNewDeleteRelevancePruningEnabled(Opts) && (Mode & AM_Path)) {
//     NewDeleteRelevanceSummary S;
//     if (shouldPruneNewDeletePathEntry(D, &S)) {
//       dumpNewDeleteRelevanceSummary("HandleCode", D, S, /*Pruned=*/true);

//       Mode &= ~AM_Path;

//       if (Mode == AM_None)
//         return;
//     } else {
//       dumpNewDeleteRelevanceSummary("HandleCode", D, S, /*Pruned=*/false);
//     }
//   }

//   // Final NewDelete relevance pruning gate.
//   // This is the safety net for entries coming from VisitFunctionDecl(),
//   // VisitObjCMethodDecl(), VisitBlockDecl(), or any future caller of
//   // HandleCode().
//   if (isNewDeleteRelevancePruningEnabled(Opts) && (Mode & AM_Path)) {
//     NewDeleteRelevanceSummary S;
//     if (shouldPruneNewDeletePathEntry(D, &S)) {
//       dumpNewDeleteRelevanceSummary("HandleCode", D, S, /*Pruned=*/true);

//       Mode &= ~AM_Path;

//       if (Mode == AM_None)
//         return;
//     } else {
//       dumpNewDeleteRelevanceSummary("HandleCode", D, S, /*Pruned=*/false);
//     }
//   }
//   // Clear the AnalysisManager of old AnalysisDeclContexts.
//   Mgr->ClearContexts();
//   // Ignore autosynthesized code.
//   if (Mgr->getAnalysisDeclContext(D)->isBodyAutosynthesized())
//     return;

//   CFG *DeclCFG = Mgr->getCFG(D);
//   if (DeclCFG)
//     MaxCFGSize.updateMax(DeclCFG->size());

//   DisplayFunction(D, Mode, IMode);
//   BugReporter BR(*Mgr);

//   if (Mode & AM_Syntax) {
//     llvm::TimeRecord CheckerStartTime;
//     if (SyntaxCheckTimer) {
//       CheckerStartTime = SyntaxCheckTimer->getTotalTime();
//       SyntaxCheckTimer->startTimer();
//     }
//     checkerMgr->runCheckersOnASTBody(D, *Mgr, BR);
//     if (SyntaxCheckTimer) {
//       SyntaxCheckTimer->stopTimer();
//       llvm::TimeRecord CheckerEndTime = SyntaxCheckTimer->getTotalTime();
//       CheckerEndTime -= CheckerStartTime;
//       DisplayTime(CheckerEndTime);
//     }
//   }

//   BR.FlushReports();

//   if ((Mode & AM_Path) && checkerMgr->hasPathSensitiveCheckers()) {
//     RunPathSensitiveChecks(D, IMode, VisitedCallees);
//     if (IMode != ExprEngine::Inline_Minimal)
//       NumFunctionsAnalyzed++;
//   }
// }

//===----------------------------------------------------------------------===//
// Path-sensitive checking.
//===----------------------------------------------------------------------===//

void AnalysisConsumer::RunPathSensitiveChecks(Decl *D,
                                              ExprEngine::InliningModes IMode,
                                              SetOfConstDecls *VisitedCallees) {
  // Construct the analysis engine.  First check if the CFG is valid.
  // FIXME: Inter-procedural analysis will need to handle invalid CFGs.
  if (!Mgr->getCFG(D))
    return;

  // See if the LiveVariables analysis scales.
  if (!Mgr->getAnalysisDeclContext(D)->getAnalysis<RelaxedLiveVariables>())
    return;

  ExprEngine Eng(CTU, *Mgr, VisitedCallees, &FunctionSummaries, IMode);

  // Execute the worklist algorithm.
  llvm::TimeRecord ExprEngineStartTime;
  if (ExprEngineTimer) {
    ExprEngineStartTime = ExprEngineTimer->getTotalTime();
    ExprEngineTimer->startTimer();
  }
  Eng.ExecuteWorkList(Mgr->getAnalysisDeclContextManager().getStackFrame(D),
                      Mgr->options.MaxNodesPerTopLevelFunction);
  if (ExprEngineTimer) {
    ExprEngineTimer->stopTimer();
    llvm::TimeRecord ExprEngineEndTime = ExprEngineTimer->getTotalTime();
    ExprEngineEndTime -= ExprEngineStartTime;
    DisplayTime(ExprEngineEndTime);
  }

  if (!Mgr->options.DumpExplodedGraphTo.empty())
    Eng.DumpGraph(Mgr->options.TrimGraph, Mgr->options.DumpExplodedGraphTo);

  // Visualize the exploded graph.
  if (Mgr->options.visualizeExplodedGraphWithGraphViz)
    Eng.ViewGraph(Mgr->options.TrimGraph);

  // Display warnings.
  if (BugReporterTimer)
    BugReporterTimer->startTimer();
  Eng.getBugReporter().FlushReports();
  if (BugReporterTimer)
    BugReporterTimer->stopTimer();
}

//===----------------------------------------------------------------------===//
// AnalysisConsumer creation.
//===----------------------------------------------------------------------===//

std::unique_ptr<AnalysisASTConsumer>
ento::CreateAnalysisConsumer(CompilerInstance &CI) {
  // Disable the effects of '-Werror' when using the AnalysisConsumer.
  CI.getPreprocessor().getDiagnostics().setWarningsAsErrors(false);

  AnalyzerOptionsRef analyzerOpts = CI.getAnalyzerOpts();
  bool hasModelPath = analyzerOpts->Config.count("model-path") > 0;

  return std::make_unique<AnalysisConsumer>(
      CI, CI.getFrontendOpts().OutputFile, analyzerOpts,
      CI.getFrontendOpts().Plugins,
      hasModelPath ? new ModelInjector(CI) : nullptr);
}
