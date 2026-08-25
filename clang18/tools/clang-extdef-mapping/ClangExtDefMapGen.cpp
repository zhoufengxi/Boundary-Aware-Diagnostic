//===- ClangExtDefMapGen.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===--------------------------------------------------------------------===//
//
// Clang tool which creates a list of defined functions and the files in which
// they are defined.
//
//===--------------------------------------------------------------------===//

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/CrossTU/CrossTranslationUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>

using namespace llvm;
using namespace clang;
using namespace clang::cross_tu;
using namespace clang::tooling;

static cl::OptionCategory
    ClangExtDefMapGenCategory("clang-extdefmapgen options");

static bool CSA_CTU_VERBOSE() {
  return std::getenv("CSA_CTU_VERBOSE") != nullptr;
}

static cl::opt<std::string> NewDeleteRelevanceMapOutput(
    "newdelete-relevance-map-output",
    cl::desc("Output path for NewDelete/NewDeleteLeaks relevance summary map"),
    cl::init(""), cl::cat(ClangExtDefMapGenCategory));

static llvm::StringRef
extdefTemplatedKindToString(FunctionDecl::TemplatedKind K) {
  switch (K) {
  case FunctionDecl::TK_NonTemplate:
    return "TK_NonTemplate";
  case FunctionDecl::TK_FunctionTemplate:
    return "TK_FunctionTemplate";
  case FunctionDecl::TK_MemberSpecialization:
    return "TK_MemberSpecialization";
  case FunctionDecl::TK_FunctionTemplateSpecialization:
    return "TK_FunctionTemplateSpecialization";
  case FunctionDecl::TK_DependentFunctionTemplateSpecialization:
    return "TK_DependentFunctionTemplateSpecialization";
  }
  return "TK_Unknown";
}

static bool CSA_CTU_DBG() { return std::getenv("CSA_CTU_DBG") != nullptr; }

static void extdefDumpDeclLoc(llvm::StringRef Tag, const Decl *D) {
  if (!CSA_CTU_DBG())
    return;

  if (!D) {
    llvm::errs() << Tag << " <null decl>\n";
    return;
  }

  SourceLocation Loc = D->getLocation();
  if (Loc.isInvalid()) {
    llvm::errs() << Tag << " loc=<invalid>\n";
    return;
  }

  const SourceManager &SM = D->getASTContext().getSourceManager();
  SourceLocation ExpLoc = SM.getExpansionLoc(Loc);
  PresumedLoc PL = SM.getPresumedLoc(ExpLoc);

  llvm::errs() << Tag;

  if (PL.isValid()) {
    llvm::errs() << " loc=" << PL.getFilename() << ":" << PL.getLine() << ":"
                 << PL.getColumn();
  } else {
    llvm::errs() << " loc=<invalid presumed loc>";
  }

  llvm::errs() << " isInMainFile=" << SM.isInMainFile(ExpLoc) << "\n";
}

static void extdefDumpFunctionDeclBrief(llvm::StringRef Tag,
                                        const FunctionDecl *FD) {
  if (!CSA_CTU_DBG())
    return;

  llvm::errs() << Tag;

  if (!FD) {
    llvm::errs() << " <null>\n";
    return;
  }

  const FunctionDecl *BodyDecl = nullptr;
  bool HasBodyViaDef = FD->hasBody(BodyDecl);

  llvm::errs() << " ptr=" << (const void *)FD
               << " qname=" << FD->getQualifiedNameAsString()
               << " type=" << FD->getType().getAsString() << " templatedKind="
               << extdefTemplatedKindToString(FD->getTemplatedKind())
               << " hasBody=" << FD->hasBody()
               << " hasBodyViaDef=" << HasBodyViaDef
               << " bodyDecl=" << (const void *)BodyDecl
               << " getDefinition=" << (const void *)FD->getDefinition()
               << " getBody=" << (const void *)FD->getBody()
               << " isThisDef=" << FD->isThisDeclarationADefinition()
               << " canonical=" << (const void *)FD->getCanonicalDecl()
               << " previous=" << (const void *)FD->getPreviousDecl()
               << " mostRecent=" << (const void *)FD->getMostRecentDecl();

  if (const FunctionTemplateDecl *Primary = FD->getPrimaryTemplate()) {
    llvm::errs() << " primaryTemplate=" << (const void *)Primary;
    if (const FunctionDecl *Templated = Primary->getTemplatedDecl())
      llvm::errs() << " primaryTemplatedFD=" << (const void *)Templated;
  }

  if (const FunctionTemplateDecl *Described =
          FD->getDescribedFunctionTemplate()) {
    llvm::errs() << " describedTemplate=" << (const void *)Described;
  }

  llvm::errs() << "\n";
}

static void extdefDumpLookupName(llvm::StringRef Tag, const FunctionDecl *FD) {
  if (!CSA_CTU_DBG())
    return;

  std::optional<std::string> LookupName =
      CrossTranslationUnitContext::getLookupName(FD);

  llvm::errs() << Tag;
  if (LookupName)
    llvm::errs() << *LookupName;
  else
    llvm::errs() << "<null>";
  llvm::errs() << "\n";
}

static void extdefDumpTemplateSpecializationInfo(const FunctionDecl *FD) {
  if (!CSA_CTU_DBG())
    return;

  if (!FD)
    return;

  if (const FunctionTemplateSpecializationInfo *Info =
          FD->getTemplateSpecializationInfo()) {
    llvm::errs() << "[EXTDEF-TPL]   specialization info=" << (const void *)Info
                 << "\n";

    llvm::errs() << "[EXTDEF-TPL]   specialization kind="
                 << Info->getTemplateSpecializationKind() << "\n";

    if (FunctionTemplateDecl *InfoTemplate = Info->getTemplate()) {
      llvm::errs() << "[EXTDEF-TPL]   info template="
                   << (const void *)InfoTemplate
                   << " name=" << InfoTemplate->getNameAsString() << "\n";
    } else {
      llvm::errs() << "[EXTDEF-TPL]   info template=<null>\n";
    }

    SourceLocation POI = Info->getPointOfInstantiation();
    if (POI.isValid()) {
      const SourceManager &SM = FD->getASTContext().getSourceManager();
      PresumedLoc PL = SM.getPresumedLoc(SM.getExpansionLoc(POI));

      if (PL.isValid()) {
        llvm::errs() << "[EXTDEF-TPL]   point of instantiation="
                     << PL.getFilename() << ":" << PL.getLine() << ":"
                     << PL.getColumn() << "\n";
      } else {
        llvm::errs()
            << "[EXTDEF-TPL]   point of instantiation=<invalid presumed loc>\n";
      }
    } else {
      llvm::errs() << "[EXTDEF-TPL]   point of instantiation=<invalid>\n";
    }
  } else {
    llvm::errs() << "[EXTDEF-TPL]   specialization info=<null>\n";
  }

  if (const TemplateArgumentList *Args = FD->getTemplateSpecializationArgs()) {
    llvm::errs() << "[EXTDEF-TPL]   template args size=" << Args->size()
                 << "\n";

    for (unsigned I = 0; I < Args->size(); ++I) {
      llvm::errs() << "[EXTDEF-TPL]     arg[" << I << "] ";
      Args->get(I).print(FD->getASTContext().getPrintingPolicy(), llvm::errs(),
                         true);
      llvm::errs() << "\n";
    }
  } else {
    llvm::errs() << "[EXTDEF-TPL]   template args=<null>\n";
  }
}

static void extdefDumpFunctionBody(llvm::StringRef Tag,
                                   const FunctionDecl *FD) {
  if (!CSA_CTU_DBG())
    return;

  if (!FD) {
    llvm::errs() << Tag << " FD=<null>\n";
    return;
  }

  const FunctionDecl *Def = nullptr;
  bool HasBody = FD->hasBody(Def);

  llvm::errs() << Tag << " hasBody=" << HasBody << " Def=" << (const void *)Def
               << " getDefinition=" << (const void *)FD->getDefinition()
               << " getBody=" << (const void *)FD->getBody() << "\n";

  if (Def && Def->getBody()) {
    llvm::errs() << Tag << " Def body dump begin\n";
    Def->getBody()->dump();
    llvm::errs() << Tag << " Def body dump end\n";
  } else if (FD->getBody()) {
    llvm::errs() << Tag << " FD direct body dump begin\n";
    FD->getBody()->dump();
    llvm::errs() << Tag << " FD direct body dump end\n";
  } else {
    llvm::errs() << Tag << " body=<null>\n";
  }
}

static void extdefDumpRedeclChain(const FunctionDecl *FD) {
  if (!CSA_CTU_DBG())
    return;

  if (!FD)
    return;

  llvm::errs() << "[EXTDEF-TPL]   redecl chain begin\n";

  unsigned I = 0;
  for (const FunctionDecl *RD : FD->redecls()) {
    const FunctionDecl *RDDef = nullptr;
    bool RDHasBody = RD->hasBody(RDDef);

    llvm::errs() << "[EXTDEF-TPL]     redecl[" << I << "]\n";
    extdefDumpFunctionDeclBrief("[EXTDEF-TPL]       RD:", RD);
    extdefDumpDeclLoc("[EXTDEF-TPL]       RD", RD);
    extdefDumpLookupName("[EXTDEF-TPL]       RD lookup=", RD);

    llvm::errs() << "[EXTDEF-TPL]       RD hasBody=" << RDHasBody
                 << " RDDef=" << (const void *)RDDef
                 << " getDefinition=" << (const void *)RD->getDefinition()
                 << " getBody=" << (const void *)RD->getBody() << "\n";

    if (RDDef && RDDef->getBody()) {
      llvm::errs() << "[EXTDEF-TPL]       RDDef body dump begin\n";
      RDDef->getBody()->dump();
      llvm::errs() << "[EXTDEF-TPL]       RDDef body dump end\n";
    } else if (RD->getBody()) {
      llvm::errs() << "[EXTDEF-TPL]       RD direct body dump begin\n";
      RD->getBody()->dump();
      llvm::errs() << "[EXTDEF-TPL]       RD direct body dump end\n";
    } else {
      llvm::errs() << "[EXTDEF-TPL]       RD body=<null>\n";
    }

    ++I;
  }

  llvm::errs() << "[EXTDEF-TPL]   redecl chain end\n";
}

//===----------------------------------------------------------------------===//
// NewDelete/NewDeleteLeaks relevance summary for CTU import pruning.
//
// This summary is intentionally conservative:
//   relevant   -> allow CTU import
//   irrelevant -> may skip CTU import later if the call site is also safe
//
// If unsure, mark as relevant.
//===----------------------------------------------------------------------===//

struct NewDeleteRelevanceSummary {
  bool HasNew = false;
  bool HasDelete = false;
  bool HasPointerParam = false;
  bool HasPointerReturn = false;
  bool HasPointerUse = false;
  bool HasUnknownCall = false;
  bool HasPointerReturningCall = false;
  bool HasPointerLikeMemberAccess = false;
  bool HasMethodOnPointerLikeRecord = false;
  bool HasUnknownCallWithPointerArg = false;
  bool HasUnknownCallWithPointerReturn = false;

  bool isRelevantForEntryPruning() const {
    return HasNew || HasDelete || HasPointerParam || HasPointerReturn ||
           HasPointerUse || HasUnknownCall || HasPointerReturningCall ||
           HasPointerLikeMemberAccess || HasMethodOnPointerLikeRecord;
  }

  bool isRelevantForCTUImportPruning() const {
    return HasNew || HasDelete || HasPointerUse ||
           HasUnknownCallWithPointerArg ||
           HasUnknownCallWithPointerReturn ||
           HasMethodOnPointerLikeRecord;
  }
};

static bool isNDPointerLikeType(QualType QT) {
  if (QT.isNull())
    return false;

  QT = QT.getCanonicalType();

  if (QT->isPointerType() || QT->isObjCObjectPointerType() ||
      QT->isBlockPointerType() || QT->isMemberPointerType())
    return true;

  if (QT->isReferenceType()) {
    QualType PointeeTy = QT->getPointeeType().getCanonicalType();
    return PointeeTy->isPointerType() || PointeeTy->isObjCObjectPointerType() ||
           PointeeTy->isBlockPointerType() || PointeeTy->isMemberPointerType();
  }

  return false;
}

static bool isNDHeapCandidatePointerType(QualType QT) {
  if (QT.isNull())
    return false;

  QT = QT.getCanonicalType();

  if (QT->isReferenceType())
    QT = QT->getPointeeType().getCanonicalType();

  if (!QT->isPointerType())
    return false;

  QualType Pointee = QT->getPointeeType().getCanonicalType();

  // Character buffers and C strings are too common in OpenHarmony.
  // Do not treat them as strong NewDelete relevance signals here.
  if (Pointee->isCharType() || Pointee->isAnyCharacterType())
    return false;

  // void* is often used as opaque data/buffer. Let call-site gate handle it.
  if (Pointee->isVoidType())
    return false;

  return true;
}

static bool ndRecordHasPointerLikeField(const CXXRecordDecl *RD) {
  if (!RD)
    return false;

  const CXXRecordDecl *Def = RD->getDefinition();
  if (!Def)
    return false;

  for (const FieldDecl *F : Def->fields()) {
    if (isNDHeapCandidatePointerType(F->getType()))
      return true;
  }

  return false;
}

class NewDeleteRelevanceVisitor
    : public RecursiveASTVisitor<NewDeleteRelevanceVisitor> {
  NewDeleteRelevanceSummary Summary;

public:
  bool VisitCXXNewExpr(const CXXNewExpr *) {
    Summary.HasNew = true;
    return true;
  }

  bool VisitCXXDeleteExpr(const CXXDeleteExpr *) {
    Summary.HasDelete = true;
    return true;
  }

  bool VisitCallExpr(const CallExpr *CE) {
    if (!CE)
      return true;

    const FunctionDecl *Callee = CE->getDirectCallee();
    if (!Callee) {
      Summary.HasUnknownCall = true;

      for (const Expr *Arg : CE->arguments()) {
        if (Arg && isNDHeapCandidatePointerType(Arg->getType()))
          Summary.HasUnknownCallWithPointerArg = true;
      }

      if (isNDHeapCandidatePointerType(CE->getType()))
        Summary.HasUnknownCallWithPointerReturn = true;

      return true;
    }

    if (Callee->isOverloadedOperator()) {
      OverloadedOperatorKind OK = Callee->getOverloadedOperator();

      if (OK == OO_New || OK == OO_Array_New)
        Summary.HasNew = true;

      if (OK == OO_Delete || OK == OO_Array_Delete)
        Summary.HasDelete = true;
    }

    if (!Callee->hasBody()) {
      Summary.HasUnknownCall = true;

      for (const Expr *Arg : CE->arguments()) {
        if (Arg && isNDHeapCandidatePointerType(Arg->getType()))
          Summary.HasUnknownCallWithPointerArg = true;
      }

      if (isNDHeapCandidatePointerType(CE->getType()))
        Summary.HasUnknownCallWithPointerReturn = true;
    }

    return true;
  }

  bool VisitCXXMemberCallExpr(const CXXMemberCallExpr *CE) {
    if (!CE)
      return true;

    const CXXMethodDecl *MD = CE->getMethodDecl();
    if (!MD)
      return true;

    // If the member function is const, it is much less likely to mutate
    // pointer ownership state. Do not mark it relevant merely because the
    // class has pointer-like fields.
    if (MD->isConst())
      return true;

    // If the body is available, do not use the coarse "record has pointer
    // field" heuristic. Let the callee's own summary decide whether it is
    // relevant.
    if (MD->hasBody())
      return true;

    if (const CXXRecordDecl *RD = MD->getParent()) {
      if (ndRecordHasPointerLikeField(RD))
        Summary.HasMethodOnPointerLikeRecord = true;
    }

    return true;
  }

  bool VisitUnaryOperator(const UnaryOperator *UO) {
    if (!UO)
      return true;

    if (UO->getOpcode() == UO_Deref) {
      const Expr *Sub = UO->getSubExpr();
      if (Sub && isNDHeapCandidatePointerType(Sub->getType()))
        Summary.HasPointerUse = true;
    }

    return true;
  }

  bool VisitArraySubscriptExpr(const ArraySubscriptExpr *ASE) {
    if (!ASE)
      return true;

    const Expr *Base = ASE->getBase();
    if (Base && isNDHeapCandidatePointerType(Base->getType()))
      Summary.HasPointerUse = true;

    return true;
  }

  bool VisitMemberExpr(const MemberExpr *ME) {
    if (!ME)
      return true;

    if (isNDPointerLikeType(ME->getType()))
      Summary.HasPointerLikeMemberAccess = true;

    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      if (isNDPointerLikeType(FD->getType()))
        Summary.HasPointerLikeMemberAccess = true;
    }

    return true;
  }

  const NewDeleteRelevanceSummary &getSummary() const { return Summary; }
};

static NewDeleteRelevanceSummary
computeNewDeleteRelevanceSummary(const FunctionDecl *FD) {
  NewDeleteRelevanceSummary S;

  if (!FD)
    return S;

  const FunctionDecl *Def = nullptr;
  if (!FD->hasBody(Def) || !Def)
    return S;

  for (const ParmVarDecl *P : Def->parameters()) {
    if (isNDPointerLikeType(P->getType()))
      S.HasPointerParam = true;
  }

  if (isNDPointerLikeType(Def->getReturnType()))
    S.HasPointerReturn = true;

  if (const auto *MD = dyn_cast<CXXMethodDecl>(Def)) {
    if (!MD->isStatic()) {
      if (ndRecordHasPointerLikeField(MD->getParent()))
        S.HasMethodOnPointerLikeRecord = true;
    }
  }

  const Stmt *Body = Def->getBody();
  if (!Body)
    return S;

  NewDeleteRelevanceVisitor V;
  V.TraverseStmt(const_cast<Stmt *>(Body));

  const NewDeleteRelevanceSummary &BodyS = V.getSummary();

  S.HasNew |= BodyS.HasNew;
  S.HasDelete |= BodyS.HasDelete;
  S.HasPointerUse |= BodyS.HasPointerUse;
  S.HasUnknownCall |= BodyS.HasUnknownCall;
  S.HasPointerReturningCall |= BodyS.HasPointerReturningCall;
  S.HasPointerLikeMemberAccess |= BodyS.HasPointerLikeMemberAccess;
  S.HasMethodOnPointerLikeRecord |= BodyS.HasMethodOnPointerLikeRecord;
  S.HasUnknownCallWithPointerArg |= BodyS.HasUnknownCallWithPointerArg;
  S.HasUnknownCallWithPointerReturn |= BodyS.HasUnknownCallWithPointerReturn;

  return S;
}

static void dumpNewDeleteExtDefSummary(StringRef LookupName,
                                       const FunctionDecl *FD,
                                       const NewDeleteRelevanceSummary &S) {
  if (CSA_CTU_VERBOSE()) {
    llvm::errs() << "isCTU-relevant= " << S.isRelevantForCTUImportPruning()
                 << " \n";
    if (!S.isRelevantForCTUImportPruning())
      return;

    llvm::errs() << "[EXTDEF-ND] lookup=" << LookupName;

    llvm::errs() << " function=";
    if (FD)
      llvm::errs() << FD->getQualifiedNameAsString();
    else
      llvm::errs() << "<null>";

    llvm::errs() << " ctu-relevant=" << S.isRelevantForCTUImportPruning();

    llvm::errs() << " reason=";

    if (S.HasNew)
      llvm::errs() << "New,";
    if (S.HasDelete)
      llvm::errs() << "Delete,";
    if (S.HasPointerUse)
      llvm::errs() << "PointerUse,";
    if (S.HasUnknownCallWithPointerArg)
      llvm::errs() << "UnknownCallWithPointerArg,";
    if (S.HasUnknownCallWithPointerReturn)
      llvm::errs() << "UnknownCallWithPointerReturn,";
    if (S.HasPointerLikeMemberAccess)
      llvm::errs() << "PointerLikeMemberAccess,";
    if (S.HasMethodOnPointerLikeRecord)
      llvm::errs() << "MethodOnPointerLikeRecord,";

    llvm::errs() << "\n";
  }
}

class MapExtDefNamesConsumer : public ASTConsumer {
public:
  MapExtDefNamesConsumer(ASTContext &Context,
                         StringRef astFilePath = StringRef())
      : Ctx(Context), SM(Context.getSourceManager()) {
    CurrentFileName = astFilePath.str();
  }

  ~MapExtDefNamesConsumer() {
    // Keep the original behavior unchanged: external definition map goes to
    // stdout. Usually the caller redirects stdout to externalDefMap.txt.
    llvm::outs() << createCrossTUIndexString(Index);

    // New sidecar summary for NewDelete CTU import pruning.
    // Do not mix it into stdout, otherwise externalDefMap.txt format changes.
    if (!NewDeleteRelevanceMapOutput.empty() &&
        !NewDeleteRelevanceIndex.empty()) {
      std::error_code EC;
      llvm::raw_fd_ostream OS(NewDeleteRelevanceMapOutput, EC,
                              llvm::sys::fs::OF_Append);

      if (EC) {
        if (CSA_CTU_VERBOSE()) {
          llvm::errs() << "[EXTDEF-ND] failed to open relevance map: "
                       << NewDeleteRelevanceMapOutput
                       << " err=" << EC.message() << "\n";
        }
        return;
      }

      OS << createCrossTUIndexString(NewDeleteRelevanceIndex);

      if (CSA_CTU_VERBOSE()) {
        llvm::errs() << "[EXTDEF-ND] append relevance entries count="
                     << NewDeleteRelevanceIndex.size()
                     << " path=" << NewDeleteRelevanceMapOutput << "\n";
      }
    }
  }

  void HandleTranslationUnit(ASTContext &Context) override {
    handleDecl(Context.getTranslationUnitDecl());
  }

private:
  void handleDecl(const Decl *D);
  void addIfInMain(const DeclaratorDecl *DD, SourceLocation defStart);

  ASTContext &Ctx;
  SourceManager &SM;
  llvm::StringMap<std::string> Index;
  llvm::StringMap<std::string> NewDeleteRelevanceIndex;
  std::string CurrentFileName;
};

void MapExtDefNamesConsumer::handleDecl(const Decl *D) {
  if (!D)
    return;

  // FunctionTemplateDecl itself is not a DeclaratorDecl; the templated
  // FunctionDecl and its specializations are what generate lookup names.
  if (const auto *FTD = dyn_cast<FunctionTemplateDecl>(D)) {
    if (const auto *Templated = FTD->getTemplatedDecl()) {
      handleDecl(Templated);
    }

    for (const auto *Spec : FTD->specializations()) {
      if (!Spec)
        continue;
      handleDecl(Spec);
    }

    return;
  }

  if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
    const FunctionDecl *Def = nullptr;
    // hasBody(Def) is more robust than isThisDeclarationADefinition(),
    // especially for template specializations.
    bool HasBody = FD->hasBody(Def);
    if (HasBody && Def) {
      if (const Stmt *Body = Def->getBody()) {
        addIfInMain(Def, Body->getBeginLoc());
      }
    }
  } else if (const auto *VD = dyn_cast<VarDecl>(D)) {
    if (cross_tu::shouldImport(VD, Ctx) && VD->hasInit())
      if (const Expr *Init = VD->getInit())
        addIfInMain(VD, Init->getBeginLoc());
  }

  if (const auto *DC = dyn_cast<DeclContext>(D))
    for (const Decl *D : DC->decls())
      handleDecl(D);
}

void MapExtDefNamesConsumer::addIfInMain(const DeclaratorDecl *DD,
                                         SourceLocation defStart) {
  std::optional<std::string> LookupName =
      CrossTranslationUnitContext::getLookupName(DD);
  if (!LookupName)
    return;
  assert(!LookupName->empty() && "Lookup name should be non-empty.");

  if (CurrentFileName.empty()) {
    CurrentFileName = std::string(
        SM.getFileEntryForID(SM.getMainFileID())->tryGetRealPathName());
    if (CurrentFileName.empty())
      CurrentFileName = "invalid_file";
  }

  SourceLocation Loc = SM.getExpansionLoc(defStart);

  switch (DD->getLinkageInternal()) {
  case Linkage::External:
  case Linkage::VisibleNone:
  case Linkage::UniqueExternal:
    if (SM.isInMainFile(Loc)) {
      if (CSA_CTU_VERBOSE()) {
        llvm::errs() << "[EXTDEF-ADD] ADD mapping " << *LookupName << " -> "
                     << CurrentFileName << "\n";
      }
      Index[*LookupName] = CurrentFileName;

      // Generate NewDelete/NewDeleteLeaks relevance summary for functions
      // only. VarDecl entries are intentionally omitted from the relevance
      // map. Missing relevance entry means "unknown" and the analyzer must
      // keep the original CTU import behavior.
      if (const auto *FD = dyn_cast<FunctionDecl>(DD)) {
        NewDeleteRelevanceSummary S = computeNewDeleteRelevanceSummary(FD);
        NewDeleteRelevanceIndex[*LookupName] =
            S.isRelevantForCTUImportPruning() ? "1" : "0";
        dumpNewDeleteExtDefSummary(*LookupName, FD, S);
      }
    }
    break;
  case Linkage::Invalid:
    llvm_unreachable("Linkage has not been computed!");
  default:
    break;
  }
}

class MapExtDefNamesAction : public ASTFrontendAction {
protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 llvm::StringRef) override {
    return std::make_unique<MapExtDefNamesConsumer>(CI.getASTContext());
  }
};

static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

static IntrusiveRefCntPtr<DiagnosticsEngine> Diags;

IntrusiveRefCntPtr<DiagnosticsEngine> GetDiagnosticsEngine() {
  if (Diags) {
    // Call reset to make sure we don't mix errors
    Diags->Reset(false);
    return Diags;
  }

  IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts = new DiagnosticOptions();
  TextDiagnosticPrinter *DiagClient =
      new TextDiagnosticPrinter(llvm::errs(), &*DiagOpts);
  DiagClient->setPrefix("clang-extdef-mappping");
  IntrusiveRefCntPtr<DiagnosticIDs> DiagID(new DiagnosticIDs());

  IntrusiveRefCntPtr<DiagnosticsEngine> DiagEngine(
      new DiagnosticsEngine(DiagID, &*DiagOpts, DiagClient));
  Diags.swap(DiagEngine);

  // Retain this one time so it's not destroyed by ASTUnit::LoadFromASTFile
  Diags->Retain();
  return Diags;
}

static CompilerInstance *CI = nullptr;

static bool HandleAST(StringRef AstPath) {

  if (!CI)
    CI = new CompilerInstance();

  IntrusiveRefCntPtr<DiagnosticsEngine> DiagEngine = GetDiagnosticsEngine();

  std::unique_ptr<ASTUnit> Unit = ASTUnit::LoadFromASTFile(
      AstPath.str(), CI->getPCHContainerOperations()->getRawReader(),
      ASTUnit::LoadASTOnly, DiagEngine, CI->getFileSystemOpts(),
      CI->getHeaderSearchOptsPtr());

  if (!Unit)
    return false;

  FileManager FM(CI->getFileSystemOpts());
  SmallString<128> AbsPath(AstPath);
  FM.makeAbsolutePath(AbsPath);

  MapExtDefNamesConsumer Consumer =
      MapExtDefNamesConsumer(Unit->getASTContext(), AbsPath);
  Consumer.HandleTranslationUnit(Unit->getASTContext());

  return true;
}

static int HandleFiles(ArrayRef<std::string> SourceFiles,
                       CompilationDatabase &compilations) {
  std::vector<std::string> SourcesToBeParsed;

  // Loop over all input files, if they are pre-compiled AST
  // process them directly in HandleAST, otherwise put them
  // on a list for ClangTool to handle.
  for (StringRef Src : SourceFiles) {
    if (Src.ends_with(".ast")) {
      if (!HandleAST(Src)) {
        return 1;
      }
    } else {
      SourcesToBeParsed.push_back(Src.str());
    }
  }

  if (!SourcesToBeParsed.empty()) {
    ClangTool Tool(compilations, SourcesToBeParsed);
    return Tool.run(newFrontendActionFactory<MapExtDefNamesAction>().get());
  }

  return 0;
}

int main(int argc, const char **argv) {
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal(argv[0], false);
  PrettyStackTraceProgram X(argc, argv);

  const char *Overview = "\nThis tool collects the USR name and location "
                         "of external definitions in the source files "
                         "(excluding headers).\n"
                         "Input can be either source files that are compiled "
                         "with compile database or .ast files that are "
                         "created from clang's -emit-ast option.\n";
  auto ExpectedParser = CommonOptionsParser::create(
      argc, argv, ClangExtDefMapGenCategory, cl::ZeroOrMore, Overview);
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser &OptionsParser = ExpectedParser.get();

  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();

  if (!NewDeleteRelevanceMapOutput.empty()) {
    std::error_code EC;
    llvm::raw_fd_ostream OS(NewDeleteRelevanceMapOutput, EC,
                            llvm::sys::fs::OF_None);

    if (EC) {
      llvm::errs() << "failed to create NewDelete relevance map: "
                   << NewDeleteRelevanceMapOutput << " err=" << EC.message()
                   << "\n";
      return 1;
    }

    if (CSA_CTU_VERBOSE()) {
      llvm::errs() << "[EXTDEF-ND] reset relevance map: "
                   << NewDeleteRelevanceMapOutput << "\n";
    }
  }

  return HandleFiles(OptionsParser.getSourcePathList(),
                     OptionsParser.getCompilations());
}
