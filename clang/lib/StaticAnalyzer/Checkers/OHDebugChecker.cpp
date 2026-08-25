#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
using namespace clang;
using namespace clang::ento;

// namespace {

class OHDebugChecker : public Checker<check::PreStmt<CallExpr>> {
public:
  void checkPreStmt(const CallExpr *CE, CheckerContext &C) const {

    llvm::errs() << "OHOS- OHDebugChecker: Checking call expression\n";

    const FunctionDecl *FD = CE->getDirectCallee();
    if (FD) {
      llvm::errs() << "OHOS- Called function: " << FD->getNameAsString()
                   << "\n";
    }


    const ASTContext &Ctx = C.getASTContext();


    const QualType returnType = CE->getCallReturnType(Ctx);
    llvm::errs() << "OHOS- Return type of the called function: "
                 << returnType.getAsString() << "\n";


    for (unsigned i = 0; i < CE->getNumArgs(); ++i) {
      llvm::errs() << "OHOS- Argument " << i << ": "
                   << CE->getArg(i)->getType().getAsString() << "\n";
    }


    if (const LambdaExpr *LE = dyn_cast<LambdaExpr>(CE->getCallee())) {
      llvm::errs() << "OHOS- Lambda captured variables:\n";
      for (const auto &capture : LE->captures()) {
        llvm::errs() << "OHOS- Capture: "
                     << capture.getCapturedVar()->getNameAsString()
                     << "OHOS- (Kind: " << capture.getCaptureKind() << ")\n";
      }
    }


    if (FD && FD->getNameAsString() == "operator new") {
      llvm::errs() << "OHOS- Detected a new operator call in the expression!\n";
    }


    llvm::errs() << "OHOS- Function call location: "
                 << CE->getBeginLoc().printToString(C.getSourceManager())
                 << "\n";
  }
};


// namespace ento {
// void registerOHDebugChecker(CheckerManager &Mgr) {
//   Mgr.registerChecker<OHDebugChecker>();
// }
// } // namespace ento

// } // end anonymous namespace


// static CheckerRegistration<OHDebugChecker> X("oh-debug-checker",
//                                              "OH Debug Checker");

void ento::registerOHDebugChecker(CheckerManager &Mgr) {
  Mgr.registerChecker<OHDebugChecker>();
}

bool ento::shouldRegisterOHDebugChecker(const CheckerManager &) { return true; }