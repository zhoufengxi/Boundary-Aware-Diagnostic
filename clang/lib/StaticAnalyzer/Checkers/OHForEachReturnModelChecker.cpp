/* MY_OHOS_LOCAL_FILE */
// OHForEachReturnModelChecker.cpp
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"

using namespace clang;
using namespace ento;




///


class OHForEachReturnModelChecker : public Checker<check::PreStmt<ReturnStmt>> {
public:
  void checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const;
};

static bool isLibcxxForEach(const FunctionDecl *FD) {
  if (!FD)
    return false;
  const IdentifierInfo *II = FD->getIdentifier();
  if (!II)
    return false;
  if (II->getName() != "for_each")
    return false;



  const DeclContext *DC = FD->getDeclContext();
  bool sawStd = false, saw__1 = false;

  while (DC) {
    if (const auto *NS = dyn_cast<NamespaceDecl>(DC)) {
      StringRef N = NS->getName();
      if (N == "std")
        sawStd = true;
      if (N == "__1")
        saw__1 = true;
    }
    DC = DC->getParent();
  }
  return sawStd && saw__1;
}

void OHForEachReturnModelChecker::checkPreStmt(const ReturnStmt *RS,
                                               CheckerContext &C) const {
  const auto *SFC = C.getStackFrame();
  if (!SFC)
    return;

  const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(SFC->getDecl());
  if (!isLibcxxForEach(FD))
    return;

  const Expr *RetE = RS->getRetValue();
  if (!RetE)
    return;

  RetE = RetE->IgnoreParenImpCasts();


  const auto *DRE = dyn_cast<DeclRefExpr>(RetE);
  if (!DRE)
    return;

  const auto *PVD = dyn_cast<ParmVarDecl>(DRE->getDecl());
  if (!PVD)
    return;


  if (PVD->getName() != "__f")
    return;


  ProgramStateRef St = C.getState();
  SVal FunVal = C.getSVal(DRE);




  const LocationContext *LCtx = C.getLocationContext();
  St = St->BindExpr(RetE, LCtx, FunVal);

  C.addTransition(St);
}


void ento::registerOHForEachReturnModelChecker(CheckerManager &mgr) {
  mgr.registerChecker<OHForEachReturnModelChecker>();
}
bool ento::shouldRegisterOHForEachReturnModelChecker(const CheckerManager &) {
  return true;
}
