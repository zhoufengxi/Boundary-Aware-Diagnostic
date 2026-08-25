/* MY_OHOS_LOCAL_FILE */
#include "clang/AST/DeclCXX.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"

using namespace clang;
using namespace ento;

namespace {

static bool isStd1BasicStringDecl(const CXXRecordDecl *RD) {
  if (!RD) return false;
  std::string QN = RD->getQualifiedNameAsString();
  return QN.find("std::__1::basic_string") != std::string::npos;
}

class OHStringDtorModelChecker : public Checker<eval::Call> {
public:
  bool evalCall(const CallEvent &Call, CheckerContext &C) const;
};



bool OHStringDtorModelChecker::evalCall(const CallEvent &Call,
                                        CheckerContext &C) const {
  const auto *DtorCall = dyn_cast<CXXDestructorCall>(&Call);
  if (!DtorCall) return false;

  const auto *DD = dyn_cast_or_null<CXXDestructorDecl>(DtorCall->getDecl());
  if (!DD) return false;

  const CXXRecordDecl *Parent = DD->getParent();
  if (!isStd1BasicStringDecl(Parent))
    return false;



  ProgramStateRef St = C.getState();
  C.addTransition(St);
  return true;
}


// bool OHStringDtorModelChecker::evalCall(const CallEvent &Call,
//                                         CheckerContext &C) const {
// const auto *DtorCall = dyn_cast<CXXDestructorCall>(&Call);
// if (!DtorCall) return false;

// const auto *DD = dyn_cast_or_null<CXXDestructorDecl>(DtorCall->getDecl());
// if (!DD) return false;

// const CXXRecordDecl *Parent = DD->getParent();
// if (!isStd1BasicStringDecl(Parent))
//   return false;



//   SVal ThisV = DtorCall->getCXXThisVal();
//   const MemRegion *ThisR = ThisV.getAsRegion();
//   if (!ThisR) return false;

//   ProgramStateRef St = C.getState();


//   SmallVector<const MemRegion *, 1> Regions;
//   Regions.push_back(ThisR);
//   St = St->invalidateRegions(Regions,
//                             Call.getOriginExpr(),
//                             C.blockCount(),
//                             C.getLocationContext(),
//                             /*CausesPointerEscape=*/false);

//   C.addTransition(St);
//   return true;
// }

} // namespace

void ento::registerOHStringDtorModelChecker(CheckerManager &Mgr) {
  Mgr.registerChecker<OHStringDtorModelChecker>();
}
bool ento::shouldRegisterOHStringDtorModelChecker(const CheckerManager &) {
  return true;
}
