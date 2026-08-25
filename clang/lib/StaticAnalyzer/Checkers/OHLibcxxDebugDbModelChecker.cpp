#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"

using namespace clang;
using namespace ento;

namespace {

class OHLibcxxDebugDbModelChecker : public Checker<eval::Call> {
public:
  bool evalCall(const CallEvent &Call, CheckerContext &C) const {
    const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
    if (!FD) return false;


    if (FD->getName() != "__debug_db_erase_c")
      return false;


    std::string QN = FD->getQualifiedNameAsString();
    if (QN.find("std::__") == std::string::npos && QN.find("std::") == std::string::npos)
      return false;

    ProgramStateRef St = C.getState();
    C.addTransition(St);
    return true;
  }
};

} // namespace

void ento::registerOHLibcxxDebugDbModelChecker(CheckerManager &Mgr) {
  Mgr.registerChecker<OHLibcxxDebugDbModelChecker>();
}
bool ento::shouldRegisterOHLibcxxDebugDbModelChecker(const CheckerManager &) {
  return true;
}
