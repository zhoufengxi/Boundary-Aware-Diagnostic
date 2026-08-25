//===-- DeleteFieldBeforeThrowingAssignChecker.cpp --------------*- C++ -*-===//
//
// Detect:
//
//   delete[] obj->field;
//   obj->field = may_throw_expr();
//
// If may_throw_expr() throws, the assignment does not take effect and
// obj->field remains dangling. A later cleanup path may delete it again.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "llvm/ADT/FoldingSet.h"
#include "clang/Analysis/PathDiagnostic.h"
#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SValBuilder.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymbolManager.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace ento;

#define DFBTA_DEBUG 1

#define DFBTA_LOG(MSG)                                                        \
  do {                                                                        \
    if (DFBTA_DEBUG) {                                                        \
      llvm::errs() << "[DFBTA] " << MSG;                                      \
    }                                                                         \
  } while (false)

static void dumpExprBrief(const char *Tag, const Expr *E) {
  if (!DFBTA_DEBUG)
    return;

  llvm::errs() << "[DFBTA] " << Tag << ": ";
  if (!E) {
    llvm::errs() << "<null>\n";
    return;
  }

  llvm::errs() << E->getStmtClassName() << " ptr=" << E << "\n";
  E->dump();
}

static void dumpStmtBrief(const char *Tag, const Stmt *S) {
  if (!DFBTA_DEBUG)
    return;

  llvm::errs() << "[DFBTA] " << Tag << ": ";
  if (!S) {
    llvm::errs() << "<null>\n";
    return;
  }

  llvm::errs() << S->getStmtClassName() << " ptr=" << S << "\n";
  S->dump();
}

static void dumpSValBrief(const char *Tag, SVal V) {
  if (!DFBTA_DEBUG)
    return;

  llvm::errs() << "[DFBTA] " << Tag << ": ";
  V.dumpToStream(llvm::errs());
  llvm::errs() << "\n";
}

static void dumpRegionBrief(const char *Tag, const MemRegion *R) {
  if (!DFBTA_DEBUG)
    return;

  llvm::errs() << "[DFBTA] " << Tag << ": ";
  if (!R) {
    llvm::errs() << "<null>\n";
    return;
  }

  llvm::errs() << "ptr=" << R << " kind=" << R->getKind() << " text=";
  R->dumpToStream(llvm::errs());
  llvm::errs() << "\n";
}

static void dumpLocBrief(const char *Tag, SourceLocation Loc,
                         const SourceManager &SM) {
  if (!DFBTA_DEBUG)
    return;

  llvm::errs() << "[DFBTA] " << Tag << ": ";
  if (Loc.isInvalid()) {
    llvm::errs() << "<invalid>\n";
    return;
  }

  llvm::errs() << Loc.printToString(SM) << "\n";
}

namespace {
struct ReleasedFieldInfo {
  const CXXDeleteExpr *DeleteExpr = nullptr;
  const Expr *DeleteArg = nullptr;
  SymbolRef DeletedSym = nullptr;
  bool IsArrayDelete = false;

  bool operator==(const ReleasedFieldInfo &Other) const {
    return DeleteExpr == Other.DeleteExpr &&
           DeleteArg == Other.DeleteArg &&
           DeletedSym == Other.DeletedSym &&
           IsArrayDelete == Other.IsArrayDelete;
  }

  bool operator!=(const ReleasedFieldInfo &Other) const {
    return !(*this == Other);
  }

  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddPointer(DeleteExpr);
    ID.AddPointer(DeleteArg);
    ID.AddPointer(DeletedSym);
    ID.AddInteger(IsArrayDelete ? 1 : 0);
  }
};

struct ReleasedFieldKey {
  const ValueDecl *BaseDecl = nullptr;
  const FieldDecl *Field = nullptr;
  bool IsThisBase = false;

  bool operator==(const ReleasedFieldKey &Other) const {
    return BaseDecl == Other.BaseDecl &&
           Field == Other.Field &&
           IsThisBase == Other.IsThisBase;
  }

  bool operator!=(const ReleasedFieldKey &Other) const {
    return !(*this == Other);
  }

  bool operator<(const ReleasedFieldKey &Other) const {
  uintptr_t ThisBase = reinterpret_cast<uintptr_t>(BaseDecl);
  uintptr_t OtherBase = reinterpret_cast<uintptr_t>(Other.BaseDecl);

  if (ThisBase != OtherBase)
    return ThisBase < OtherBase;

  uintptr_t ThisField = reinterpret_cast<uintptr_t>(Field);
  uintptr_t OtherField = reinterpret_cast<uintptr_t>(Other.Field);

  if (ThisField != OtherField)
    return ThisField < OtherField;

  return IsThisBase < Other.IsThisBase;
}

  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddPointer(BaseDecl);
    ID.AddPointer(Field);
    ID.AddInteger(IsThisBase ? 1 : 0);
  }
};
} // namespace

REGISTER_MAP_WITH_PROGRAMSTATE(ReleasedFieldMap,
                               ReleasedFieldKey,
                               ReleasedFieldInfo)

namespace {

class DeleteFieldBeforeThrowingAssignChecker
    : public Checker<check::PreStmt<CXXDeleteExpr>,
                     check::PreStmt<BinaryOperator>,
                     check::PostStmt<BinaryOperator>,
                     check::DeadSymbols> {
  mutable std::unique_ptr<BugType> BT;

public:
  void checkPreStmt(const CXXDeleteExpr *DE, CheckerContext &C) const;
//   void checkPostStmt(const CXXDeleteExpr *DE, CheckerContext &C) const;
  void checkPreStmt(const BinaryOperator *BO, CheckerContext &C) const;
  void checkPostStmt(const BinaryOperator *BO, CheckerContext &C) const;
  void checkDeadSymbols(SymbolReaper &SR, CheckerContext &C) const;

private:
  bool getFieldKey(const Expr *E, ReleasedFieldKey &Key,
                   CheckerContext &C) const;

  bool getFieldKeyFromMemberExpr(const MemberExpr *ME,
                                 ReleasedFieldKey &Key,
                                 CheckerContext &C) const;

  bool mayThrow(const Expr *E, CheckerContext &C) const;

  void reportBug(const BinaryOperator *BO, const ReleasedFieldKey &Key,
                 const ReleasedFieldInfo &Info, CheckerContext &C) const;
};

static const Expr *ignoreTransparentExprs(const Expr *E) {
  if (!E)
    return nullptr;

  return E->IgnoreParenImpCasts();
}

bool DeleteFieldBeforeThrowingAssignChecker::getFieldKeyFromMemberExpr(
    const MemberExpr *ME, ReleasedFieldKey &Key, CheckerContext &C) const {
  (void)C;

  DFBTA_LOG("getFieldKeyFromMemberExpr ENTER ME=" << ME << "\n");
  dumpExprBrief("field member expr", ME);

  if (!ME) {
    DFBTA_LOG("ME is null\n");
    return false;
  }

  const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
  if (!FD) {
    DFBTA_LOG("member decl is not FieldDecl\n");
    return false;
  }

  FD = FD->getCanonicalDecl();

  DFBTA_LOG("field name=" << FD->getNameAsString()
                          << " type=" << FD->getType().getAsString()
                          << "\n");

  if (!FD->getType()->isAnyPointerType()) {
    DFBTA_LOG("field is not raw pointer -> skip\n");
    return false;
  }

  const Expr *Base = ignoreTransparentExprs(ME->getBase());
  dumpExprBrief("field base after ignore", Base);

  if (!Base) {
    DFBTA_LOG("base is null\n");
    return false;
  }

  Key = ReleasedFieldKey();
  Key.Field = FD;

  // prop->value
  // obj.value
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
    const auto *VD = dyn_cast<ValueDecl>(DRE->getDecl());
    if (!VD) {
      DFBTA_LOG("base DeclRefExpr is not ValueDecl\n");
      return false;
    }

    Key.BaseDecl = cast<ValueDecl>(VD->getCanonicalDecl());
    Key.IsThisBase = false;

    DFBTA_LOG("built key from DeclRefExpr base="
              << Key.BaseDecl->getNameAsString()
              << " field=" << Key.Field->getNameAsString()
              << "\n");

    return true;
  }

  // this->value
  if (isa<CXXThisExpr>(Base)) {
    Key.BaseDecl = nullptr;
    Key.IsThisBase = true;

    DFBTA_LOG("built key from this base"
              << " field=" << Key.Field->getNameAsString()
              << "\n");

    return true;
  }

  // (*prop).value
  if (const auto *UO = dyn_cast<UnaryOperator>(Base)) {
    if (UO->getOpcode() == UO_Deref) {
      const Expr *Sub = ignoreTransparentExprs(UO->getSubExpr());
      dumpExprBrief("deref base subexpr", Sub);

      if (const auto *DRE = dyn_cast<DeclRefExpr>(Sub)) {
        const auto *VD = dyn_cast<ValueDecl>(DRE->getDecl());
        if (!VD) {
          DFBTA_LOG("deref base DeclRefExpr is not ValueDecl\n");
          return false;
        }

        Key.BaseDecl = cast<ValueDecl>(VD->getCanonicalDecl());
        Key.IsThisBase = false;

        DFBTA_LOG("built key from deref DeclRefExpr base="
                  << Key.BaseDecl->getNameAsString()
                  << " field=" << Key.Field->getNameAsString()
                  << "\n");

        return true;
      }
    }
  }

  // obj.inner.value


  if (const auto *BaseME = dyn_cast<MemberExpr>(Base)) {
    const auto *BaseFD = dyn_cast<FieldDecl>(BaseME->getMemberDecl());
    if (BaseFD) {
      Key.BaseDecl = BaseFD->getCanonicalDecl();
      Key.IsThisBase = false;

      DFBTA_LOG("built key from nested member base="
                << Key.BaseDecl->getNameAsString()
                << " field=" << Key.Field->getNameAsString()
                << "\n");

      return true;
    }
  }

  DFBTA_LOG("unsupported base expr for field key, class="
            << Base->getStmtClassName() << "\n");

  return false;
}

bool DeleteFieldBeforeThrowingAssignChecker::getFieldKey(
    const Expr *E, ReleasedFieldKey &Key, CheckerContext &C) const {
  DFBTA_LOG("getFieldKey ENTER\n");
  dumpExprBrief("input expr", E);

  E = ignoreTransparentExprs(E);
  dumpExprBrief("after IgnoreParenImpCasts", E);

  if (!E) {
    DFBTA_LOG("expr after ignore is null\n");
    return false;
  }

  const auto *ME = dyn_cast<MemberExpr>(E);
  if (!ME) {
    DFBTA_LOG("expr after ignore is not MemberExpr, class="
              << E->getStmtClassName() << "\n");
    return false;
  }

  return getFieldKeyFromMemberExpr(ME, Key, C);
}

static bool hasNoThrowExceptionSpec(const FunctionDecl *FD) {
  if (!FD)
    return false;

  const auto *FPT = FD->getType()->getAs<FunctionProtoType>();
  if (!FPT)
    return false;

  switch (FPT->getExceptionSpecType()) {
  case EST_DynamicNone:    // throw()
  case EST_BasicNoexcept:  // noexcept
  case EST_NoexceptTrue:   // noexcept(true)
    return true;

  case EST_None:             // no exception specification
  case EST_Dynamic:          // throw(T)
  case EST_MSAny:            // __declspec(nothrow)-unrelated MS any exception
  case EST_DependentNoexcept:
  case EST_NoexceptFalse:    // noexcept(false)
  case EST_Unevaluated:
  case EST_Uninstantiated:
  case EST_Unparsed:
    return false;
  }

  return false;
}

static bool exprMayThrowImpl(const Stmt *S) {
  if (!S)
    return false;

  DFBTA_LOG("exprMayThrowImpl visit " << S->getStmtClassName()
                                      << " ptr=" << S << "\n");

  if (isa<CXXThrowExpr>(S)) {
    DFBTA_LOG("mayThrow: found CXXThrowExpr\n");
    return true;
  }

  if (isa<CXXNewExpr>(S)) {
    DFBTA_LOG("mayThrow: found CXXNewExpr\n");
    return true;
  }

  if (const auto *CE = dyn_cast<CallExpr>(S)) {
    const FunctionDecl *FD = CE->getDirectCallee();

    if (!FD) {
      DFBTA_LOG("mayThrow: CallExpr has no direct callee -> true\n");
      return true;
    }

    DFBTA_LOG("mayThrow: CallExpr callee="
              << FD->getQualifiedNameAsString()
              << " type=" << FD->getType().getAsString() << "\n");

    bool NoThrow = hasNoThrowExceptionSpec(FD);
    DFBTA_LOG("mayThrow: callee noThrow=" << NoThrow << "\n");

    if (!NoThrow)
      return true;
  }

  if (const auto *CCE = dyn_cast<CXXConstructExpr>(S)) {
    const CXXConstructorDecl *Ctor = CCE->getConstructor();

    if (!Ctor) {
      DFBTA_LOG("mayThrow: CXXConstructExpr has no ctor -> true\n");
      return true;
    }

    DFBTA_LOG("mayThrow: ctor="
              << Ctor->getQualifiedNameAsString()
              << " type=" << Ctor->getType().getAsString() << "\n");

    bool NoThrow = hasNoThrowExceptionSpec(Ctor);
    DFBTA_LOG("mayThrow: ctor noThrow=" << NoThrow << "\n");

    if (!NoThrow)
      return true;
  }

  for (const Stmt *Child : S->children()) {
    if (exprMayThrowImpl(Child))
      return true;
  }

  return false;
}

bool DeleteFieldBeforeThrowingAssignChecker::mayThrow(const Expr *E,
                                                      CheckerContext &C) const {
  DFBTA_LOG("mayThrow ENTER\n");
  dumpExprBrief("mayThrow input", E);

  E = ignoreTransparentExprs(E);
  dumpExprBrief("mayThrow after ignore", E);

  if (!E) {
    DFBTA_LOG("mayThrow result=0 because expr is null\n");
    return false;
  }

  bool Ret = exprMayThrowImpl(E);
  DFBTA_LOG("mayThrow result=" << Ret << "\n");
  return Ret;
}

void DeleteFieldBeforeThrowingAssignChecker::checkPreStmt(
    const CXXDeleteExpr *DE, CheckerContext &C) const {
  DFBTA_LOG("==== checkPreStmt<CXXDeleteExpr> ENTER ====\n");

  if (!DE) {
    DFBTA_LOG("DE is null\n");
    return;
  }

  dumpLocBrief("delete loc", DE->getBeginLoc(), C.getSourceManager());
  dumpStmtBrief("delete expr", DE);

  const Expr *RawArg = DE->getArgument();
  dumpExprBrief("delete raw arg", RawArg);

  const Expr *Arg = ignoreTransparentExprs(RawArg);
  dumpExprBrief("delete arg after ignore", Arg);

  if (!Arg) {
    DFBTA_LOG("delete arg is null -> return\n");
    return;
  }

  ReleasedFieldKey Key;
  if (!getFieldKey(Arg, Key, C)) {
    DFBTA_LOG("cannot build released-field key from delete arg -> return\n");
    return;
  }

  ProgramStateRef State = C.getState();

  SVal DeletedV = State->getSVal(Arg, C.getLocationContext());
  dumpSValBrief("deleted value", DeletedV);

  if (DeletedV.isUnknownOrUndef()) {
    DFBTA_LOG("deleted value is unknown/undef, but still track syntax key\n");
  }

  if (DeletedV.isZeroConstant()) {
    DFBTA_LOG("deleted value is definitely null -> skip tracking\n");
    return;
  }

  ReleasedFieldInfo Info;
  Info.DeleteExpr = DE;
  Info.DeleteArg = Arg;
  Info.DeletedSym = DeletedV.getAsSymbol();
  Info.IsArrayDelete = DE->isArrayForm();

  State = State->set<ReleasedFieldMap>(Key, Info);

  DFBTA_LOG("tracked released field by key. contains="
            << State->contains<ReleasedFieldMap>(Key));

  if (Key.BaseDecl)
    DFBTA_LOG(" base=" << Key.BaseDecl->getNameAsString());
  else if (Key.IsThisBase)
    DFBTA_LOG(" base=this");
  else
    DFBTA_LOG(" base=<null>");

  DFBTA_LOG(" field=" << Key.Field->getNameAsString() << "\n");

  C.addTransition(State);

  DFBTA_LOG("==== checkPreStmt<CXXDeleteExpr> EXIT addTransition ====\n");
}

void DeleteFieldBeforeThrowingAssignChecker::checkPreStmt(
    const BinaryOperator *BO, CheckerContext &C) const {
  DFBTA_LOG("==== checkPreStmt<BinaryOperator> ENTER ====\n");

  if (!BO) {
    DFBTA_LOG("BO is null\n");
    return;
  }

  dumpLocBrief("binary loc", BO->getBeginLoc(), C.getSourceManager());
  DFBTA_LOG("binary opcode=" << BO->getOpcodeStr() << "\n");
  dumpStmtBrief("binary operator", BO);

  if (BO->getOpcode() != BO_Assign) {
    DFBTA_LOG("not assignment -> return\n");
    return;
  }

  dumpExprBrief("assign LHS raw", BO->getLHS());
  dumpExprBrief("assign RHS raw", BO->getRHS());

  ReleasedFieldKey Key;
  if (!getFieldKey(BO->getLHS(), Key, C)) {
    DFBTA_LOG("cannot build released-field key from assignment LHS -> return\n");
    return;
  }

  ProgramStateRef State = C.getState();

  bool Contains = State->contains<ReleasedFieldMap>(Key);

  DFBTA_LOG("released map contains LHS key=" << Contains);

  if (Key.BaseDecl)
    DFBTA_LOG(" base=" << Key.BaseDecl->getNameAsString());
  else if (Key.IsThisBase)
    DFBTA_LOG(" base=this");
  else
    DFBTA_LOG(" base=<null>");

  DFBTA_LOG(" field=" << Key.Field->getNameAsString() << "\n");

  const ReleasedFieldInfo *Info = State->get<ReleasedFieldMap>(Key);
  if (!Info) {
    DFBTA_LOG("no ReleasedFieldInfo for LHS key -> return\n");
    return;
  }

  const Expr *RHS = BO->getRHS();
  bool RHSMayThrow = mayThrow(RHS, C);
  DFBTA_LOG("RHS mayThrow=" << RHSMayThrow << "\n");

  if (!RHSMayThrow) {
    DFBTA_LOG("RHS cannot throw according to checker -> return\n");
    return;
  }

  DFBTA_LOG("about to report bug\n");
  reportBug(BO, Key, *Info, C);
}

void DeleteFieldBeforeThrowingAssignChecker::checkPostStmt(
    const BinaryOperator *BO, CheckerContext &C) const {
  DFBTA_LOG("==== checkPostStmt<BinaryOperator> ENTER ====\n");

  if (!BO) {
    DFBTA_LOG("BO is null\n");
    return;
  }

  DFBTA_LOG("post binary opcode=" << BO->getOpcodeStr() << "\n");

  if (BO->getOpcode() != BO_Assign) {
    DFBTA_LOG("post not assignment -> return\n");
    return;
  }

  ReleasedFieldKey Key;
  if (!getFieldKey(BO->getLHS(), Key, C)) {
    DFBTA_LOG("post cannot build key from assignment LHS -> return\n");
    return;
  }

  ProgramStateRef State = C.getState();
  bool Contains = State->contains<ReleasedFieldMap>(Key);

  DFBTA_LOG("post released map contains=" << Contains);

  if (Key.BaseDecl)
    DFBTA_LOG(" base=" << Key.BaseDecl->getNameAsString());
  else if (Key.IsThisBase)
    DFBTA_LOG(" base=this");
  else
    DFBTA_LOG(" base=<null>");

  DFBTA_LOG(" field=" << Key.Field->getNameAsString() << "\n");

  if (!Contains) {
    DFBTA_LOG("post no tracked state -> return\n");
    return;
  }

  State = State->remove<ReleasedFieldMap>(Key);

  DFBTA_LOG("post normal assignment succeeded; remove tracked field key\n");

  C.addTransition(State);
}

void DeleteFieldBeforeThrowingAssignChecker::checkDeadSymbols(
    SymbolReaper &SR, CheckerContext &C) const {
  (void)SR;

  DFBTA_LOG("==== checkDeadSymbols ENTER ====\n");

  ProgramStateRef State = C.getState();
  ReleasedFieldMapTy Map = State->get<ReleasedFieldMap>();

  unsigned Count = 0;
  for (ReleasedFieldMapTy::iterator I = Map.begin(), E = Map.end(); I != E;
       ++I) {
    ++Count;
  }

  DFBTA_LOG("tracked map size=" << Count << "\n");


  //
  //   BaseDecl + FieldDecl
  //


  //

}

static std::string getFieldNameForDiagnostic(const ReleasedFieldKey &Key) {
  if (Key.Field) {
    if (!Key.Field->getName().empty())
      return ("'" + Key.Field->getName().str() + "'");
  }

  return "the pointer field";
}

void DeleteFieldBeforeThrowingAssignChecker::reportBug(
    const BinaryOperator *BO, const ReleasedFieldKey &Key,
    const ReleasedFieldInfo &Info, CheckerContext &C) const {
  if (!BT) {
    BT = std::make_unique<BugType>(
        this, "Potential double free on exceptional path",
        "C++ exception safety");
  }

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  std::string FieldName = getFieldNameForDiagnostic(Key);

  SmallString<256> Buf;
  llvm::raw_svector_ostream OS(Buf);

  OS << "Potential double free on exceptional path: " << FieldName
     << " was released before this assignment; if the right-hand side throws, "
        "the assignment will not take effect and the field may be released "
        "again during cleanup";

  auto R = std::make_unique<PathSensitiveBugReport>(*BT, OS.str(), N);
  R->addRange(BO->getSourceRange());

  if (Info.DeletedSym)
    R->markInteresting(Info.DeletedSym);

  if (Info.DeleteExpr) {
    PathDiagnosticLocation DeleteLoc =
        PathDiagnosticLocation::createBegin(Info.DeleteExpr,
                                            C.getSourceManager(),
                                            C.getLocationContext());

    R->addNote("Pointer field is released here", DeleteLoc);
  }

  C.emitReport(std::move(R));
}

} // namespace

namespace clang {
namespace ento {

void registerDeleteFieldBeforeThrowingAssignChecker(CheckerManager &Mgr) {
  Mgr.registerChecker<DeleteFieldBeforeThrowingAssignChecker>();
}

bool shouldRegisterDeleteFieldBeforeThrowingAssignChecker(
    const CheckerManager &Mgr) {
  return true;
}

} // namespace ento
} // namespace clang