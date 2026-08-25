//== SelfDeleteFieldUAFChecker.cpp ------------------------------*- C++ -*-==//
//
// Clang 15.0.4-compatible implementation.
//
// Enhancements vs MVP:
//  1) MayFreeReceiver inferred (direct delete-this + transitive this-call).
//  2) Better notes (callsite + destructor location) using
//  PathDiagnosticLocation. 3) Kill set on `this->field = nullptr/0`. 4) Broader
//  destructor field-use collection.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h" // PathDiagnosticLocation
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"

using namespace clang;
using namespace ento;
using namespace llvm;
// field -> callee (FunctionDecl*) that may free receiver
REGISTER_MAP_WITH_PROGRAMSTATE(FieldToCalleeMap, const FieldDecl *,
                               const FunctionDecl *)
// field -> callsite stmt
REGISTER_MAP_WITH_PROGRAMSTATE(FieldToCallSiteMap, const FieldDecl *,
                               const Stmt *)

REGISTER_MAP_WITH_PROGRAMSTATE(ReportedForMethodMap, const FunctionDecl *, bool)

namespace {

static bool isInNamespace(const DeclContext *DC, StringRef Ns) {
  while (DC) {
    if (const auto *NS = dyn_cast<NamespaceDecl>(DC)) {
      const IdentifierInfo *II = NS->getIdentifier();
      if (II && II->isStr(Ns))
        return true;
    }
    DC = DC->getParent();
  }
  return false;
}

static bool isThisExpr(const Expr *E) {
  if (!E)
    return false;
  E = E->IgnoreParenCasts();
  return isa<CXXThisExpr>(E);
}

static bool isDeleteThis(const CXXDeleteExpr *DE) {
  if (!DE)
    return false;
  const Expr *Arg = DE->getArgument();
  if (!Arg)
    return false;
  return isThisExpr(Arg);
}

// Match expression `this->field` (MemberExpr with base = CXXThisExpr)
static const FieldDecl *getThisFieldFromMemberExpr(const Expr *E) {
  if (!E)
    return nullptr;
  E = E->IgnoreParenCasts();
  const auto *ME = dyn_cast<MemberExpr>(E);
  if (!ME)
    return nullptr;
  const Expr *Base = ME->getBase();
  if (!Base)
    return nullptr;
  if (!isThisExpr(Base))
    return nullptr;
  return dyn_cast<FieldDecl>(ME->getMemberDecl());
}

// Extract field from receiver of member call like `this->field->Method(...)`
static const FieldDecl *
getThisFieldReceiverFromMemberCallExpr(const Expr *OriginExpr) {
  if (!OriginExpr)
    return nullptr;

  const Expr *E = OriginExpr->IgnoreParenCasts();
  const auto *MCE = dyn_cast<CXXMemberCallExpr>(E);
  if (!MCE)
    return nullptr;

  const Expr *Obj = MCE->getImplicitObjectArgument();
  if (!Obj)
    return nullptr;

  return getThisFieldFromMemberExpr(Obj);
}

static bool isNullLikeRHS(const Expr *RHS) {
  if (!RHS)
    return false;
  RHS = RHS->IgnoreParenCasts();
  if (isa<CXXNullPtrLiteralExpr>(RHS))
    return true;
  if (isa<GNUNullExpr>(RHS))
    return true;
  if (const auto *IL = dyn_cast<IntegerLiteral>(RHS))
    return IL->getValue() == 0;
  return false;
}

class DtorFieldUseCollector : public ConstStmtVisitor<DtorFieldUseCollector> {
public:
  explicit DtorFieldUseCollector(SmallPtrSetImpl<const FieldDecl *> &Out)
      : OutFields(Out) {}

  void Visit(const Stmt *S) {
    if (!S)
      return;
    ConstStmtVisitor<DtorFieldUseCollector>::Visit(S);
    for (const Stmt *Child : S->children())
      Visit(Child);
  }

  // this->field used as receiver: this->field->Method(...)
  void VisitCXXMemberCallExpr(const CXXMemberCallExpr *CE) {
    if (!CE)
      return;
    const Expr *Obj = CE->getImplicitObjectArgument();
    if (const FieldDecl *FD = getThisFieldFromMemberExpr(Obj))
      OutFields.insert(FD);
  }

  // *this->field
  void VisitUnaryOperator(const UnaryOperator *UO) {
    if (!UO)
      return;
    if (UO->getOpcode() != UO_Deref)
      return;
    if (const FieldDecl *FD = getThisFieldFromMemberExpr(UO->getSubExpr()))
      OutFields.insert(FD);
  }

  // Passing this->field as argument (conservative "used")
  void VisitCallExpr(const CallExpr *CE) {
    if (!CE)
      return;
    for (const Expr *Arg : CE->arguments()) {
      if (const FieldDecl *FD = getThisFieldFromMemberExpr(Arg))
        OutFields.insert(FD);
    }
  }

private:
  SmallPtrSetImpl<const FieldDecl *> &OutFields;
};

class SelfDeleteFieldUAFChecker
    : public Checker<check::PreCall, check::PreStmt<CXXDeleteExpr>,
                     check::PreStmt<BinaryOperator>> {
  mutable std::unique_ptr<BugType> BT;

  mutable DenseMap<const FunctionDecl *, bool> HasDeleteThisCache;

  // Cache: class -> fields used/dereferenced in destructor
  mutable DenseMap<const CXXRecordDecl *, SmallPtrSet<const FieldDecl *, 8>>
      DtorUseCache;

  // Cache: function -> MayFreeReceiver?
  mutable DenseMap<const FunctionDecl *, bool> MayFreeCache;

  const SmallPtrSet<const FieldDecl *, 8> &
  getDtorUsedFields(const CXXRecordDecl *RD) const {
    auto It = DtorUseCache.find(RD);
    if (It != DtorUseCache.end())
      return It->second;

    SmallPtrSet<const FieldDecl *, 8> Empty;
    DtorUseCache[RD] = Empty;

    if (!RD)
      return DtorUseCache[RD];

    const CXXDestructorDecl *DD = RD->getDestructor();
    if (!DD)
      return DtorUseCache[RD];

    const CXXDestructorDecl *DefDD = DD;
    if (!DefDD->hasBody()) {
      for (const auto *R : DD->redecls()) {
        if (const auto *Cand = dyn_cast<CXXDestructorDecl>(R)) {
          if (Cand->hasBody()) {
            DefDD = Cand;
            break;
          }
        }
      }
    }
    if (!DefDD->hasBody())
      return DtorUseCache[RD];

    const Stmt *Body = DefDD->getBody();
    if (!Body)
      return DtorUseCache[RD];

    DtorFieldUseCollector Collector(DtorUseCache[RD]);
    Collector.Visit(Body);
    return DtorUseCache[RD];
  }

  bool hasDeleteThis(const FunctionDecl *FD) const {
    if (!FD || !FD->hasBody())
      return false;
    auto It = HasDeleteThisCache.find(FD);
    if (It != HasDeleteThisCache.end())
      return It->second;

    bool Found = false;
    const Stmt *Body = FD->getBody();

    class FindDeleteThis : public ConstStmtVisitor<FindDeleteThis> {
    public:
      bool &Found;
      explicit FindDeleteThis(bool &F) : Found(F) {}
      void Visit(const Stmt *S) {
        if (!S || Found)
          return;
        ConstStmtVisitor<FindDeleteThis>::Visit(S);
        for (const Stmt *C : S->children())
          Visit(C);
      }
      void VisitCXXDeleteExpr(const CXXDeleteExpr *DE) {
        if (isDeleteThis(DE))
          Found = true;
      }
    };

    FindDeleteThis V(Found);
    V.Visit(Body);
    HasDeleteThisCache[FD] = Found;
    return Found;
  }

  bool
  isMayFreeReceiverImpl(const FunctionDecl *FD,
                        SmallPtrSetImpl<const FunctionDecl *> &Visiting) const {
    if (!FD)
      return false;
    if (!FD->hasBody())
      return false;

    auto It = MayFreeCache.find(FD);
    if (It != MayFreeCache.end())
      return It->second;

    if (Visiting.contains(FD))
      return false;
    Visiting.insert(FD);

    bool Result = false;
    const Stmt *Body = FD->getBody();

    class Finder : public ConstStmtVisitor<Finder> {
    public:
      Finder(const SelfDeleteFieldUAFChecker &Self,
             SmallPtrSetImpl<const FunctionDecl *> &Visiting, bool &Out)
          : Self(Self), Visiting(Visiting), Out(Out) {}

      void Visit(const Stmt *S) {
        if (!S || Out)
          return;
        ConstStmtVisitor<Finder>::Visit(S);
        for (const Stmt *Child : S->children()) {
          if (Out)
            return;
          Visit(Child);
        }
      }

      void VisitCXXDeleteExpr(const CXXDeleteExpr *DE) {
        if (isDeleteThis(DE))
          Out = true;
      }

      // transitive: calls another MayFreeReceiver on `this`
      void VisitCXXMemberCallExpr(const CXXMemberCallExpr *CE) {
        if (!CE || Out)
          return;
        const Expr *Obj = CE->getImplicitObjectArgument();
        if (!Obj || !isThisExpr(Obj))
          return;
        const FunctionDecl *Callee = CE->getDirectCallee();
        if (!Callee)
          return;
        if (Self.isMayFreeReceiverImpl(Callee, Visiting))
          Out = true;
      }

    private:
      const SelfDeleteFieldUAFChecker &Self;
      SmallPtrSetImpl<const FunctionDecl *> &Visiting;
      bool &Out;
    };

    Finder F(*this, Visiting, Result);
    F.Visit(Body);

    Visiting.erase(FD);
    MayFreeCache[FD] = Result;
    return Result;
  }

  bool isMayFreeReceiver(const FunctionDecl *FD) const {
    SmallPtrSet<const FunctionDecl *, 16> Visiting;
    return isMayFreeReceiverImpl(FD, Visiting);
  }

  void reportBug(const CXXDeleteExpr *DeleteThisExpr, const FieldDecl *FD,
                 const FunctionDecl *Callee, const Stmt *CallSite,
                 const CXXRecordDecl *RD, CheckerContext &C) const {
    if (!BT) {
      BT.reset(new BugType(
          this, "Potential UAF: member pointer may be freed before delete this",
          "OH"));
    }

    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    std::string Msg = BT->getDescription().str();
    if (FD && FD->getIdentifier()) {
      Msg += " (field: ";
      Msg += FD->getNameAsString();
      Msg += ")";
    }

    auto R = std::make_unique<PathSensitiveBugReport>(*BT, Msg, N);
    R->addRange(DeleteThisExpr->getSourceRange());

    const SourceManager &SM = C.getSourceManager();
    const LocationContext *LCtx = C.getLocationContext();

    // Note: callsite that may free member pointee
    if (CallSite) {
      PathDiagnosticLocation PDL =
          PathDiagnosticLocation::createBegin(CallSite, SM, LCtx);
      std::string Note =
          "Call here may free the member pointee before `delete this`";
      if (Callee) {
        Note += " (callee: ";
        Note += Callee->getQualifiedNameAsString();
        Note += ")";
      }
      R->addNote(Note, PDL);
    }

    // Note: destructor uses the field
    if (RD) {
      if (const CXXDestructorDecl *DD = RD->getDestructor()) {
        PathDiagnosticLocation DLoc =
            PathDiagnosticLocation::createBegin(DD, SM);
        R->addNote("Destructor dereferences/uses the same field", DLoc);
      }
    }

    C.emitReport(std::move(R));
  }

public:
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const {
    const auto *Callee = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
    if (!Callee)
      return;


    if (!isInNamespace(Callee->getDeclContext(), "OHOS"))
      return;

    if (!isMayFreeReceiver(Callee))
      return;

    const FieldDecl *FD =
        getThisFieldReceiverFromMemberCallExpr(Call.getOriginExpr());
    if (!FD)
      return;

    const auto *CurMD =
        dyn_cast_or_null<CXXMethodDecl>(C.getStackFrame()->getDecl());
    if (!CurMD)
      return;

    const FunctionDecl *CurFD = CurMD;
    if (!hasDeleteThis(CurFD))
      return;

    const CXXRecordDecl *RD = CurMD->getParent();
    if (!RD)
      return;

    const auto &DtorFields = getDtorUsedFields(RD);
    if (!DtorFields.contains(FD))
      return;

    ProgramStateRef St = C.getState();


    if (const bool *Reported = St->get<ReportedForMethodMap>(CurFD)) {
      if (*Reported)
        return;
    }

    const Stmt *CallSite = dyn_cast_or_null<Stmt>(Call.getOriginExpr());


    St = St->set<FieldToCalleeMap>(FD, Callee);
    St = St->set<FieldToCallSiteMap>(FD, CallSite);


    reportBugAtCallSite(FD, Callee, CallSite, RD, C);


    St = St->set<ReportedForMethodMap>(CurFD, true);
    C.addTransition(St);
  }

  void reportBugAtCallSite(const FieldDecl *FD, const FunctionDecl *Callee,
                           const Stmt *CallSite, const CXXRecordDecl *RD,
                           CheckerContext &C) const {
    if (!BT) {
      BT.reset(new BugType(this,
                           "Potential UAF: member pointee may be freed and "
                           "later used in destructor",
                           "OH"));
    }
    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    std::string Msg = BT->getDescription().str();
    if (FD && FD->getIdentifier()) {
      Msg += " (field: " + FD->getNameAsString() + ")";
    }

    auto R = std::make_unique<PathSensitiveBugReport>(*BT, Msg, N);

    const SourceManager &SM = C.getSourceManager();
    const LocationContext *LCtx = C.getLocationContext();

    if (CallSite) {
      R->addRange(CallSite->getSourceRange());
      PathDiagnosticLocation PDL =
          PathDiagnosticLocation::createBegin(CallSite, SM, LCtx);
      std::string Note = "This call may free the member pointee";
      if (Callee) {
        Note += " (callee: " + Callee->getQualifiedNameAsString() + ")";
      }
      R->addNote(Note, PDL);
    }

    if (RD) {
      if (const CXXDestructorDecl *DD = RD->getDestructor()) {
        PathDiagnosticLocation DLoc =
            PathDiagnosticLocation::createBegin(DD, SM);
        R->addNote("Destructor dereferences/uses the same field", DLoc);
      }
    }

    C.emitReport(std::move(R));
  }

  // Kill risk when `this->field = nullptr/0;`
  void checkPreStmt(const BinaryOperator *BO, CheckerContext &C) const {
    if (!BO || !BO->isAssignmentOp())
      return;

    const FieldDecl *FD = getThisFieldFromMemberExpr(BO->getLHS());
    if (!FD)
      return;

    if (!isNullLikeRHS(BO->getRHS()))
      return;

    ProgramStateRef St = C.getState();
    St = St->remove<FieldToCalleeMap>(FD);
    St = St->remove<FieldToCallSiteMap>(FD);
    C.addTransition(St);
  }

  void checkPreStmt(const CXXDeleteExpr *DE, CheckerContext &C) const {
    if (!isDeleteThis(DE))
      return;

    const auto *CurMD =
        dyn_cast_or_null<CXXMethodDecl>(C.getStackFrame()->getDecl());
    if (!CurMD)
      return;

    const FunctionDecl *CurFD = CurMD;
    ProgramStateRef St = C.getState();


    if (const bool *Reported = St->get<ReportedForMethodMap>(CurFD)) {
      if (*Reported)
        return;
    }

    const CXXRecordDecl *RD = CurMD->getParent();
    if (!RD)
      return;

    const auto &DtorFields = getDtorUsedFields(RD);
    if (DtorFields.empty())
      return;

    auto Map = St->get<FieldToCalleeMap>();
    if (Map.isEmpty())
      return;

    for (auto I = Map.begin(), E = Map.end(); I != E; ++I) {
      const FieldDecl *FD = I->first;
      const FunctionDecl *Callee = I->second;
      if (!DtorFields.contains(FD))
        continue;

      const Stmt *CallSite = nullptr;
      auto CSMap = St->get<FieldToCallSiteMap>();
      if (!CSMap.isEmpty()) {
        if (const Stmt *const *P = CSMap.lookup(FD))
          CallSite = *P;
      }

      reportBug(DE, FD, Callee, CallSite, RD, C);


      St = St->set<ReportedForMethodMap>(CurFD, true);
      C.addTransition(St);
      return;
    }
  }
};

} // end anonymous namespace

void ento::registerSelfDeleteFieldUAFChecker(CheckerManager &Mgr) {
  Mgr.registerChecker<SelfDeleteFieldUAFChecker>();
}

bool ento::shouldRegisterSelfDeleteFieldUAFChecker(const CheckerManager &mgr) {
  return true;
}
