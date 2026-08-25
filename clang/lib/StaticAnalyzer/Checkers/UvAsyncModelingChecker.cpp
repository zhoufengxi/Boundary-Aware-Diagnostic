/* MY_OHOS_LOCAL_FILE */
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include <cstdlib>
#include "clang/StaticAnalyzer/Core/PathSensitive/UvAsyncModeling.h"
using namespace clang;
using namespace ento;


namespace {

static bool UvModelDbg() {
  return std::getenv("CSA_UV_MODEL_DBG") != nullptr && 1;
}

static void dumpSVal(StringRef Prefix, SVal V) {
  if (!UvModelDbg())
    return;

  llvm::errs() << "[UV-MODEL] " << Prefix << " = ";
  V.dumpToStream(llvm::errs());
  llvm::errs() << "\n";
}

static void dumpRegion(StringRef Prefix, const MemRegion *R) {
  if (!UvModelDbg())
    return;

  llvm::errs() << "[UV-MODEL] " << Prefix << " = ";
  if (R)
    R->dumpToStream(llvm::errs());
  else
    llvm::errs() << "<null>";
  llvm::errs() << "\n";
}



class UvAsyncModelingChecker
    : public Checker<eval::Call, check::PostCall> {
public:
  bool evalCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
private:
  bool isUvQueueWorkWithQos(const FunctionDecl *FD) const;

  const FieldDecl *findField(const RecordDecl *RD, StringRef Name) const;

  const RecordDecl *getPointeeRecordDecl(const Expr *E) const;
const FunctionDecl *getFunctionDeclFromCodeSVal(SVal V) const;
ProgramStateRef bindField(ProgramStateRef State,
                          const SubRegion *BaseR,
                          const FieldDecl *FD,
                          SVal V,
                          CheckerContext &C) const;
};

} // end anonymous namespace
const FunctionDecl *
UvAsyncModelingChecker::getFunctionDeclFromCodeSVal(SVal V) const {
  Optional<loc::MemRegionVal> MRV = V.getAs<loc::MemRegionVal>();
  if (!MRV)
    return nullptr;

  const MemRegion *R = MRV->getRegion();
  if (!R)
    return nullptr;

  if (const auto *FCR = dyn_cast<FunctionCodeRegion>(R)) {
    return dyn_cast_or_null<FunctionDecl>(FCR->getDecl());
  }

  return nullptr;
}
bool UvAsyncModelingChecker::isUvQueueWorkWithQos(
    const FunctionDecl *FD) const {
  if (!FD)
    return false;

  const IdentifierInfo *II = FD->getIdentifier();
  if (!II)
    return false;

  return II->getName() == "uv_queue_work_with_qos";
}

const FieldDecl *UvAsyncModelingChecker::findField(const RecordDecl *RD,
                                                   StringRef Name) const {
  if (!RD)
    return nullptr;

  const RecordDecl *Def = RD->getDefinition();
  if (Def)
    RD = Def;

  for (const FieldDecl *F : RD->fields()) {
    if (F->getName() == Name)
      return F;
  }

  return nullptr;
}

const RecordDecl *
UvAsyncModelingChecker::getPointeeRecordDecl(const Expr *E) const {
  if (!E)
    return nullptr;

  E = E->IgnoreParenImpCasts();

  QualType Ty = E->getType();

  if (!Ty->isPointerType())
    return nullptr;

  QualType PointeeTy = Ty->getPointeeType();

  const auto *RT = PointeeTy->getAs<RecordType>();
  if (!RT)
    return nullptr;

  const RecordDecl *RD = RT->getDecl();
  if (!RD)
    return nullptr;

  if (const RecordDecl *Def = RD->getDefinition())
    return Def;

  return RD;
}

ProgramStateRef UvAsyncModelingChecker::bindField(ProgramStateRef State,
                                                  const SubRegion *BaseR,
                                                  const FieldDecl *FD,
                                                  SVal V,
                                                  CheckerContext &C) const {
  if (!State || !BaseR || !FD)
    return State;

  SValBuilder &SVB = C.getSValBuilder();
  MemRegionManager &MRMgr = SVB.getRegionManager();

  const FieldRegion *FR = MRMgr.getFieldRegion(FD, BaseR);
  if (!FR)
    return State;

  if (UvModelDbg()) {
    llvm::errs() << "[UV-MODEL] bind field ";

llvm::errs() << FD->getName();

    llvm::errs() << " region = ";
    FR->dumpToStream(llvm::errs());
    llvm::errs() << "\n";
  }

  return State->bindLoc(loc::MemRegionVal(FR),
                        V,
                        C.getLocationContext());
}
bool UvAsyncModelingChecker::evalCall(const CallEvent &Call,
                                      CheckerContext &C) const {
  const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());

  if (!isUvQueueWorkWithQos(FD))
    return false;

  if (UvModelDbg()) {
    llvm::errs() << "\n[UV-MODEL] ===== matched uv_queue_work_with_qos =====\n";
    if (FD) {
      llvm::errs() << "[UV-MODEL] callee = ";
      FD->printQualifiedName(llvm::errs());
      llvm::errs() << "\n";
    }
    llvm::errs() << "[UV-MODEL] numArgs = " << Call.getNumArgs() << "\n";
  }

  if (Call.getNumArgs() != 5) {
    if (UvModelDbg())
      llvm::errs() << "[UV-MODEL] unexpected arg count, fallback\n";
    return false;
  }

  ProgramStateRef State = C.getState();
  SValBuilder &SVB = C.getSValBuilder();
  const LocationContext *LCtx = C.getLocationContext();

  // int uv_queue_work_with_qos(uv_loop_t *loop,
  //                            uv_work_t *req,
  //                            uv_work_cb work_cb,
  //                            uv_after_work_cb after_work_cb,
  //                            uv_qos_t qos);
  SVal LoopV = Call.getArgSVal(0);
  SVal ReqV = Call.getArgSVal(1);
  SVal WorkCbV = Call.getArgSVal(2);
  SVal AfterCbV = Call.getArgSVal(3);
  SVal QosV = Call.getArgSVal(4);

  dumpSVal("loop", LoopV);
  dumpSVal("req", ReqV);
  dumpSVal("work_cb", WorkCbV);
  dumpSVal("after_work_cb", AfterCbV);
  dumpSVal("qos", QosV);


  const FunctionDecl *AfterCbFuncDecl  = getFunctionDeclFromCodeSVal(AfterCbV);
  const FunctionDecl *WorkCbFuncDecl = getFunctionDeclFromCodeSVal(WorkCbV);


if (UvModelDbg()) {
  llvm::errs() << "[UV-MODEL] after_work_cb FD = ";
  if (AfterCbFuncDecl)
    AfterCbFuncDecl->printQualifiedName(llvm::errs());
  else
    llvm::errs() << "<null>";
  llvm::errs() << "\n";
}
if (UvModelDbg()) {
  llvm::errs() << "[UV-MODEL] work_cb FD = ";
  if (WorkCbFuncDecl)
    WorkCbFuncDecl->printQualifiedName(llvm::errs());
  else
    llvm::errs() << "<null>";
  llvm::errs() << "\n";

  llvm::errs() << "[UV-MODEL] after_work_cb FD = ";
  if (AfterCbFuncDecl)
    AfterCbFuncDecl->printQualifiedName(llvm::errs());
  else
    llvm::errs() << "<null>";
  llvm::errs() << "\n";
}
const MemRegion *ReqMR = ReqV.getAsRegion();
dumpRegion("req region", ReqMR);

const SubRegion *ReqR = dyn_cast_or_null<SubRegion>(ReqMR);
if (!ReqR) {
  if (UvModelDbg())
    llvm::errs() << "[UV-MODEL] req is not a SubRegion, fallback\n";
  return false;
}

  const Expr *ReqExpr = Call.getArgExpr(1);
  const RecordDecl *UvWorkRD = getPointeeRecordDecl(ReqExpr);

  if (!UvWorkRD) {
    if (UvModelDbg())
      llvm::errs() << "[UV-MODEL] cannot get uv_work_t record decl, fallback\n";
    return false;
  }

  if (UvModelDbg()) {
    llvm::errs() << "[UV-MODEL] req pointee record = ";
    UvWorkRD->printQualifiedName(llvm::errs());
    llvm::errs() << "\n";
  }

  const FieldDecl *WorkCbFD = findField(UvWorkRD, "work_cb");
  const FieldDecl *AfterCbFieldDecl  = findField(UvWorkRD, "after_work_cb");
const FieldDecl *DataFD = findField(UvWorkRD, "data");
if (DataFD) {
  MemRegionManager &MRMgr = SVB.getRegionManager();
  const FieldRegion *DataR = MRMgr.getFieldRegion(DataFD, ReqR);
  SVal DataV = State->getSVal(DataR);
  dumpSVal("req->data", DataV);
}
  if (!WorkCbFD || !AfterCbFieldDecl) {
    if (UvModelDbg()) {
      llvm::errs() << "[UV-MODEL] cannot find fields:";
      llvm::errs() << " work_cb=" << (const void *)WorkCbFD;
      llvm::errs() << " after_work_cb=" << (const void *)AfterCbFieldDecl;
      llvm::errs() << "\n";
    }
    return false;
  }

  // Model:
  //   req->work_cb = work_cb;
  //   req->after_work_cb = after_work_cb;
  //
  // Important:
  //   Do not invalidate req.
  //   Do not invalidate req->data.
  State = bindField(State, ReqR, WorkCbFD, WorkCbV, C);
  State = bindField(State, ReqR, AfterCbFieldDecl, AfterCbV, C);

if (WorkCbFuncDecl && ReqMR) {
  // First execute:
  //   work_cb(req)
  State = State->set<PendingUvCallbackFD>(WorkCbFuncDecl);
  State = State->set<PendingUvCallbackReq>(ReqMR);

  if (UvModelDbg()) {
    llvm::errs() << "[UV-MODEL] set pending work_cb: ";
    WorkCbFuncDecl->printQualifiedName(llvm::errs());
    llvm::errs() << "(";
    ReqMR->dumpToStream(llvm::errs());
    llvm::errs() << ")\n";
  }

  // Then execute:
  //   after_work_cb(req, 0)
  //
  // Do not inline it now. Save it until synthetic work_cb returns.
  if (AfterCbFuncDecl) {
    State = State->set<PendingUvAfterCallbackFD>(AfterCbFuncDecl);
    State = State->set<PendingUvAfterCallbackReq>(ReqMR);

    if (UvModelDbg()) {
      llvm::errs()
          << "[UV-MODEL] set pending after_work_cb after work_cb: ";
      AfterCbFuncDecl->printQualifiedName(llvm::errs());
      llvm::errs() << "(";
      ReqMR->dumpToStream(llvm::errs());
      llvm::errs() << ", 0)\n";
    }
  }
} else if (AfterCbFuncDecl && ReqMR) {
  // Fallback: no work_cb is available. Execute after_work_cb directly.
  State = State->set<PendingUvCallbackFD>(AfterCbFuncDecl);
  State = State->set<PendingUvCallbackReq>(ReqMR);

  if (UvModelDbg()) {
    llvm::errs() << "[UV-MODEL] set pending after_work_cb: ";
    AfterCbFuncDecl->printQualifiedName(llvm::errs());
    llvm::errs() << "(";
    ReqMR->dumpToStream(llvm::errs());
    llvm::errs() << ", 0)\n";
  }
}

  // Model success path:
  //   return 0;
  //
  // First version only models the successful submission path.
  // This avoids entering:
  //   if (ret != 0) delete work;
  //
  // Later we can add a failure branch if necessary.
  if (const Expr *Origin = Call.getOriginExpr()) {
    QualType RetTy = Call.getResultType();
    SVal RetZero = SVB.makeIntVal(0, RetTy);
    State = State->BindExpr(Origin, LCtx, RetZero);

    if (UvModelDbg()) {
      llvm::errs() << "[UV-MODEL] bind return value = 0\n";
    }
  }

  if (UvModelDbg()) {
    llvm::errs() << "[UV-MODEL] add success transition\n";
    llvm::errs() << "[UV-MODEL] ===== end uv_queue_work_with_qos model =====\n\n";
  }

  C.addTransition(State);
  return true;
}


void UvAsyncModelingChecker::checkPostCall(const CallEvent &Call,
                                           CheckerContext &C) const {
  const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());


  if (!isUvQueueWorkWithQos(FD))
    return;

  ProgramStateRef State = C.getState();

  const FunctionDecl *CallbackFD = State->get<PendingUvCallbackFD>();
  const MemRegion *ReqR = State->get<PendingUvCallbackReq>();

  if (!CallbackFD || !ReqR)
    return;

  if (UvModelDbg()) {
    llvm::errs() << "[UV-MODEL] PostCall sees pending callback: ";
    CallbackFD->printQualifiedName(llvm::errs());
    llvm::errs() << "(";
    ReqR->dumpToStream(llvm::errs());
    llvm::errs() << ", 0)\n";
  }



}


namespace clang {
namespace ento {

void registerUvAsyncModelingChecker(CheckerManager &Mgr) {
  Mgr.registerChecker<UvAsyncModelingChecker>();
}

bool shouldRegisterUvAsyncModelingChecker(const CheckerManager &Mgr) {
  return true;
}

} // namespace ento
} // namespace