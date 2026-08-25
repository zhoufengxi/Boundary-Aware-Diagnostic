
// #include "PrettyStackTraceLocationContext.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/ASTImporter.h"
#include "clang/AST/ASTImporterSharedState.h"
#include "clang/AST/ASTStructuralEquivalence.h"
#include "clang/AST/Attr.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclAccessPair.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclGroup.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/DeclVisitor.h"
#include "clang/AST/DeclarationName.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/ExternalASTSource.h"
#include "clang/AST/LambdaCapture.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/StmtObjC.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/TemplateName.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/AST/TypeVisitor.h"
#include "clang/AST/UnresolvedSet.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Analysis/Analyses/LiveVariables.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/CFGStmtMap.h"
#include "clang/Analysis/ConstructionContext.h"
#include "clang/Analysis/PathDiagnostic.h"
#include "clang/Analysis/ProgramPoint.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/ExceptionSpecificationType.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/LLVM.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CrossTU/CrossTranslationUnit.h"
#include "clang/Lex/Lexer.h"
#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/BugReporter/CommonBugCategories.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallDescription.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerHelpers.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/DynamicExtent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/DynamicType.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/DynamicTypeInfo.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ExplodedGraph.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ExprEngine.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState_Fwd.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SValBuilder.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Store.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/StoreRef.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymbolManager.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/ImmutableList.h"
#include <optional>
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

using namespace clang;
using namespace ento;
static bool CSA_CTU_DBG() { return std::getenv("CSA_CTU_DBG") != nullptr; }
static bool isCTUInterestingDecl(const Decl *D) {
  if (!D)
    return false;

  const auto *ND = llvm::dyn_cast<NamedDecl>(D);
  if (!ND)
    return false;

  std::string Name = ND->getQualifiedNameAsString();

  // 只保留你关心的函数
  return Name.find("Listener::EmitByUv_test") != std::string::npos ||
         Name.find("Listener::operator=") != std::string::npos ||
         Name.find("GetReference") != std::string::npos ||
         Name.find("operator!=") != std::string::npos ||
         Name.find("basic_string") != std::string::npos ||
         Name.find("operator()") != std::string::npos; // lambda
}

static void dumpCTUDeclBrief(llvm::StringRef Prefix, const Decl *D) {
  llvm::errs() << Prefix;
  if (!D) {
    llvm::errs() << "<null>\n";
    return;
  }

  llvm::errs() << " ptr=" << (const void *)D
               << " kind=" << D->getDeclKindName();

  if (const auto *ND = llvm::dyn_cast<NamedDecl>(D))
    llvm::errs() << " qname=" << ND->getQualifiedNameAsString();

  if (const auto *FD = llvm::dyn_cast<FunctionDecl>(D)) {
    llvm::errs() << " templatedKind=" << (int)FD->getTemplatedKind()
                 << " isDef=" << FD->isThisDeclarationADefinition()
                 << " hasBody=" << FD->doesThisDeclarationHaveABody()
                 << " canonical=" << (const void *)FD->getCanonicalDecl();

    const FunctionDecl *Body = nullptr;
    if (FD->hasBody(Body) && Body)
      llvm::errs() << " bodyDecl=" << (const void *)Body;
    else
      llvm::errs() << " bodyDecl=<null>";

    if (const FunctionDecl *Pat = FD->getTemplateInstantiationPattern())
      llvm::errs() << " pattern=" << (const void *)Pat;
    else
      llvm::errs() << " pattern=<null>";
  }

  llvm::errs() << "\n";
}

static void dumpCTULoc(llvm::StringRef Prefix, const Decl *D) {
  llvm::errs() << Prefix;
  if (!D) {
    llvm::errs() << "<null>\n";
    return;
  }

  const auto &SM = D->getASTContext().getSourceManager();
  SourceLocation Loc = D->getLocation();
  if (Loc.isInvalid()) {
    llvm::errs() << "<invalid loc>\n";
    return;
  }

  Loc = SM.getExpansionLoc(Loc);
  llvm::errs() << Loc.printToString(SM) << "\n";
}

static void dumpCTUUSR(llvm::StringRef Prefix,
                       const std::optional<std::string> &USR) {
  llvm::errs() << Prefix;
  if (USR)
    llvm::errs() << *USR << "\n";
  else
    llvm::errs() << "<none>\n";
}

// static bool shouldLogCTUFD(const FunctionDecl *FD) {
//   if (!CSA_CTU_DBG() || !FD)
//     return false;

//   std::string QName = FD->getQualifiedNameAsString();

//   return QName.find("Listener::EmitByUv_test") != std::string::npos ||
//          QName.find("Listener::operator=") != std::string::npos ||
//          QName.find("GetReference") != std::string::npos;
// }

// static void dumpCTUFDOneLine(llvm::StringRef Prefix, const FunctionDecl *FD)
// {
//   llvm::errs() << Prefix;
//   if (!FD) {
//     llvm::errs() << "<null>\n";
//     return;
//   }

//   llvm::errs() << " qname=" << FD->getQualifiedNameAsString()
//                << " ptr=" << (const void *)FD
//                << " tk=" << (int)FD->getTemplatedKind()
//                << " def=" << FD->isThisDeclarationADefinition()
//                << " hasBody=" << FD->doesThisDeclarationHaveABody()
//                << " canon=" << (const void *)FD->getCanonicalDecl();

//   const FunctionDecl *Body = nullptr;
//   if (FD->hasBody(Body) && Body)
//     llvm::errs() << " body=" << (const void *)Body;
//   else
//     llvm::errs() << " body=<null>";

//   llvm::errs() << "\n";
// }

static bool shouldLogCTUFD(const FunctionDecl *FD) {
  if (!CSA_CTU_DBG() || !FD)
    return false;

  std::string QName = FD->getQualifiedNameAsString();

  // return QName == "std::for_each" ||
  //          QName.find("EventListener::EmitByUv") != std::string::npos ||
  //          QName.find("shared_from_this") != std::string::npos ||
  //          QName.find("InterfaceStateObserver::IfaceChangedCallback") !=
  //              std::string::npos ||
  //          QName.find("CallbackTemplate") != std::string::npos;
  return QName == "OHOS::NetManagerStandard::EventListener::EmitByUv";
}

static bool shouldLogCallFD(const clang::FunctionDecl *FD) {
  if (!CSA_CTU_DBG() || !FD)
    return false;

  std::string QName = FD->getQualifiedNameAsString();

  return QName == "std::for_each" ||
         QName.find("std::__h::operator!=") != std::string::npos ||
         QName == "Listener::EmitByUv_test" || QName == "GetReference";
}

static void dumpCallFDShort(llvm::StringRef Prefix,
                            const clang::FunctionDecl *FD) {
  llvm::errs() << Prefix;
  if (!FD) {
    llvm::errs() << "<null>\n";
    return;
  }

  llvm::errs() << " qname=" << FD->getQualifiedNameAsString()
               << " ptr=" << (const void *)FD
               << " tk=" << (int)FD->getTemplatedKind()
               << " def=" << FD->isThisDeclarationADefinition()
               << " hasBody=" << FD->doesThisDeclarationHaveABody()
               << " canon=" << (const void *)FD->getCanonicalDecl();

  const clang::FunctionDecl *Body = nullptr;
  if (FD->hasBody(Body) && Body)
    llvm::errs() << " body=" << (const void *)Body;
  else
    llvm::errs() << " body=<null>";

  if (const clang::FunctionDecl *Pat = FD->getTemplateInstantiationPattern())
    llvm::errs() << " pattern=" << (const void *)Pat;
  else
    llvm::errs() << " pattern=<null>";

  llvm::errs() << "\n";
}
static void dumpFDShort(llvm::StringRef Prefix, const FunctionDecl *FD) {
  llvm::errs() << Prefix;
  if (!FD) {
    llvm::errs() << "<null>\n";
    return;
  }

  llvm::errs() << " qname=" << FD->getQualifiedNameAsString()
               << " ptr=" << (const void *)FD
               << " tk=" << (int)FD->getTemplatedKind()
               << " def=" << FD->isThisDeclarationADefinition()
               << " hasBody=" << FD->doesThisDeclarationHaveABody()
               << " canon=" << (const void *)FD->getCanonicalDecl();

  const FunctionDecl *Body = nullptr;
  if (FD->hasBody(Body) && Body)
    llvm::errs() << " body=" << (const void *)Body;
  else
    llvm::errs() << " body=<null>";

  if (const FunctionDecl *Pat = FD->getTemplateInstantiationPattern())
    llvm::errs() << " pattern=" << (const void *)Pat;
  else
    llvm::errs() << " pattern=<null>";

  llvm::errs() << "\n";
}

static bool shouldLogEmitByUvFD(const FunctionDecl *FD) {
  return CSA_CTU_DBG() && FD &&
         FD->getQualifiedNameAsString() ==
             "OHOS::NetManagerStandard::EventListener::EmitByUv";
}

static void dumpCTUFDOneLine(llvm::StringRef Prefix, const FunctionDecl *FD) {
  llvm::errs() << Prefix;
  if (!FD) {
    llvm::errs() << "<null>\n";
    return;
  }

  llvm::errs() << " qname=" << FD->getQualifiedNameAsString()
               << " ptr=" << (const void *)FD
               << " tk=" << (int)FD->getTemplatedKind()
               << " def=" << FD->isThisDeclarationADefinition()
               << " hasBody=" << FD->doesThisDeclarationHaveABody()
               << " canon=" << (const void *)FD->getCanonicalDecl();

  const FunctionDecl *Body = nullptr;
  if (FD->hasBody(Body) && Body)
    llvm::errs() << " body=" << (const void *)Body;
  else
    llvm::errs() << " body=<null>";

  llvm::errs() << "\n";
}

static bool shouldLogEmitByUvLookup(llvm::StringRef LookupName) {
  return CSA_CTU_DBG() &&
         LookupName.find("EventListener@F@EmitByUv") != llvm::StringRef::npos;
}
///////////////////////////////////////////////////////////////

static void dumpCTURegion(llvm::StringRef Prefix,
                          const clang::ento::MemRegion *R) {
  llvm::errs() << Prefix;
  if (!R) {
    llvm::errs() << "<null>\n";
    return;
  }
  R->dumpToStream(llvm::errs());
  llvm::errs() << "\n";
}

static void dumpCTUCallTarget(const clang::ento::CallEvent &Call) {
  llvm::errs() << "[CTU-CALL] kind=" << Call.getKindAsString() << "\n";

  if (const Decl *D = Call.getDecl()) {
    llvm::errs() << "[CTU-CALL] callee decl kind=" << D->getDeclKindName()
                 << "\n";
    if (const auto *ND = llvm::dyn_cast<clang::NamedDecl>(D))
      llvm::errs() << "[CTU-CALL] callee qname="
                   << ND->getQualifiedNameAsString() << "\n";
  } else {
    llvm::errs() << "[CTU-CALL] callee decl=<null>\n";
  }

  if (const Expr *E = Call.getOriginExpr()) {
    llvm::errs() << "[CTU-CALL] origin expr class=" << E->getStmtClassName()
                 << "\n";
  } else {
    llvm::errs() << "[CTU-CALL] origin expr=<null>\n";
  }
}

static bool shouldLogInterestingCall(const clang::ento::CallEvent &Call) {
  const Decl *D = Call.getDecl();
  if (!D)
    return false;
  const auto *ND = llvm::dyn_cast<clang::NamedDecl>(D);
  if (!ND)
    return false;
  std::string Q = ND->getQualifiedNameAsString();

  return Q == "OHOS::NetManagerStandard::EventManager::EmitByUv" ||
         Q == "OHOS::NetManagerStandard::EventListener::EmitByUv" ||
         Q == "OHOS::NetManagerStandard::InterfaceStateObserver::"
              "IfaceChangedCallback" ||
         Q == "OHOS::NetManagerStandard::InterfaceStateObserver::"
              "CallbackTemplate" ||
         Q == "std::for_each";
}
////////

// static void dumpCTUSVal(llvm::StringRef Prefix, clang::ento::SVal V) {
//   llvm::errs() << Prefix;
//   V.dumpToStream(llvm::errs());
//   llvm::errs() << "\n";
// }

static bool shouldLogInterestingQName(llvm::StringRef Q) {
  return Q == "OHOS::NetManagerStandard::EventManager::EmitByUv" ||
         Q == "OHOS::NetManagerStandard::EventListener::EmitByUv" ||
         Q == "OHOS::NetManagerStandard::InterfaceStateObserver::"
              "IfaceChangedCallback" ||
         Q == "OHOS::NetManagerStandard::InterfaceStateObserver::"
              "CallbackTemplate" ||
         Q == "std::for_each";
}

static bool shouldLogInterestingDecl(const clang::Decl *D) {
  if (!CSA_CTU_DBG() || !D)
    return false;
  const auto *ND = llvm::dyn_cast<clang::NamedDecl>(D);
  if (!ND)
    return false;
  return shouldLogInterestingQName(ND->getQualifiedNameAsString());
}

static void dumpInterestingDecl(llvm::StringRef Prefix, const clang::Decl *D) {
  llvm::errs() << Prefix;
  if (!D) {
    llvm::errs() << "<null>\n";
    return;
  }
  llvm::errs() << D->getDeclKindName();
  if (const auto *ND = llvm::dyn_cast<clang::NamedDecl>(D))
    llvm::errs() << " qname=" << ND->getQualifiedNameAsString();
  llvm::errs() << " ptr=" << (const void *)D << "\n";
}

static void dumpCTUSVal(llvm::StringRef Prefix, clang::ento::SVal V) {
  llvm::errs() << Prefix;
  V.dumpToStream(llvm::errs());
  llvm::errs() << "\n";
}

static void dumpCTUSymbol(llvm::StringRef Prefix, clang::ento::SymbolRef Sym) {
  llvm::errs() << Prefix;
  if (!Sym) {
    llvm::errs() << "<null>\n";
    return;
  }
  Sym->dumpToStream(llvm::errs());
  llvm::errs() << "\n";
}

static bool shouldLogInterestingMallocDecl(const clang::Decl *D) {
  if (!CSA_CTU_DBG() || !D)
    return false;

  const auto *ND = llvm::dyn_cast<clang::NamedDecl>(D);
  if (!ND)
    return false;

  std::string Q = ND->getQualifiedNameAsString();

  return Q.find("InterfaceStateObserver") != std::string::npos ||
         Q.find("EventListener") != std::string::npos ||
         Q.find("EventManager") != std::string::npos ||
         Q.find("CallbackTemplate") != std::string::npos ||
         Q.find("IfaceChangedCallback::") != std::string::npos ||
         Q.find("EventManager::EmitByUv") != std::string::npos ||
         Q.find("NapiUtils") != std::string::npos;
}

// static bool shouldLogInterestingMallocDecl(const clang::Decl *D) {
//   if (!CSA_CTU_DBG() || !D)
//     return false;
//   const auto *ND = llvm::dyn_cast<clang::NamedDecl>(D);
//   if (!ND)
//     return false;
//   llvm::StringRef Q = ND->getQualifiedNameAsString();

//   return Q == "OHOS::NetManagerStandard::InterfaceStateObserver::"
//               "CallbackTemplate" ||
//          Q == "OHOS::NetManagerStandard::InterfaceStateObserver::"
//               "IfaceChangedCallback" ||
//          Q == "OHOS::NetManagerStandard::EventListener::EmitByUv" ||
//          Q == "OHOS::NetManagerStandard::EventManager::EmitByUv";
// }

static void dumpMallocCurrentFunction(const clang::ento::CheckerContext &C,
                                      llvm::StringRef Prefix) {
  llvm::errs() << Prefix;
  const LocationContext *LCtx = C.getLocationContext();
  if (!LCtx) {
    llvm::errs() << "<null>\n";
    return;
  }
  const clang::Decl *D = LCtx->getDecl();
  if (const auto *ND = llvm::dyn_cast_or_null<clang::NamedDecl>(D))
    llvm::errs() << ND->getQualifiedNameAsString();
  else if (D)
    llvm::errs() << D->getDeclKindName();
  else
    llvm::errs() << "<null>";
  llvm::errs() << "\n";
}

// static void dumpRefStateBrief(llvm::StringRef Prefix, const RefState *RS) {
//   llvm::errs() << Prefix;
//   if (!RS) {
//     llvm::errs() << "<null>\n";
//     return;
//   }

//   llvm::errs() << "allocated=" << RS->isAllocated()
//                << " size0=" << RS->isAllocatedOfSizeZero()
//                << " released=" << RS->isReleased()
//                << " relinquished=" << RS->isRelinquished()
//                << " escaped=" << RS->isEscaped()
//                << " family=" << (int)RS->getAllocationFamily()
//                << " stmt=" << (const void *)RS->getStmt() << "\n";
// }

// static bool shouldLogInterestingBugReport(const clang::ento::BugReport &R) {
//   if (!CSA_CTU_DBG())
//     return false;

//   llvm::StringRef Desc = R.getDescription();
//   llvm::StringRef ShortDesc = R.getShortDescription();
//   llvm::StringRef BugTypeDesc = R.getBugType().getDescription();
//   llvm::StringRef Check = R.getBugType().getCheckerName();

//   return Desc.contains("free") || Desc.contains("released memory") ||
//          Desc.contains("non-owned memory") || ShortDesc.contains("free") ||
//          BugTypeDesc.contains("Double free") || Check.contains("Malloc") ||
//          Check.contains("NewDelete");
// }

static void dumpBugLoc(const clang::ento::BugReport &R,
                       llvm::StringRef Prefix) {
  llvm::errs() << Prefix;

  clang::ento::PathDiagnosticLocation L = R.getLocation();
  if (!L.isValid()) {
    llvm::errs() << "<invalid>\n";
    return;
  }

  clang::SourceLocation SL = L.asLocation();
  if (!SL.isValid()) {
    llvm::errs()
        << "<valid PathDiagnosticLocation but invalid SourceLocation>\n";
    return;
  }

  llvm::errs() << "SourceLocation ptr enc=" << SL.getRawEncoding() << "\n";
}

static void dumpBugDecl(const clang::ento::BugReport &R,
                        llvm::StringRef Prefix) {
  llvm::errs() << Prefix;
  const clang::Decl *D = R.getDeclWithIssue();
  if (!D) {
    llvm::errs() << "<null>\n";
    return;
  }

  if (const auto *ND = llvm::dyn_cast<clang::NamedDecl>(D))
    llvm::errs() << ND->getQualifiedNameAsString();
  else
    llvm::errs() << D->getDeclKindName();

  llvm::errs() << " ptr=" << (const void *)D << "\n";
}

/////////////////////////////////////////////////////////////////

static bool shouldLogInterestingBugReport(const clang::ento::BugReport &R) {
  if (!CSA_CTU_DBG())
    return false;

  llvm::StringRef Desc = R.getDescription();
  llvm::StringRef ShortDesc = R.getShortDescription();
  llvm::StringRef BugTypeDesc = R.getBugType().getDescription();
  llvm::StringRef Checker = R.getBugType().getCheckerName();

  return Desc.contains("free") || Desc.contains("released memory") ||
         Desc.contains("non-owned memory") || ShortDesc.contains("free") ||
         BugTypeDesc.contains("Double free") || Checker.contains("Malloc") ||
         Checker.contains("NewDelete");
}

static void dumpBugReportBrief(const clang::ento::BugReport &R,
                               llvm::StringRef Prefix) {
  llvm::errs() << Prefix << " report=" << (const void *)&R
               << " checker=" << R.getBugType().getCheckerName()
               << " bugType=" << R.getBugType().getDescription() << " short=\""
               << R.getShortDescription() << "\""
               << " desc=\"" << R.getDescription() << "\"";

  const clang::Decl *D = R.getDeclWithIssue();
  if (const auto *ND = llvm::dyn_cast_or_null<clang::NamedDecl>(D))
    llvm::errs() << " issueDecl=" << ND->getQualifiedNameAsString();
  else if (D)
    llvm::errs() << " issueDeclKind=" << D->getDeclKindName();
  else
    llvm::errs() << " issueDecl=<null>";

  llvm::errs() << "\n";
}

static void
dumpPathDiagnosticLocationBrief(const clang::ento::PathDiagnosticLocation &L,
                                const clang::SourceManager &SM,
                                llvm::StringRef Prefix) {
  llvm::errs() << Prefix;

  if (!L.isValid()) {
    llvm::errs() << "<invalid>\n";
    return;
  }

  clang::SourceLocation SL = L.asLocation();
  if (!SL.isValid()) {
    llvm::errs() << "<invalid SourceLocation>\n";
    return;
  }

  clang::PresumedLoc PLoc = SM.getPresumedLoc(SL);
  if (PLoc.isValid()) {
    llvm::errs() << PLoc.getFilename() << ":" << PLoc.getLine() << ":"
                 << PLoc.getColumn() << "\n";
  } else {
    llvm::errs() << "raw=" << SL.getRawEncoding()
                 << " <invalid presumed loc>\n";
  }
}

static void dumpPathPiecesBrief(const clang::ento::PathPieces &Pieces,
                                const clang::SourceManager &SM,
                                llvm::StringRef Prefix) {
  unsigned I = 0;
  for (const auto &PieceRef : Pieces) {
    const clang::ento::PathDiagnosticPiece *Piece = PieceRef.get();
    if (!Piece) {
      llvm::errs() << Prefix << "[" << I++ << "] <null piece>\n";
      continue;
    }

    llvm::errs() << Prefix << "[" << I << "] kind=" << (int)Piece->getKind()
                 << " str=\"" << Piece->getString() << "\"\n";

    dumpPathDiagnosticLocationBrief(Piece->getLocation(), SM,
                                    llvm::Twine(Prefix)
                                        .concat("[")
                                        .concat(llvm::Twine(I))
                                        .concat("] loc=")
                                        .str());

    // 如果是 call piece，里面还有 nested path。
    if (const auto *Call =
            llvm::dyn_cast<clang::ento::PathDiagnosticCallPiece>(Piece)) {
      llvm::errs() << Prefix << "[" << I << "] call callee=\""
                   << Call->getCallee() << "\"\n";
      dumpPathPiecesBrief(Call->path, SM,
                          llvm::Twine(Prefix)
                              .concat("[")
                              .concat(llvm::Twine(I))
                              .concat("].call ")
                              .str());
    }

    ++I;
  }
}

// static bool isDoubleFreeReport(const clang::ento::BugReport *R) {
//   if (!R)
//     return false;

//   return R->getBugType().getDescription().contains("Double free") ||
//          R->getDescription().contains("free released memory") ||
//          R->getShortDescription().contains("free released memory");
// }

static bool isDoubleFreeReport(const clang::ento::BugReport *R) {
  if (!R)
    return false;

  return R->getBugType().getDescription().contains("Double free") ||
         R->getDescription().contains("free released memory") ||
         R->getDescription().contains("free released memory") ||
         R->getShortDescription().contains("free released memory") ||
         R->getDescription().contains("Attempt to free released memory");
}
// static const clang::ento::PathDiagnosticPiece *
// findFirstAllocationPiece(const clang::ento::PathPieces &Pieces) {
//   for (const auto &PieceRef : Pieces) {
//     const auto *Piece = PieceRef.get();
//     if (!Piece)
//       continue;

//     if (Piece->getString().contains("Memory is allocated"))
//       return Piece;

//     if (const auto *Call =
//             llvm::dyn_cast<clang::ento::PathDiagnosticCallPiece>(Piece)) {
//       if (const auto *Found = findFirstAllocationPiece(Call->path))
//         return Found;
//     }
//   }

//   return nullptr;
// }

static const clang::ento::PathDiagnosticPiece *
findFirstAllocationPiece(const clang::ento::PathPieces &Pieces) {
  for (const auto &PieceRef : Pieces) {
    const auto *Piece = PieceRef.get();
    if (!Piece)
      continue;

    if (Piece->getString().contains("Memory is allocated"))
      return Piece;

    if (const auto *Call =
            llvm::dyn_cast<clang::ento::PathDiagnosticCallPiece>(Piece)) {
      if (const auto *Found = findFirstAllocationPiece(Call->path))
        return Found;
    }
  }

  return nullptr;
}

static bool
topLevelAlreadyHasAllocationOrigin(const clang::ento::PathPieces &Pieces) {
  for (const auto &PieceRef : Pieces) {
    const auto *Piece = PieceRef.get();
    if (!Piece)
      continue;

    if (Piece->getString().contains("Memory was originally allocated here"))
      return true;
  }

  return false;
}

// static bool
// topLevelAlreadyHasAllocationOrigin(const clang::ento::PathPieces &Pieces) {
//   for (const auto &PieceRef : Pieces) {
//     const auto *Piece = PieceRef.get();
//     if (!Piece)
//       continue;

//     if (Piece->getString().contains("Memory was originally allocated here"))
//       return true;
//   }

//   return false;
// }

// static void
// promoteAllocationOriginForDoubleFree(const clang::ento::BugReport *Report,
//                                      clang::ento::PathDiagnostic &PD) {
//   if (!isDoubleFreeReport(Report))
//     return;

//   clang::ento::PathPieces &Pieces = PD.getMutablePieces();
//   if (Pieces.empty())
//     return;

//   if (topLevelAlreadyHasAllocationOrigin(Pieces))
//     return;

//   const clang::ento::PathDiagnosticPiece *AllocPiece =
//       findFirstAllocationPiece(Pieces);
//   if (!AllocPiece)
//     return;

//   clang::ento::PathDiagnosticLocation AllocLoc = AllocPiece->getLocation();
//   if (!AllocLoc.isValid())
//     return;

//   auto OriginPiece = std::make_shared<clang::ento::PathDiagnosticEventPiece>(
//       AllocLoc,
//       "Memory was originally allocated here; later it is freed twice");

//   Pieces.push_front(std::move(OriginPiece));
// }

static void dumpOnePieceBrief(const clang::ento::PathDiagnosticPiece *Piece,
                              const clang::SourceManager &SM,
                              llvm::StringRef Prefix) {
  llvm::errs() << Prefix;

  if (!Piece) {
    llvm::errs() << "<null>\n";
    return;
  }

  llvm::errs() << "kind=" << (int)Piece->getKind() << " str=\""
               << Piece->getString() << "\"";

  clang::ento::PathDiagnosticLocation L = Piece->getLocation();
  if (!L.isValid()) {
    llvm::errs() << " loc=<invalid>\n";
    return;
  }

  clang::SourceLocation SL = L.asLocation();
  if (!SL.isValid()) {
    llvm::errs() << " loc=<invalid SourceLocation>\n";
    return;
  }

  clang::PresumedLoc PLoc = SM.getPresumedLoc(SL);
  if (PLoc.isValid()) {
    llvm::errs() << " loc=" << PLoc.getFilename() << ":" << PLoc.getLine()
                 << ":" << PLoc.getColumn() << "\n";
  } else {
    llvm::errs() << " locRaw=" << SL.getRawEncoding() << "\n";
  }
}

static void dumpPiecesBrief(const clang::ento::PathPieces &Pieces,
                            const clang::SourceManager &SM,
                            llvm::StringRef Prefix, unsigned Depth = 0) {
  unsigned I = 0;
  for (const auto &PieceRef : Pieces) {
    const auto *Piece = PieceRef.get();

    SmallString<128> Pfx;
    llvm::raw_svector_ostream OS(Pfx);
    OS << Prefix << "[" << I << "] ";

    dumpOnePieceBrief(Piece, SM, OS.str());

    if (const auto *Call =
            llvm::dyn_cast_or_null<clang::ento::PathDiagnosticCallPiece>(
                Piece)) {
      if (Depth < 8) {
        SmallString<128> ChildPfx;
        llvm::raw_svector_ostream ChildOS(ChildPfx);
        ChildOS << Prefix << "[" << I << "].call ";
        dumpPiecesBrief(Call->path, SM, ChildOS.str(), Depth + 1);
      }
    }

    ++I;
  }
}
static const clang::ento::PathDiagnosticPiece *
findFirstAllocationPieceDbg(const clang::ento::PathPieces &Pieces,
                            const clang::SourceManager &SM,
                            unsigned Depth = 0) {
  unsigned I = 0;
  for (const auto &PieceRef : Pieces) {
    const auto *Piece = PieceRef.get();
    if (!Piece) {
      ++I;
      continue;
    }

    if (CSA_CTU_DBG()) {
      SmallString<128> Pfx;
      llvm::raw_svector_ostream OS(Pfx);
      OS << "[CTU-PROMOTE] scan depth=" << Depth << " idx=" << I << " ";
      dumpOnePieceBrief(Piece, SM, OS.str());
    }

    if (Piece->getString().contains("Memory is allocated")) {
      if (CSA_CTU_DBG()) {
        llvm::errs() << "[CTU-PROMOTE] FOUND allocation piece depth=" << Depth
                     << " idx=" << I << "\n";
      }
      return Piece;
    }

    if (const auto *Call =
            llvm::dyn_cast<clang::ento::PathDiagnosticCallPiece>(Piece)) {
      if (const auto *Found =
              findFirstAllocationPieceDbg(Call->path, SM, Depth + 1))
        return Found;
    }

    ++I;
  }

  return nullptr;
}
static void
promoteAllocationOriginForDoubleFree(const clang::ento::BugReport *Report,
                                     clang::ento::PathDiagnostic &PD,
                                     const clang::SourceManager &SM) {
  if (CSA_CTU_DBG()) {
    llvm::errs() << "\n[CTU-PROMOTE] ===== "
                    "promoteAllocationOriginForDoubleFree ENTER =====\n";
    llvm::errs() << "[CTU-PROMOTE] Report=" << (const void *)Report
                 << " PD=" << (const void *)&PD << "\n";

    if (Report) {
      llvm::errs() << "[CTU-PROMOTE] checker="
                   << Report->getBugType().getCheckerName()
                   << " bugType=" << Report->getBugType().getDescription()
                   << " desc=\"" << Report->getDescription() << "\"\n";

      const clang::Decl *D = Report->getDeclWithIssue();
      if (const auto *ND = llvm::dyn_cast_or_null<clang::NamedDecl>(D))
        llvm::errs() << "[CTU-PROMOTE] issueDecl="
                     << ND->getQualifiedNameAsString() << "\n";
      else if (D)
        llvm::errs() << "[CTU-PROMOTE] issueDeclKind=" << D->getDeclKindName()
                     << "\n";
      else
        llvm::errs() << "[CTU-PROMOTE] issueDecl=<null>\n";
    }

    llvm::errs() << "[CTU-PROMOTE] isDoubleFreeReport="
                 << isDoubleFreeReport(Report) << "\n";
    llvm::errs() << "[CTU-PROMOTE] path.size(before)=" << PD.path.size()
                 << "\n";
  }

  if (!isDoubleFreeReport(Report)) {
    if (CSA_CTU_DBG())
      llvm::errs() << "[CTU-PROMOTE] skip: not double free\n";
    return;
  }

  clang::ento::PathPieces &Pieces = PD.getMutablePieces();

  if (Pieces.empty()) {
    if (CSA_CTU_DBG())
      llvm::errs() << "[CTU-PROMOTE] skip: Pieces.empty\n";
    return;
  }

  if (CSA_CTU_DBG()) {
    llvm::errs() << "[CTU-PROMOTE] dumping pieces before promote\n";
    dumpPiecesBrief(Pieces, SM, "[CTU-PROMOTE] before ");
  }

  if (topLevelAlreadyHasAllocationOrigin(Pieces)) {
    if (CSA_CTU_DBG())
      llvm::errs() << "[CTU-PROMOTE] skip: top level already has origin\n";
    return;
  }

  const clang::ento::PathDiagnosticPiece *AllocPiece =
      findFirstAllocationPieceDbg(Pieces, SM);

  if (!AllocPiece) {
    if (CSA_CTU_DBG())
      llvm::errs() << "[CTU-PROMOTE] skip: allocation piece not found\n";
    return;
  }

  clang::ento::PathDiagnosticLocation AllocLoc = AllocPiece->getLocation();
  if (!AllocLoc.isValid()) {
    if (CSA_CTU_DBG())
      llvm::errs() << "[CTU-PROMOTE] skip: AllocLoc invalid\n";
    return;
  }

  if (CSA_CTU_DBG()) {
    dumpOnePieceBrief(AllocPiece, SM,
                      "[CTU-PROMOTE] selected allocation piece ");
  }

  // auto OriginPiece = std::make_shared<clang::ento::PathDiagnosticEventPiece>(
  //     AllocLoc,
  //     "Memory was originally allocated here; later it is freed twice");
  auto OriginPiece = std::make_shared<clang::ento::PathDiagnosticEventPiece>(
      AllocLoc, "Memory is allocated");
  if (CSA_CTU_DBG())
    llvm::errs() << "[CTU-PROMOTE] push_front origin piece\n";

  Pieces.push_front(std::move(OriginPiece));

  if (CSA_CTU_DBG()) {
    llvm::errs() << "[CTU-PROMOTE] path.size(after)=" << PD.path.size() << "\n";
    dumpPiecesBrief(Pieces, SM, "[CTU-PROMOTE] after ");
    llvm::errs() << "[CTU-PROMOTE] ===== promoteAllocationOriginForDoubleFree "
                    "EXIT =====\n";
  }
}

////////////////

static std::optional<clang::ento::PathDiagnosticLocation>
findSharedAllocationOriginForDoubleFree(
    const clang::ento::BugReport *Report,
    clang::ento::DiagnosticForConsumerMapTy &Diagnostics) {
  if (!isDoubleFreeReport(Report))
    return std::nullopt;

  for (auto &P : Diagnostics) {
    std::unique_ptr<clang::ento::PathDiagnostic> &PD = P.second;
    if (!PD)
      continue;

    const clang::ento::PathDiagnosticPiece *AllocPiece =
        findFirstAllocationPiece(PD->path);
    if (!AllocPiece)
      continue;

    clang::ento::PathDiagnosticLocation L = AllocPiece->getLocation();
    if (L.isValid())
      return L;
  }

  return std::nullopt;
}

static void promoteSharedAllocationOriginForDoubleFree(
    const clang::ento::BugReport *Report, clang::ento::PathDiagnostic &PD,
    const std::optional<clang::ento::PathDiagnosticLocation> &SharedAllocLoc) {
  if (!isDoubleFreeReport(Report))
    return;

  if (!SharedAllocLoc)
    return;

  clang::ento::PathPieces &Pieces = PD.getMutablePieces();

  if (topLevelAlreadyHasAllocationOrigin(Pieces))
    return;

  // auto OriginPiece = std::make_shared<clang::ento::PathDiagnosticEventPiece>(
  //     *SharedAllocLoc,
  //     "Memory was originally allocated here; later it is freed twice");
  auto OriginPiece = std::make_shared<clang::ento::PathDiagnosticEventPiece>(
      *SharedAllocLoc, "Memory is allocated");

  Pieces.push_front(std::move(OriginPiece));
}

static void dumpPDLoc(const clang::ento::PathDiagnosticLocation &L,
                      const clang::SourceManager &SM, llvm::StringRef Prefix) {
  llvm::errs() << Prefix;

  if (!L.isValid()) {
    llvm::errs() << "<invalid>\n";
    return;
  }

  clang::SourceLocation SL = L.asLocation();
  if (!SL.isValid()) {
    llvm::errs() << "<invalid SourceLocation>\n";
    return;
  }

  clang::PresumedLoc PLoc = SM.getPresumedLoc(SL);
  if (PLoc.isValid()) {
    llvm::errs() << PLoc.getFilename() << ":" << PLoc.getLine() << ":"
                 << PLoc.getColumn() << "\n";
  } else {
    llvm::errs() << "raw=" << SL.getRawEncoding()
                 << " <invalid presumed loc>\n";
  }
}

static void dumpOnePDPiece(const clang::ento::PathDiagnosticPiece *Piece,
                           const clang::SourceManager &SM,
                           llvm::StringRef Prefix) {
  llvm::errs() << Prefix;

  if (!Piece) {
    llvm::errs() << "<null>\n";
    return;
  }

  llvm::errs() << "kind=" << (int)Piece->getKind() << " str=\""
               << Piece->getString() << "\" ";

  dumpPDLoc(Piece->getLocation(), SM, "loc=");
}

static void dumpFullPathPieces(const clang::ento::PathPieces &Pieces,
                               const clang::SourceManager &SM,
                               llvm::StringRef Prefix, unsigned Depth = 0) {
  unsigned I = 0;
  for (const auto &PieceRef : Pieces) {
    const auto *Piece = PieceRef.get();

    SmallString<256> Pfx;
    llvm::raw_svector_ostream OS(Pfx);
    OS << Prefix << "depth=" << Depth << " idx=" << I << " ";

    dumpOnePDPiece(Piece, SM, OS.str());

    if (const auto *Call =
            llvm::dyn_cast_or_null<clang::ento::PathDiagnosticCallPiece>(
                Piece)) {
      SmallString<256> ChildPfx;
      llvm::raw_svector_ostream ChildOS(ChildPfx);
      ChildOS << Prefix << "depth=" << Depth << " idx=" << I << ".call ";
      dumpFullPathPieces(Call->path, SM, ChildOS.str(), Depth + 1);
    }

    ++I;
  }
}

static const clang::ento::PathDiagnosticPiece *
findFirstAllocationPieceWithLog(const clang::ento::PathPieces &Pieces,
                                const clang::SourceManager &SM,
                                unsigned Depth = 0) {
  unsigned I = 0;
  for (const auto &PieceRef : Pieces) {
    const auto *Piece = PieceRef.get();
    if (!Piece) {
      ++I;
      continue;
    }

    if (CSA_CTU_DBG() && (Piece->getString().contains("Memory") ||
                          Piece->getString().contains("free") ||
                          Piece->getString().contains("allocated"))) {
      SmallString<256> Pfx;
      llvm::raw_svector_ostream OS(Pfx);
      OS << "[CTU-SHARED] scan depth=" << Depth << " idx=" << I << " ";
      dumpOnePDPiece(Piece, SM, OS.str());
    }

    if (Piece->getString().contains("Memory is allocated")) {
      if (CSA_CTU_DBG()) {
        llvm::errs() << "[CTU-SHARED] FOUND allocation piece depth=" << Depth
                     << " idx=" << I << "\n";
        dumpOnePDPiece(Piece, SM, "[CTU-SHARED] allocation piece ");
      }
      return Piece;
    }

    if (const auto *Call =
            llvm::dyn_cast<clang::ento::PathDiagnosticCallPiece>(Piece)) {
      if (const auto *Found =
              findFirstAllocationPieceWithLog(Call->path, SM, Depth + 1))
        return Found;
    }

    ++I;
  }

  return nullptr;
}

static bool hasPromotedAllocationOrigin(const clang::ento::PathPieces &Pieces) {
  for (const auto &PieceRef : Pieces) {
    const auto *Piece = PieceRef.get();
    if (!Piece)
      continue;

    if (Piece->getString().contains("Memory was originally allocated here"))
      return true;
  }
  return false;
}

static std::optional<clang::ento::PathDiagnosticLocation>
findSharedAllocationOriginForDoubleFreeWithLog(
    const clang::ento::BugReport *Report,
    clang::ento::DiagnosticForConsumerMapTy &Diagnostics,
    const clang::SourceManager &SM) {
  // if (CSA_CTU_DBG()) {
  //   llvm::errs()
  //       << "\n[CTU-SHARED] ===== findSharedAllocationOrigin ENTER =====\n";
  //   llvm::errs() << "[CTU-SHARED] report=" << (const void *)Report << "\n";
  //   if (Report) {
  //     llvm::errs() << "[CTU-SHARED] checker="
  //                  << Report->getBugType().getCheckerName()
  //                  << " bugType=" << Report->getBugType().getDescription()
  //                  << " desc=\"" << Report->getDescription() << "\" short=\""
  //                  << Report->getShortDescription() << "\"\n";
  //   }
  //   llvm::errs() << "[CTU-SHARED] isDoubleFreeReport="
  //                << isDoubleFreeReport(Report) << "\n";
  //   llvm::errs() << "[CTU-SHARED] Diagnostics.size=" << Diagnostics.size()
  //                << "\n";
  // }

  if (!isDoubleFreeReport(Report)) {
    // if (CSA_CTU_DBG())
    //   llvm::errs() << "[CTU-SHARED] skip: not double free\n";
    return std::nullopt;
  }

  unsigned DiagIdx = 0;
  for (auto &P : Diagnostics) {
    PathDiagnosticConsumer *Consumer = P.first;
    std::unique_ptr<clang::ento::PathDiagnostic> &PD = P.second;

    // if (CSA_CTU_DBG()) {
    //   llvm::errs() << "[CTU-SHARED] diagnostic[" << DiagIdx
    //                << "] Consumer=" << (const void *)Consumer
    //                << " PD=" << (const void *)PD.get();
    //   if (PD)
    //     llvm::errs() << " path.size=" << PD->path.size();
    //   llvm::errs() << "\n";
    // }

    if (!PD) {
      ++DiagIdx;
      continue;
    }

    // if (CSA_CTU_DBG()) {
    //   llvm::errs() << "[CTU-SHARED] dump diagnostic[" << DiagIdx << "]
    //   path\n"; dumpFullPathPieces(PD->path, SM, "[CTU-SHARED]   ");
    // }

    const clang::ento::PathDiagnosticPiece *AllocPiece =
        findFirstAllocationPieceWithLog(PD->path, SM);

    if (AllocPiece) {
      clang::ento::PathDiagnosticLocation L = AllocPiece->getLocation();
      if (L.isValid()) {
        // if (CSA_CTU_DBG()) {
        //   llvm::errs()
        //       << "[CTU-SHARED] SharedAllocLoc selected from diagnostic["
        //       << DiagIdx << "] ";
        //   dumpPDLoc(L, SM, "loc=");
        //   llvm::errs() << "[CTU-SHARED] ===== findSharedAllocationOrigin EXIT
        //   "
        //                   "FOUND =====\n";
        // }
        return L;
      }

      // if (CSA_CTU_DBG())
      //   llvm::errs() << "[CTU-SHARED] allocation piece has invalid loc\n";
    }

    ++DiagIdx;
  }

  // if (CSA_CTU_DBG())
  //   llvm::errs()
  //       << "[CTU-SHARED] ===== findSharedAllocationOrigin EXIT NONE =====\n";

  return std::nullopt;
}

static void promoteSharedAllocationOriginForDoubleFreeWithLog(
    const clang::ento::BugReport *Report, clang::ento::PathDiagnostic &PD,
    const std::optional<clang::ento::PathDiagnosticLocation> &SharedAllocLoc,
    const clang::SourceManager &SM) {
  // if (CSA_CTU_DBG()) {
  //   llvm::errs() << "\n[CTU-PROMOTE2] ===== promoteShared ENTER =====\n";
  //   llvm::errs() << "[CTU-PROMOTE2] report=" << (const void *)Report
  //                << " PD=" << (const void *)&PD
  //                << " path.size(before)=" << PD.path.size() << "\n";
  //   llvm::errs() << "[CTU-PROMOTE2] isDoubleFreeReport="
  //                << isDoubleFreeReport(Report) << "\n";
  //   llvm::errs() << "[CTU-PROMOTE2] SharedAllocLoc.hasValue="
  //                << (bool)SharedAllocLoc << "\n";
  //   if (SharedAllocLoc)
  //     dumpPDLoc(*SharedAllocLoc, SM, "[CTU-PROMOTE2] SharedAllocLoc=");
  // }

  if (!isDoubleFreeReport(Report)) {
    // if (CSA_CTU_DBG())
    //   llvm::errs() << "[CTU-PROMOTE2] skip: not double free\n";
    return;
  }

  if (!SharedAllocLoc) {
    // if (CSA_CTU_DBG())
    //   llvm::errs() << "[CTU-PROMOTE2] skip: no shared alloc loc\n";
    return;
  }

  clang::ento::PathPieces &Pieces = PD.getMutablePieces();

  if (hasPromotedAllocationOrigin(Pieces)) {
    // if (CSA_CTU_DBG())
    //   llvm::errs() << "[CTU-PROMOTE2] skip: already promoted\n";
    return;
  }

  // if (CSA_CTU_DBG()) {
  //   // llvm::errs() << "[CTU-PROMOTE2] before promote pieces:\n";
  //   dumpFullPathPieces(Pieces, SM, "[CTU-PROMOTE2]   before ");
  // }

  // auto OriginPiece = std::make_shared<clang::ento::PathDiagnosticEventPiece>(
  //     *SharedAllocLoc,
  //     "Memory was originally allocated here; later it is freed twice");
  auto OriginPiece = std::make_shared<clang::ento::PathDiagnosticEventPiece>(
      *SharedAllocLoc, "Memory is allocated");
  Pieces.push_front(std::move(OriginPiece));

  // if (CSA_CTU_DBG()) {
  //   llvm::errs() << "[CTU-PROMOTE2] inserted origin piece\n";
  //   llvm::errs() << "[CTU-PROMOTE2] path.size(after)=" << PD.path.size()
  //                << "\n";
  //   dumpFullPathPieces(Pieces, SM, "[CTU-PROMOTE2]   after ");
  //   llvm::errs() << "[CTU-PROMOTE2] ===== promoteShared EXIT =====\n";
  // }
}

static bool hasCSAAllocationOrigin(const PathDiagnostic &D) {
  SmallVector<const PathPieces *, 8> WL;
  WL.push_back(&D.path);

  while (!WL.empty()) {
    const PathPieces &Pieces = *WL.pop_back_val();
    for (const auto &I : Pieces) {
      const PathDiagnosticPiece *P = I.get();
      if (!P)
        continue;

      if (P->getString().contains("Memory is allocated"))
        return true;

      if (const auto *Call = dyn_cast<PathDiagnosticCallPiece>(P))
        WL.push_back(&Call->path);
      else if (const auto *Macro = dyn_cast<PathDiagnosticMacroPiece>(P))
        WL.push_back(&Macro->subPieces);
    }
  }
  return false;
}