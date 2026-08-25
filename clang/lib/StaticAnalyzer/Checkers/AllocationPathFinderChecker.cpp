#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>

using namespace clang;
using namespace ento;

namespace {

class AllocationPathFinderChecker
    : public Checker<check::PreStmt<CXXNewExpr>, check::PreCall> {
public:
  void checkPreStmt(const CXXNewExpr *NE, CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  enum class SummaryState {
    Computing,
    NoAllocation,
    MayAllocation
  };

  class SummaryScanner;

  mutable std::unique_ptr<BugType> BT;

  // Function-level allocation reachability cache.
  //
  // Key:
  //   canonical FunctionDecl of the function definition.
  //
  // Value:
  //   whether the function body may directly or indirectly reach allocation.
  mutable llvm::DenseMap<const FunctionDecl *, SummaryState> SummaryCache;

  // Human-readable reason for the summary result.
  //
  // Example:
  //   foo -> bar -> C++ new
  mutable llvm::DenseMap<const FunctionDecl *, std::string> SummaryReason;

private:
  static bool isClassicPlacementNew(const CXXNewExpr *NE) {
    if (!NE)
      return false;

    if (NE->getNumPlacementArgs() == 0)
      return false;

    const Expr *Arg = NE->getPlacementArg(0);
    if (!Arg)
      return false;

    QualType T = Arg->IgnoreParenImpCasts()->getType();

    // Classic placement new:
    //
    //   new (buffer) T(...)
    //
    // It constructs an object at an existing address and does not itself
    // allocate heap memory.
    return T->isPointerType();
  }

  static bool isKnownAllocationFunction(llvm::StringRef Name) {
    return Name == "malloc" ||
           Name == "calloc" ||
           Name == "realloc" ||
           Name == "reallocarray" ||
           Name == "aligned_alloc" ||
           Name == "memalign" ||
           Name == "valloc" ||
           Name == "pvalloc" ||
           Name == "posix_memalign" ||
           Name == "strdup" ||
           Name == "strndup";
  }

  static const FunctionDecl *getFunctionDefinition(const FunctionDecl *FD) {
    if (!FD)
      return nullptr;

    const FunctionDecl *Def = nullptr;
    if (FD->hasBody(Def) && Def)
      return Def;

    return nullptr;
  }

  static std::string getFunctionName(const FunctionDecl *FD) {
    if (!FD)
      return "<unknown>";

    std::string Name = FD->getQualifiedNameAsString();
    if (Name.empty())
      return "<anonymous>";

    return Name;
  }

  static std::string getEntryFunctionName(const CheckerContext &C) {
    const LocationContext *LC = C.getLocationContext();
    if (!LC)
      return "<unknown>";

    const StackFrameContext *SF = LC->getStackFrame();
    if (!SF)
      return "<unknown>";

    const StackFrameContext *RootSF = SF;

    for (const LocationContext *Parent = SF->getParent(); Parent;
         Parent = Parent->getParent()) {
      if (const auto *ParentSF = dyn_cast<StackFrameContext>(Parent))
        RootSF = ParentSF;
    }

    const Decl *D = RootSF->getDecl();
    if (!D)
      return "<unknown>";

    if (const auto *FD = dyn_cast<FunctionDecl>(D))
      return getFunctionName(FD);

    return "<unknown>";
  }

  bool mayReachAllocation(const FunctionDecl *FD, unsigned Depth = 0) const ;

  std::string getCachedReason(const FunctionDecl *FD) const {
    const FunctionDecl *Def = getFunctionDefinition(FD);
    if (!Def)
      return "";

    const FunctionDecl *Key = Def->getCanonicalDecl();

    auto It = SummaryReason.find(Key);
    if (It == SummaryReason.end())
      return "";

    return It->second;
  }

  void reportAndStop(CheckerContext &C,
                     const Stmt *S,
                     llvm::StringRef Reason) const {
    // Clang 15.0.4: generateSink requires both state and predecessor.
    //
    // This is important for your mode:
    //   once an allocation-reachable point is found, stop this path.
    ExplodedNode *N = C.generateSink(C.getState(), C.getPredecessor());
    if (!N)
      return;

    if (!BT) {
      BT.reset(new BugType(this,
                           "Reachable dynamic allocation",
                           "Allocation path finder"));
    }

    std::string Entry = getEntryFunctionName(C);

    std::string Msg;
    llvm::raw_string_ostream OS(Msg);
    OS << "Entry function '" << Entry
       << "' reaches dynamic allocation";

    if (!Reason.empty())
      OS << ": " << Reason;

    OS.flush();

    auto R = std::make_unique<PathSensitiveBugReport>(*BT, Msg, N);

    if (S)
      R->addRange(S->getSourceRange());

    C.emitReport(std::move(R));
  }
};



class AllocationPathFinderChecker::SummaryScanner
    : public RecursiveASTVisitor<SummaryScanner> {
  const AllocationPathFinderChecker &Checker;
  bool Found = false;
  std::string Reason;
  unsigned Depth = 0;

public:
  SummaryScanner(const AllocationPathFinderChecker &Checker, unsigned Depth)
      : Checker(Checker), Depth(Depth) {}

  bool TraverseStmt(Stmt *S) {
    if (Found || !S)
      return true;

    return RecursiveASTVisitor<SummaryScanner>::TraverseStmt(S);
  }

  bool VisitCXXNewExpr(CXXNewExpr *NE) {
    if (Checker.isClassicPlacementNew(NE))
      return true;

    Found = true;

    if (NE->isArray())
      Reason = "C++ array new";
    else
      Reason = "C++ new";

    return true;
  }

  bool VisitCallExpr(CallExpr *CE) {
    const FunctionDecl *FD = CE->getDirectCallee();
    if (!FD)
      return true;

    if (FD->getOverloadedOperator() == OO_New) {
      Found = true;
      Reason = "direct operator new";
      return true;
    }

    if (FD->getOverloadedOperator() == OO_Array_New) {
      Found = true;
      Reason = "direct operator new[]";
      return true;
    }

    if (const IdentifierInfo *II = FD->getIdentifier()) {
      llvm::StringRef Name = II->getName();

      if (Checker.isKnownAllocationFunction(Name)) {
        Found = true;
        Reason = Name.str();
        return true;
      }
    }

    if (Checker.mayReachAllocation(FD, Depth + 1)) {
      Found = true;

      std::string CalleeReason = Checker.getCachedReason(FD);

      llvm::raw_string_ostream OS(Reason);
      OS << "call to '" << getFunctionName(FD) << "'";

      if (!CalleeReason.empty())
        OS << " -> " << CalleeReason;

      OS.flush();

      return true;
    }

    return true;
  }

  bool VisitCXXConstructExpr(CXXConstructExpr *CE) {
    const CXXConstructorDecl *Ctor = CE->getConstructor();
    if (!Ctor)
      return true;

    if (Checker.mayReachAllocation(Ctor, Depth + 1)) {
      Found = true;

      std::string CalleeReason = Checker.getCachedReason(Ctor);

      llvm::raw_string_ostream OS(Reason);
      OS << "constructor '" << getFunctionName(Ctor) << "'";

      if (!CalleeReason.empty())
        OS << " -> " << CalleeReason;

      OS.flush();

      return true;
    }

    return true;
  }

  bool VisitCXXTemporaryObjectExpr(CXXTemporaryObjectExpr *TE) {
    const CXXConstructorDecl *Ctor = TE->getConstructor();
    if (!Ctor)
      return true;

    if (Checker.mayReachAllocation(Ctor, Depth + 1)) {
      Found = true;

      std::string CalleeReason = Checker.getCachedReason(Ctor);

      llvm::raw_string_ostream OS(Reason);
      OS << "temporary object constructor '" << getFunctionName(Ctor) << "'";

      if (!CalleeReason.empty())
        OS << " -> " << CalleeReason;

      OS.flush();

      return true;
    }

    return true;
  }

  bool hasFoundAllocation() const {
    return Found;
  }

  llvm::StringRef getReason() const {
    return Reason;
  }
};

void AllocationPathFinderChecker::checkPreStmt(
    const CXXNewExpr *NE,
    CheckerContext &C) const {
  if (isClassicPlacementNew(NE))
    return;

  if (NE->isArray())
    reportAndStop(C, NE, "C++ array new");
  else
    reportAndStop(C, NE, "C++ new");
}

void AllocationPathFinderChecker::checkPreCall(
    const CallEvent &Call,
    CheckerContext &C) const {
  // CXXNewExpr is already handled by checkPreStmt(CXXNewExpr).
  // Avoid reporting the allocator call again for:
  //
  //   new T
  //
  // which may internally appear as operator new.
  if (isa<CXXAllocatorCall>(Call))
    return;

  const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());

  if (FD) {
    if (FD->getOverloadedOperator() == OO_New) {
      reportAndStop(C, Call.getOriginExpr(), "direct operator new");
      return;
    }

    if (FD->getOverloadedOperator() == OO_Array_New) {
      reportAndStop(C, Call.getOriginExpr(), "direct operator new[]");
      return;
    }

    if (const IdentifierInfo *II = FD->getIdentifier()) {
      llvm::StringRef Name = II->getName();

      if (isKnownAllocationFunction(Name)) {
        reportAndStop(C, Call.getOriginExpr(), Name);
        return;
      }
    }

    // Core fast path:
    //
    // If the callee may directly or indirectly reach allocation, report at the
    // current call site and stop the current path. This avoids requiring CSA to
    // actually inline the callee chain:
    //
    //   A -> B -> C -> new
    //
    // When analyzing A, once the path reaches B(), this checker can report A.
    if (mayReachAllocation(FD)) {
      std::string CalleeReason = getCachedReason(FD);

      std::string Reason;
      llvm::raw_string_ostream OS(Reason);
      OS << "call to '" << getFunctionName(FD) << "'";

      if (!CalleeReason.empty())
        OS << " -> " << CalleeReason;

      OS.flush();

      reportAndStop(C, Call.getOriginExpr(), Reason);
      return;
    }
  }

  const IdentifierInfo *II = Call.getCalleeIdentifier();
  if (!II)
    return;

  llvm::StringRef Name = II->getName();

  if (!isKnownAllocationFunction(Name))
    return;

  reportAndStop(C, Call.getOriginExpr(), Name);
}

} // namespace

void ento::registerAllocationPathFinderChecker(CheckerManager &Mgr) {
  Mgr.registerChecker<AllocationPathFinderChecker>();
}

bool ento::shouldRegisterAllocationPathFinderChecker(
    const CheckerManager &Mgr) {
  return true;
}


bool AllocationPathFinderChecker::mayReachAllocation(
    const FunctionDecl *FD,
    unsigned Depth) const {
  if (!FD)
    return false;

  // Prevent pathological recursion through large template/code graphs.
  if (Depth > 64)
    return false;

  const FunctionDecl *Def = getFunctionDefinition(FD);
  if (!Def)
    return false;

  const FunctionDecl *Key = Def->getCanonicalDecl();

  auto It = SummaryCache.find(Key);
  if (It != SummaryCache.end()) {
    switch (It->second) {
    case SummaryState::MayAllocation:
      return true;
    case SummaryState::NoAllocation:
      return false;
    case SummaryState::Computing:
      return false;
    }
  }

  const Stmt *Body = Def->getBody();
  if (!Body) {
    SummaryCache[Key] = SummaryState::NoAllocation;
    return false;
  }

  SummaryCache[Key] = SummaryState::Computing;

  SummaryScanner Scanner(*this, Depth);
  Scanner.TraverseStmt(const_cast<Stmt *>(Body));

  if (Scanner.hasFoundAllocation()) {
    SummaryCache[Key] = SummaryState::MayAllocation;
    SummaryReason[Key] = Scanner.getReason().str();
    return true;
  }

  SummaryCache[Key] = SummaryState::NoAllocation;
  return false;
}
