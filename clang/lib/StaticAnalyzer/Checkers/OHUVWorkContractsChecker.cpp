/* MY_OHOS_LOCAL_FILE */
// OHUVWorkContractsChecker.cpp
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Analysis/CFG.h"
#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ConstraintManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SValBuilder.h"

using namespace clang;
using namespace ento;

namespace {

enum class AsyncReleaseKind : unsigned {
  None = 0,
  PayloadFreedInAfterCb = 1,
};

struct AsyncReleaseKindTrait
    : public ProgramStatePartialTrait<AsyncReleaseKind> {
  static void *GDMIndex() {
    static int index;
    return &index;
  }
};

// Map payload symbol -> kind

class OHUVWorkContractsChecker
    : public Checker<check::PostCall, check::EndFunction> {
  mutable std::unique_ptr<BugType> BT;

  // Simple cache: FunctionDecl* -> frees_payload? (0/1/unknown=2)
  // Use mutable to allow caching in const callbacks.
  mutable llvm::DenseMap<const FunctionDecl *, unsigned> AfterCbCache;

public:
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkEndFunction(const ReturnStmt *RS, CheckerContext &C) const;

private:
  bool isUVQueueWorkWithQos(const CallEvent &Call) const;
  const FunctionDecl *getCalleeFD(const CallEvent &Call) const;

  // Extract callback FunctionDecl from argument (best-effort).
  const FunctionDecl *getCallbackFDFromArg(const CallEvent &Call,
                                           unsigned ArgIdx) const;

  // Extract req->data symbol from the "req" argument (best-effort).
  SymbolRef getReqDataSymbol(const CallEvent &Call, CheckerContext &C,
                             unsigned ReqArgIdx) const;

  // Summary: does after_cb free payload derived from req->data?
  bool afterCbFreesPayload(const FunctionDecl *AfterFD) const;

  bool scanFunctionForFreeOfParamData(const FunctionDecl *FD) const;
};

} // end anonymous namespace

REGISTER_MAP_WITH_PROGRAMSTATE(AsyncReleasedPayloads, SymbolRef, unsigned)
REGISTER_TRAIT_WITH_PROGRAMSTATE(EndFnAlreadyNoted, bool)

bool OHUVWorkContractsChecker::isUVQueueWorkWithQos(
    const CallEvent &Call) const {
  const FunctionDecl *FD = getCalleeFD(Call);
  if (!FD)
    return false;
  auto Name = FD->getName();
  return Name == "uv_queue_work_with_qos";
}

const FunctionDecl *
OHUVWorkContractsChecker::getCalleeFD(const CallEvent &Call) const {
  return dyn_cast_or_null<FunctionDecl>(Call.getDecl());
}

void OHUVWorkContractsChecker::checkPostCall(const CallEvent &Call,
                                             CheckerContext &C) const {
  if (!isUVQueueWorkWithQos(Call))
    return;

  // 1) Get return value symbol
  SVal RetV = Call.getReturnValue();
  auto RetSym = RetV.getAsSymbol();
  if (!RetSym) {
    // If return not symbolic, still try: if it's concrete int, handle quickly.
    if (auto CI = RetV.getAs<nonloc::ConcreteInt>()) {
      llvm::APSInt V = CI->getValue();
      if (V == 0) {
        // Success but we still need proof after_cb frees payload; handle below.
      } else {
        return; // Fail => do nothing
      }
    } else {
      return;
    }
  }

  ProgramStateRef State = C.getState();

  // 2) Split states on (ret == 0) and (ret != 0)
  SValBuilder &SVB = C.getSValBuilder();
  DefinedOrUnknownSVal RetD = RetV.castAs<DefinedOrUnknownSVal>();
  auto Zero = SVB.makeIntVal(0, SVB.getContext().IntTy);

  ProgramStateRef StateSuccess =
      State->assume(RetD, true); // placeholder; we'll use evalBinOp to compare.
  ProgramStateRef StateFail = State->assume(RetD, false);

  // More precise: build (ret == 0) condition
  SVal Cond = SVB.evalEQ(State, RetD, Zero.castAs<DefinedOrUnknownSVal>());
  auto CondD = Cond.getAs<DefinedOrUnknownSVal>();
  if (!CondD)
    return;

  ProgramStateRef Succ = State->assume(*CondD, true);
  ProgramStateRef Fail = State->assume(*CondD, false);

  if (!Succ && !Fail)
    return;

  // 3) On success branch, if after_cb frees payload, mark payload as
  // AsyncReleased
  if (Succ) {
    const unsigned ReqArgIdx = 1;
    const unsigned AfterCbArgIdx = 3;

    const FunctionDecl *AfterFD = getCallbackFDFromArg(Call, AfterCbArgIdx);
    SymbolRef PayloadSym = getReqDataSymbol(Call, C, ReqArgIdx);

    if (AfterFD && PayloadSym) {
      if (afterCbFreesPayload(AfterFD)) {
        Succ = Succ->set<AsyncReleasedPayloads>(
            PayloadSym,
            static_cast<unsigned>(AsyncReleaseKind::PayloadFreedInAfterCb));
      }
    }
    C.addTransition(Succ);
  }

  // 4) Fail branch: don't exempt anything. Keep it.
  if (Fail)
    C.addTransition(Fail);
}

const FunctionDecl *
OHUVWorkContractsChecker::getCallbackFDFromArg(const CallEvent &Call,
                                               unsigned ArgIdx) const {
  if (ArgIdx >= Call.getNumArgs())
    return nullptr;

  const Expr *E = Call.getArgExpr(ArgIdx);
  if (!E)
    return nullptr;
  E = E->IgnoreParenCasts();

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    return dyn_cast<FunctionDecl>(DRE->getDecl());
  }
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_AddrOf) {
      const Expr *Sub = UO->getSubExpr()->IgnoreParenCasts();
      if (const auto *DRE2 = dyn_cast<DeclRefExpr>(Sub))
        return dyn_cast<FunctionDecl>(DRE2->getDecl());
    }
  }

  // TODO: handle MemberExpr, lambda, std::function, etc. (later)
  return nullptr;
}

SymbolRef OHUVWorkContractsChecker::getReqDataSymbol(const CallEvent &Call,
                                                     CheckerContext &C,
                                                     unsigned ReqArgIdx) const {
  if (ReqArgIdx >= Call.getNumArgs())
    return nullptr;

  SVal ReqV = Call.getArgSVal(ReqArgIdx);
  ProgramStateRef State = C.getState();
  SValBuilder &SVB = C.getSValBuilder();

  // req is expected to be a pointer to a struct with field 'data'
  // We need to get the pointee region.
  const MemRegion *MR = ReqV.getAsRegion();
  if (!MR)
    return nullptr;

  // Try to get a typed region (ElementRegion / SymbolicRegion etc.)
  const auto *TR = dyn_cast<TypedRegion>(MR);
  if (!TR)
    return nullptr;

  QualType T = TR->getLocationType(); // pointer type
  const Type *TP = T.getTypePtrOrNull();
  if (!TP)
    return nullptr;

  const auto *PT = TP->getAs<PointerType>();
  if (!PT)
    return nullptr;

  QualType PointeeTy = PT->getPointeeType();
  const RecordType *RT = PointeeTy->getAs<RecordType>();
  if (!RT)
    return nullptr;

  const RecordDecl *RD = RT->getDecl();
  if (!RD)
    return nullptr;

  const FieldDecl *DataField = nullptr;
  for (const FieldDecl *F : RD->fields()) {
    if (F->getName() == "data") {
      DataField = F;
      break;
    }
  }
  if (!DataField)
    return nullptr;

  // Build req->data region:
  // First, get the region of *req (the pointee)
  // For symbolic regions, we can create an ElementRegion representing deref.
  // A common trick: use getAs<loc::MemRegionVal> and then load field via
  // StoreManager helpers. Here we do a conservative load using
  // ProgramState::getSVal(FieldRegion).

  const MemRegion *PointeeReg = MR->StripCasts();
  // If MR is a region for the pointer itself, we need its pointee. In practice,
  // Call.getArgSVal often is loc. A more reliable method: use the region
  // returned by ReqV as the pointee base if it’s already a region.
  const FieldRegion *FR = SVB.getRegionManager().getFieldRegion(
      DataField, cast<SubRegion>(PointeeReg));

  SVal DataV = State->getSVal(FR);
  return DataV.getAsSymbol();
}

bool OHUVWorkContractsChecker::afterCbFreesPayload(
    const FunctionDecl *AfterFD) const {
  if (!AfterFD)
    return false;
  auto It = AfterCbCache.find(AfterFD);
  if (It != AfterCbCache.end())
    return It->second == 1;

  bool Result = scanFunctionForFreeOfParamData(AfterFD);
  AfterCbCache[AfterFD] = Result ? 1u : 0u;
  return Result;
}

static bool isFreeLikeName(llvm::StringRef N) {
  return N == "free" || N == "uv_free" || N.endswith("_free");
}

bool OHUVWorkContractsChecker::scanFunctionForFreeOfParamData(
    const FunctionDecl *FD) const {
  if (!FD || !FD->hasBody())
    return false;

  const Stmt *Body = FD->getBody();

  // Heuristic: req is usually first arg (uv_work_t* req) for after_cb
  // signature. Adjust if needed by inspecting parameter types/names.
  if (FD->param_size() == 0)
    return false;
  const ParmVarDecl *ReqParam = FD->getParamDecl(0);

  // Walk AST
  class Walker : public RecursiveASTVisitor<Walker> {
  public:
    const ParmVarDecl *ReqParam;
    bool Found = false;

    explicit Walker(const ParmVarDecl *P) : ReqParam(P) {}

    bool VisitCXXDeleteExpr(CXXDeleteExpr *DE) {
      if (Found)
        return false;
      const Expr *E = DE->getArgument();
      if (exprDerivedFromReqData(E))
        Found = true;
      return true;
    }

    bool VisitCallExpr(CallExpr *CE) {
      if (Found)
        return false;
      const FunctionDecl *Callee = CE->getDirectCallee();
      if (!Callee)
        return true;
      llvm::StringRef Name = Callee->getName();
      if (!isFreeLikeName(Name))
        return true;

      if (CE->getNumArgs() >= 1) {
        const Expr *Arg0 = CE->getArg(0);
        if (exprDerivedFromReqData(Arg0))
          Found = true;
      }
      return true;
    }

  private:
    bool exprIsReq(const Expr *E) const {
      E = E ? E->IgnoreParenCasts() : nullptr;
      if (auto *DRE = dyn_cast_or_null<DeclRefExpr>(E))
        return DRE->getDecl() == ReqParam;
      return false;
    }

    bool exprDerivedFromReqData(const Expr *E) const {
      if (!E)
        return false;
      E = E->IgnoreParenCasts();

      // Match req->data or (*req).data
      if (auto *ME = dyn_cast<MemberExpr>(E)) {
        const ValueDecl *Member = ME->getMemberDecl();
        if (Member && Member->getName() == "data") {
          const Expr *Base = ME->getBase()->IgnoreParenCasts();
          // base can be req or *req
          if (exprIsReq(Base))
            return true;
          if (auto *UO = dyn_cast<UnaryOperator>(Base))
            if (UO->getOpcode() == UO_Deref && exprIsReq(UO->getSubExpr()))
              return true;
        }
      }

      // Match derived: auto p = req->data; free(p)
      // Very MVP: allow a single DeclRefExpr and see if it was initialized from
      // req->data (needs dataflow; skip now)
      return false;
    }
  };

  Walker W(ReqParam);
  W.TraverseStmt(const_cast<Stmt *>(Body));
  return W.Found;
}

void OHUVWorkContractsChecker::checkEndFunction(const ReturnStmt *RS,
                                                CheckerContext &C) const {
  ProgramStateRef State = C.getState();


  if (State->get<EndFnAlreadyNoted>())
    return;

  const auto &M = State->get<AsyncReleasedPayloads>();
  if (M.isEmpty())
    return;


  ExplodedNode *N = C.generateNonFatalErrorNode(State);
  if (!N)
    return;

  if (!BT)
    BT = std::make_unique<BugType>(this, "OH libuv contract note",
                                   "OpenHarmony contracts");

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "libuv contract applied: payload is considered freed in after_cb on "
      "ret==0",
      N);


  unsigned Count = 0;
  for (auto I = M.begin(); I != M.end(); ++I) {
    ++Count;
    if (Count > 10)
      break;
    SymbolRef Sym = I->first;
    unsigned Kind = I->second;

    SmallString<128> Buf;
    llvm::raw_svector_ostream OS(Buf);
    OS << "payload symbol: ";
    Sym->dumpToStream(OS);
    OS << ", kind=" << Kind;

    PathDiagnosticLocation L = PathDiagnosticLocation::createBegin(
        N->getLocationContext()->getDecl(), C.getSourceManager());

    R->addNote(OS.str(), L);
    // R->addNote(OS.str(), N->getLocation());
  }


  State = State->set<EndFnAlreadyNoted>(true);
  C.addTransition(State);

  C.emitReport(std::move(R));
}

void ento::registerOHUVWorkContractsChecker(CheckerManager &mgr) {
  mgr.registerChecker<OHUVWorkContractsChecker>();
}

bool ento::shouldRegisterOHUVWorkContractsChecker(const CheckerManager &mgr) {
  return true;
}