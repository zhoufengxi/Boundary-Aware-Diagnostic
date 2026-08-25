#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"

#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SValBuilder.h"

using namespace clang;
using namespace ento;

namespace {

static bool isLibcxxForEach(const FunctionDecl *FD) {
  if (!FD)
    return false;
  if (FD->getName() != "for_each")
    return false;
  std::string QN = FD->getQualifiedNameAsString();
  // libc++: std::__1::for_each
  return QN.find("std::__1::for_each") != std::string::npos;
}

class OHForEachFunctorMaterializeChecker : public Checker<check::PreCall> {
public:
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
};

void OHForEachFunctorMaterializeChecker::checkPreCall(const CallEvent &Call,
                                                      CheckerContext &C) const {
  const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!isLibcxxForEach(FD))
    return;
  if (Call.getNumArgs() < 3)
    return;


  SVal FVal = Call.getArgSVal(2);
  const MemRegion *FR = FVal.getAsRegion();
  if (!FR)
    return;


  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return;

  const auto *CE = dyn_cast<CallExpr>(Origin);
  if (!CE)
    return;
  if (CE->getNumArgs() < 3)
    return;

  const Expr *FArgE = CE->getArg(2)->IgnoreParenImpCasts();
  const auto *LE = dyn_cast<LambdaExpr>(FArgE);
  if (!LE)
    return;

  const CXXRecordDecl *ClosureRD = LE->getLambdaClass();
  if (!ClosureRD)
    return;

  ProgramStateRef St = C.getState();
  MemRegionManager &MRMgr = C.getSValBuilder().getRegionManager();
  const LocationContext *LCtx = C.getLocationContext();
  const auto *BaseTVR = dyn_cast_or_null<TypedValueRegion>(FR->getBaseRegion());
  if (!BaseTVR)
    return;


  SmallVector<const FieldDecl *, 8> Fields;
  for (const FieldDecl *FDf : ClosureRD->fields())
    Fields.push_back(FDf);

  unsigned I = 0;
  for (const LambdaCapture &Cap : LE->captures()) {
    if (I >= Fields.size())
      break;
    const FieldDecl *Field = Fields[I];
    QualType FT = Field->getType();


    if (!(FT->isPointerType() || FT->isReferenceType())) {
      ++I;
      continue;
    }


    const VarDecl *CapturedVD = Cap.getCapturedVar();
    if (!CapturedVD) {
      ++I;
      continue;
    }

    const VarRegion *CapturedVR = MRMgr.getVarRegion(CapturedVD, LCtx);
    if (!CapturedVR) {
      ++I;
      continue;
    }

    SVal CapturedV = St->getSVal(loc::MemRegionVal(CapturedVR));

    const FieldRegion *DstFR = MRMgr.getFieldRegion(Field, BaseTVR);
    if (!DstFR) {
      ++I;
      continue;
    }

    St = St->bindLoc(loc::MemRegionVal(DstFR), CapturedV, LCtx);

    ++I;
  }

  C.addTransition(St);
}

} // namespace

void ento::registerOHForEachFunctorMaterializeChecker(CheckerManager &Mgr) {
  Mgr.registerChecker<OHForEachFunctorMaterializeChecker>();
}
bool ento::shouldRegisterOHForEachFunctorMaterializeChecker(
    const CheckerManager &) {
  return true;
}
