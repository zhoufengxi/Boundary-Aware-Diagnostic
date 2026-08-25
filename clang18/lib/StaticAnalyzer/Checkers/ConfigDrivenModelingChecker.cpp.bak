//===-- ConfigDrivenModelingChecker.cpp ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A strict YAML-driven call-summary and resource-protocol checker for C/C++.
// The checker deliberately contains no project or library specific API names.
//
//===----------------------------------------------------------------------===//

#include "AllocationState.h"
#include "Yaml.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/CrossTU/CrossTranslationUnit.h"
#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"
#include "clang/StaticAnalyzer/Core/AnalyzerOptions.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ConfigDrivenCallbacks.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ExprEngine.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/YAMLTraits.h"
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace clang;
using namespace clang::ento;

namespace config_modeling {

enum class CallKind {
  Function,
  StaticMethod,
  InstanceMethod,
  Constructor,
  Destructor
};
enum class EvaluationKind { Conservative, Pure, Custom };
enum class SelectorBase { Return, This, Argument };
enum class SelectorMode { Value, Region };
enum class CompareOp { EQ, NE, LT, LE, GT, GE };
enum class ConditionKind { Value, Choice, ProtocolState, Counter };
enum class ValueKind { Integer, Boolean, Null, Selector, Conjured, Unknown };
enum class EffectKind {
  Bind,
  Invalidate,
  OwnershipAcquire,
  OwnershipRelease,
  OwnershipTransfer,
  OwnershipEscape,
  ProtocolSet,
  ProtocolTransition,
  ProtocolAlias,
  ProtocolEscape,
  CounterSet,
  CounterAdjust,
  InvokeCallback,
  Terminate
};
enum class ProtocolKind { Typestate, Counter };
enum class CounterMode { Lifetime, Balance };
enum class UnknownCallPolicy { Escape, Preserve };
enum class AllocationFamily { Malloc, CXXNew, CXXNewArray, Alloca };

struct Selector {
  SelectorBase Base = SelectorBase::Argument;
  unsigned Index = 0;
  unsigned Dereference = 0;
  std::vector<std::string> Fields;
  SelectorMode Mode = SelectorMode::Value;
};

struct ValueSpec {
  ValueKind Kind = ValueKind::Unknown;
  int64_t Integer = 0;
  bool Boolean = false;
  Selector Select;
};

struct Condition {
  ConditionKind Kind = ConditionKind::Value;
  Selector LHS;
  CompareOp Op = CompareOp::EQ;
  ValueSpec RHS;
  std::string Protocol;
  Selector Resource;
  std::vector<std::string> States;
  int64_t Count = 0;
};

struct Effect {
  EffectKind Kind = EffectKind::Bind;
  Selector Target;
  Selector Source;
  ValueSpec Value;
  std::string Protocol;
  std::vector<std::string> FromStates;
  std::string ToState;
  int64_t Count = 0;
  int64_t Delta = 0;
  AllocationFamily Family = AllocationFamily::Malloc;
  Selector Callback;
  std::vector<ValueSpec> CallbackArguments;
};

struct Requirement {
  std::string Protocol;
  Selector Resource;
  std::vector<std::string> AllowedStates;
  int64_t MinimumCount = 1;
};

struct Outcome {
  std::string Name;
  bool IsDefault = false;
  std::vector<Condition> When;
  std::vector<Effect> Effects;
};

struct MatchSpec {
  std::string QualifiedName;
  CallKind Kind = CallKind::Function;
  unsigned ParameterCount = 0;
  std::vector<std::string> ParameterTypes;
  std::string ReturnType;
  bool Variadic = false;
};

struct Model {
  std::string Id;
  MatchSpec Match;
  EvaluationKind Evaluation = EvaluationKind::Conservative;
  bool OverrideBody = false;
  std::vector<Requirement> Requires;
  std::vector<Outcome> Outcomes;
};

struct DiagnosticOptions {
  bool Leak = false;
  bool InvalidTransition = false;
  bool InvalidUse = false;
  bool CounterUnderflow = false;
};

struct Protocol {
  std::string Id;
  ProtocolKind Kind = ProtocolKind::Typestate;
  CounterMode CountMode = CounterMode::Lifetime;
  std::vector<std::string> States;
  std::vector<std::string> TerminalStates;
  unsigned MaxTrackedCount = 32;
  UnknownCallPolicy UnknownCall = UnknownCallPolicy::Escape;
  DiagnosticOptions Diagnostics;
};

struct Configuration {
  unsigned Version = 0;
  std::vector<Protocol> Protocols;
  std::vector<Model> Models;
};

} // namespace config_modeling

LLVM_YAML_IS_SEQUENCE_VECTOR(config_modeling::ValueSpec)
LLVM_YAML_IS_SEQUENCE_VECTOR(config_modeling::Condition)
LLVM_YAML_IS_SEQUENCE_VECTOR(config_modeling::Effect)
LLVM_YAML_IS_SEQUENCE_VECTOR(config_modeling::Requirement)
LLVM_YAML_IS_SEQUENCE_VECTOR(config_modeling::Outcome)
LLVM_YAML_IS_SEQUENCE_VECTOR(config_modeling::Model)
LLVM_YAML_IS_SEQUENCE_VECTOR(config_modeling::Protocol)

namespace llvm {
namespace yaml {

template <> struct ScalarEnumerationTraits<config_modeling::CallKind> {
  static void enumeration(IO &IO, config_modeling::CallKind &V) {
    IO.enumCase(V, "function", config_modeling::CallKind::Function);
    IO.enumCase(V, "static_method", config_modeling::CallKind::StaticMethod);
    IO.enumCase(V, "instance_method", config_modeling::CallKind::InstanceMethod);
    IO.enumCase(V, "constructor", config_modeling::CallKind::Constructor);
    IO.enumCase(V, "destructor", config_modeling::CallKind::Destructor);
  }
};
template <> struct ScalarEnumerationTraits<config_modeling::EvaluationKind> {
  static void enumeration(IO &IO, config_modeling::EvaluationKind &V) {
    IO.enumCase(V, "conservative", config_modeling::EvaluationKind::Conservative);
    IO.enumCase(V, "pure", config_modeling::EvaluationKind::Pure);
    IO.enumCase(V, "custom", config_modeling::EvaluationKind::Custom);
  }
};
template <> struct ScalarEnumerationTraits<config_modeling::SelectorBase> {
  static void enumeration(IO &IO, config_modeling::SelectorBase &V) {
    IO.enumCase(V, "return", config_modeling::SelectorBase::Return);
    IO.enumCase(V, "this", config_modeling::SelectorBase::This);
    IO.enumCase(V, "argument", config_modeling::SelectorBase::Argument);
  }
};
template <> struct ScalarEnumerationTraits<config_modeling::SelectorMode> {
  static void enumeration(IO &IO, config_modeling::SelectorMode &V) {
    IO.enumCase(V, "value", config_modeling::SelectorMode::Value);
    IO.enumCase(V, "region", config_modeling::SelectorMode::Region);
  }
};
template <> struct ScalarEnumerationTraits<config_modeling::CompareOp> {
  static void enumeration(IO &IO, config_modeling::CompareOp &V) {
    IO.enumCase(V, "eq", config_modeling::CompareOp::EQ);
    IO.enumCase(V, "ne", config_modeling::CompareOp::NE);
    IO.enumCase(V, "lt", config_modeling::CompareOp::LT);
    IO.enumCase(V, "le", config_modeling::CompareOp::LE);
    IO.enumCase(V, "gt", config_modeling::CompareOp::GT);
    IO.enumCase(V, "ge", config_modeling::CompareOp::GE);
  }
};
template <> struct ScalarEnumerationTraits<config_modeling::ConditionKind> {
  static void enumeration(IO &IO, config_modeling::ConditionKind &V) {
    IO.enumCase(V, "value", config_modeling::ConditionKind::Value);
    IO.enumCase(V, "choice", config_modeling::ConditionKind::Choice);
    IO.enumCase(V, "protocol_state",
                config_modeling::ConditionKind::ProtocolState);
    IO.enumCase(V, "counter", config_modeling::ConditionKind::Counter);
  }
};
template <> struct ScalarEnumerationTraits<config_modeling::ValueKind> {
  static void enumeration(IO &IO, config_modeling::ValueKind &V) {
    IO.enumCase(V, "integer", config_modeling::ValueKind::Integer);
    IO.enumCase(V, "boolean", config_modeling::ValueKind::Boolean);
    IO.enumCase(V, "null", config_modeling::ValueKind::Null);
    IO.enumCase(V, "selector", config_modeling::ValueKind::Selector);
    IO.enumCase(V, "conjured", config_modeling::ValueKind::Conjured);
    IO.enumCase(V, "unknown", config_modeling::ValueKind::Unknown);
  }
};
template <> struct ScalarEnumerationTraits<config_modeling::EffectKind> {
  static void enumeration(IO &IO, config_modeling::EffectKind &V) {
    IO.enumCase(V, "bind", config_modeling::EffectKind::Bind);
    IO.enumCase(V, "invalidate", config_modeling::EffectKind::Invalidate);
    IO.enumCase(V, "ownership_acquire", config_modeling::EffectKind::OwnershipAcquire);
    IO.enumCase(V, "ownership_release", config_modeling::EffectKind::OwnershipRelease);
    IO.enumCase(V, "ownership_transfer", config_modeling::EffectKind::OwnershipTransfer);
    IO.enumCase(V, "ownership_escape", config_modeling::EffectKind::OwnershipEscape);
    IO.enumCase(V, "protocol_set", config_modeling::EffectKind::ProtocolSet);
    IO.enumCase(V, "protocol_transition", config_modeling::EffectKind::ProtocolTransition);
    IO.enumCase(V, "protocol_alias", config_modeling::EffectKind::ProtocolAlias);
    IO.enumCase(V, "protocol_escape", config_modeling::EffectKind::ProtocolEscape);
    IO.enumCase(V, "counter_set", config_modeling::EffectKind::CounterSet);
    IO.enumCase(V, "counter_adjust", config_modeling::EffectKind::CounterAdjust);
    IO.enumCase(V, "invoke_callback",
                config_modeling::EffectKind::InvokeCallback);
    IO.enumCase(V, "terminate", config_modeling::EffectKind::Terminate);
  }
};
template <> struct ScalarEnumerationTraits<config_modeling::ProtocolKind> {
  static void enumeration(IO &IO, config_modeling::ProtocolKind &V) {
    IO.enumCase(V, "typestate", config_modeling::ProtocolKind::Typestate);
    IO.enumCase(V, "counter", config_modeling::ProtocolKind::Counter);
  }
};
template <> struct ScalarEnumerationTraits<config_modeling::CounterMode> {
  static void enumeration(IO &IO, config_modeling::CounterMode &V) {
    IO.enumCase(V, "lifetime", config_modeling::CounterMode::Lifetime);
    IO.enumCase(V, "balance", config_modeling::CounterMode::Balance);
  }
};
template <> struct ScalarEnumerationTraits<config_modeling::UnknownCallPolicy> {
  static void enumeration(IO &IO, config_modeling::UnknownCallPolicy &V) {
    IO.enumCase(V, "escape", config_modeling::UnknownCallPolicy::Escape);
    IO.enumCase(V, "preserve", config_modeling::UnknownCallPolicy::Preserve);
  }
};
template <> struct ScalarEnumerationTraits<config_modeling::AllocationFamily> {
  static void enumeration(IO &IO, config_modeling::AllocationFamily &V) {
    IO.enumCase(V, "malloc", config_modeling::AllocationFamily::Malloc);
    IO.enumCase(V, "new", config_modeling::AllocationFamily::CXXNew);
    IO.enumCase(V, "new_array", config_modeling::AllocationFamily::CXXNewArray);
    IO.enumCase(V, "alloca", config_modeling::AllocationFamily::Alloca);
  }
};

template <> struct MappingTraits<config_modeling::Selector> {
  static void mapping(IO &IO, config_modeling::Selector &S) {
    IO.mapRequired("base", S.Base);
    if (S.Base == config_modeling::SelectorBase::Argument)
      IO.mapOptional("index", S.Index, 0U);
    IO.mapOptional("dereference", S.Dereference, 0U);
    IO.mapOptional("fields", S.Fields);
    IO.mapOptional("mode", S.Mode, config_modeling::SelectorMode::Value);
  }
};
template <> struct MappingTraits<config_modeling::ValueSpec> {
  static void mapping(IO &IO, config_modeling::ValueSpec &V) {
    IO.mapRequired("kind", V.Kind);
    switch (V.Kind) {
    case config_modeling::ValueKind::Integer:
      IO.mapRequired("integer", V.Integer);
      break;
    case config_modeling::ValueKind::Boolean:
      IO.mapRequired("boolean", V.Boolean);
      break;
    case config_modeling::ValueKind::Selector:
      IO.mapRequired("selector", V.Select);
      break;
    case config_modeling::ValueKind::Null:
    case config_modeling::ValueKind::Conjured:
    case config_modeling::ValueKind::Unknown:
      break;
    }
  }
};
template <> struct MappingTraits<config_modeling::Condition> {
  static void mapping(IO &IO, config_modeling::Condition &C) {
    IO.mapOptional("kind", C.Kind, config_modeling::ConditionKind::Value);
    switch (C.Kind) {
    case config_modeling::ConditionKind::Value:
      IO.mapRequired("lhs", C.LHS);
      IO.mapRequired("op", C.Op);
      IO.mapRequired("rhs", C.RHS);
      break;
    case config_modeling::ConditionKind::Choice:
      break;
    case config_modeling::ConditionKind::ProtocolState:
      IO.mapRequired("protocol", C.Protocol);
      IO.mapRequired("resource", C.Resource);
      IO.mapRequired("states", C.States);
      break;
    case config_modeling::ConditionKind::Counter:
      IO.mapRequired("protocol", C.Protocol);
      IO.mapRequired("resource", C.Resource);
      IO.mapRequired("op", C.Op);
      IO.mapRequired("count", C.Count);
      break;
    }
  }
};
template <> struct MappingTraits<config_modeling::Effect> {
  static void mapping(IO &IO, config_modeling::Effect &E) {
    IO.mapRequired("kind", E.Kind);
    switch (E.Kind) {
    case config_modeling::EffectKind::Bind:
      IO.mapRequired("target", E.Target);
      IO.mapRequired("value", E.Value);
      break;
    case config_modeling::EffectKind::Invalidate:
    case config_modeling::EffectKind::OwnershipRelease:
    case config_modeling::EffectKind::OwnershipTransfer:
    case config_modeling::EffectKind::OwnershipEscape:
      IO.mapRequired("target", E.Target);
      break;
    case config_modeling::EffectKind::OwnershipAcquire:
      IO.mapRequired("target", E.Target);
      IO.mapOptional("family", E.Family,
                     config_modeling::AllocationFamily::Malloc);
      break;
    case config_modeling::EffectKind::ProtocolSet:
      IO.mapRequired("protocol", E.Protocol);
      IO.mapRequired("target", E.Target);
      IO.mapRequired("to", E.ToState);
      break;
    case config_modeling::EffectKind::ProtocolTransition:
      IO.mapRequired("protocol", E.Protocol);
      IO.mapRequired("target", E.Target);
      IO.mapOptional("from", E.FromStates);
      IO.mapRequired("to", E.ToState);
      break;
    case config_modeling::EffectKind::ProtocolAlias:
      IO.mapRequired("protocol", E.Protocol);
      IO.mapRequired("target", E.Target);
      IO.mapRequired("source", E.Source);
      break;
    case config_modeling::EffectKind::ProtocolEscape:
      IO.mapRequired("protocol", E.Protocol);
      IO.mapRequired("target", E.Target);
      break;
    case config_modeling::EffectKind::CounterSet:
      IO.mapRequired("protocol", E.Protocol);
      IO.mapRequired("target", E.Target);
      IO.mapRequired("count", E.Count);
      break;
    case config_modeling::EffectKind::CounterAdjust:
      IO.mapRequired("protocol", E.Protocol);
      IO.mapRequired("target", E.Target);
      IO.mapRequired("delta", E.Delta);
      break;
    case config_modeling::EffectKind::InvokeCallback:
      IO.mapRequired("callback", E.Callback);
      IO.mapOptional("arguments", E.CallbackArguments);
      break;
    case config_modeling::EffectKind::Terminate:
      break;
    }
  }
};
template <> struct MappingTraits<config_modeling::Requirement> {
  static void mapping(IO &IO, config_modeling::Requirement &R) {
    IO.mapRequired("protocol", R.Protocol);
    IO.mapRequired("resource", R.Resource);
    IO.mapOptional("allowed_states", R.AllowedStates);
    IO.mapOptional("minimum_count", R.MinimumCount, int64_t(1));
  }
};
template <> struct MappingTraits<config_modeling::Outcome> {
  static void mapping(IO &IO, config_modeling::Outcome &O) {
    IO.mapRequired("name", O.Name);
    IO.mapOptional("default", O.IsDefault, false);
    if (!O.IsDefault)
      IO.mapRequired("when", O.When);
    IO.mapOptional("effects", O.Effects);
  }
};
template <> struct MappingTraits<config_modeling::MatchSpec> {
  static void mapping(IO &IO, config_modeling::MatchSpec &M) {
    IO.mapRequired("qualified_name", M.QualifiedName);
    IO.mapRequired("kind", M.Kind);
    IO.mapRequired("parameter_count", M.ParameterCount);
    IO.mapOptional("parameter_types", M.ParameterTypes);
    IO.mapOptional("return_type", M.ReturnType);
    IO.mapOptional("variadic", M.Variadic, false);
  }
};
template <> struct MappingTraits<config_modeling::Model> {
  static void mapping(IO &IO, config_modeling::Model &M) {
    IO.mapRequired("id", M.Id);
    IO.mapRequired("match", M.Match);
    IO.mapOptional("evaluation", M.Evaluation,
                   config_modeling::EvaluationKind::Conservative);
    IO.mapOptional("override_body", M.OverrideBody, false);
    IO.mapOptional("requires", M.Requires);
    IO.mapRequired("outcomes", M.Outcomes);
  }
};
template <> struct MappingTraits<config_modeling::DiagnosticOptions> {
  static void mapping(IO &IO, config_modeling::DiagnosticOptions &D) {
    IO.mapOptional("leak", D.Leak, false);
    IO.mapOptional("invalid_transition", D.InvalidTransition, false);
    IO.mapOptional("invalid_use", D.InvalidUse, false);
    IO.mapOptional("counter_underflow", D.CounterUnderflow, false);
  }
};
template <> struct MappingTraits<config_modeling::Protocol> {
  static void mapping(IO &IO, config_modeling::Protocol &P) {
    IO.mapRequired("id", P.Id);
    IO.mapRequired("kind", P.Kind);
    if (P.Kind == config_modeling::ProtocolKind::Typestate) {
      IO.mapRequired("states", P.States);
      IO.mapOptional("terminal_states", P.TerminalStates);
    } else {
      IO.mapOptional("counter_mode", P.CountMode,
                     config_modeling::CounterMode::Lifetime);
      IO.mapOptional("max_tracked_count", P.MaxTrackedCount, 32U);
    }
    IO.mapOptional("unknown_call", P.UnknownCall,
                   config_modeling::UnknownCallPolicy::Escape);
    IO.mapOptional("diagnostics", P.Diagnostics);
  }
};
template <> struct MappingTraits<config_modeling::Configuration> {
  static void mapping(IO &IO, config_modeling::Configuration &C) {
    IO.mapRequired("version", C.Version);
    IO.mapRequired("protocols", C.Protocols);
    IO.mapRequired("models", C.Models);
  }
};

} // namespace yaml
} // namespace llvm

namespace {

using namespace config_modeling;

struct ResourceKey {
  unsigned Protocol = 0;
  SymbolRef Symbol = nullptr;
  const MemRegion *Region = nullptr;
  const StackFrameContext *Frame = nullptr;

  bool operator==(const ResourceKey &O) const {
    return Protocol == O.Protocol && Symbol == O.Symbol && Region == O.Region &&
           Frame == O.Frame;
  }
  bool operator!=(const ResourceKey &O) const { return !(*this == O); }
  bool operator<(const ResourceKey &O) const {
    if (Protocol != O.Protocol)
      return Protocol < O.Protocol;
    std::less<const void *> Less;
    if (Symbol != O.Symbol)
      return Less(Symbol, O.Symbol);
    if (Region != O.Region)
      return Less(Region, O.Region);
    return Less(Frame, O.Frame);
  }
  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddInteger(Protocol);
    ID.AddPointer(Symbol);
    ID.AddPointer(Region);
    ID.AddPointer(Frame);
  }
};

struct TrackedResource {
  unsigned State = 0;
  int64_t Count = 0;
  bool CountUnknown = false;
  const Expr *Origin = nullptr;

  bool operator==(const TrackedResource &O) const {
    return State == O.State && Count == O.Count &&
           CountUnknown == O.CountUnknown && Origin == O.Origin;
  }
  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddInteger(State);
    ID.AddInteger(static_cast<uint64_t>(Count));
    ID.AddBoolean(CountUnknown);
    ID.AddPointer(Origin);
  }
};

} // end anonymous namespace

REGISTER_MAP_WITH_PROGRAMSTATE(ConfigProtocolState, ResourceKey,
                               TrackedResource)
REGISTER_MAP_WITH_PROGRAMSTATE(ConfigProtocolAlias, ResourceKey, ResourceKey)

namespace {

class ConfiguredProtocolBugVisitor final : public BugReporterVisitor {
  ResourceKey Key;
  Protocol ProtocolInfo;

  StringRef stateName(unsigned State) const {
    if (State < ProtocolInfo.States.size())
      return ProtocolInfo.States[State];
    return "<unknown>";
  }

public:
  ConfiguredProtocolBugVisitor(const ResourceKey &Key, const Protocol &P)
      : Key(Key), ProtocolInfo(P) {}

  void Profile(llvm::FoldingSetNodeID &ID) const override {
    static int Tag = 0;
    ID.AddPointer(&Tag);
    Key.Profile(ID);
    ID.AddString(ProtocolInfo.Id);
  }

  PathDiagnosticPieceRef VisitNode(const ExplodedNode *N,
                                   BugReporterContext &BRC,
                                   PathSensitiveBugReport &) override {
    if (N->pred_empty())
      return nullptr;

    const TrackedResource *Current =
        N->getState()->get<ConfigProtocolState>(Key);
    const TrackedResource *Previous =
        N->getFirstPred()->getState()->get<ConfigProtocolState>(Key);
    if (!Current)
      return nullptr;

    SmallString<160> Buffer;
    llvm::raw_svector_ostream Out(Buffer);
    bool Emit = false;

    if (ProtocolInfo.Kind == ProtocolKind::Typestate) {
      if (!Previous) {
        Out << "Configured protocol '" << ProtocolInfo.Id
            << "' enters state '" << stateName(Current->State) << "'";
        Emit = true;
      } else if (Current->State != Previous->State) {
        Out << "Configured protocol '" << ProtocolInfo.Id
            << "' transitions from '" << stateName(Previous->State)
            << "' to '" << stateName(Current->State) << "'";
        Emit = true;
      }
    } else if (!Previous) {
      // A zero balance manufactured only to diagnose an immediate decrement
      // has no useful history of its own; leave that report as one final
      // event. Positive and unknown initial counters are real protocol state.
      if (Current->CountUnknown) {
        Out << "Counter for configured protocol '" << ProtocolInfo.Id
            << "' is initialized as unknown";
        Emit = true;
      } else if (Current->Count != 0) {
        Out << "Counter for configured protocol '" << ProtocolInfo.Id
            << "' is initialized to " << Current->Count;
        Emit = true;
      }
    } else if (Current->CountUnknown != Previous->CountUnknown) {
      Out << "Counter for configured protocol '" << ProtocolInfo.Id
          << "' becomes "
          << (Current->CountUnknown ? "unknown" : "known");
      Emit = true;
    } else if (!Current->CountUnknown &&
               Current->Count != Previous->Count) {
      Out << "Counter for configured protocol '" << ProtocolInfo.Id
          << "' changes from " << Previous->Count << " to "
          << Current->Count;
      Emit = true;
    }

    if (!Emit)
      return nullptr;

    const Stmt *S = Current->Origin;
    if (!S)
      S = N->getStmtForDiagnostics();
    if (!S)
      return nullptr;
    PathDiagnosticLocation Location = PathDiagnosticLocation::createBegin(
        S, BRC.getSourceManager(), N->getLocationContext());
    return std::make_shared<PathDiagnosticEventPiece>(
        Location, Out.str(), true);
  }
};

struct ResolvedValue {
  SVal Value = UnknownVal();
  QualType Type;
  bool Valid = false;
};

struct EffectResult {
  ProgramStateRef State;
  ExplodedNode *Predecessor = nullptr;
  bool Terminated = false;
  bool CallbackSchedulingFailed = false;
};

class ConfigDrivenModelingChecker
    : public Checker<eval::Call, check::PreCall, check::PostCall,
                     check::LiveSymbols, check::DeadSymbols, check::EndFunction,
                     check::PointerEscape, check::PreStmt<ReturnStmt>> {
  mutable std::optional<Configuration> Config;
  mutable bool Initialized = false;
  mutable bool ConfigValid = false;
  mutable std::unique_ptr<BugType> LeakBug;
  mutable std::unique_ptr<BugType> TransitionBug;
  mutable std::unique_ptr<BugType> UseBug;
  mutable std::unique_ptr<BugType> UnderflowBug;
  mutable std::unique_ptr<BugType> DoubleReleaseBug;

public:
  void initialize(CheckerManager &Mgr) const;
  bool evalCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkDeadSymbols(SymbolReaper &SR, CheckerContext &C) const;
  void checkLiveSymbols(ProgramStateRef State, SymbolReaper &SR) const;
  void checkEndFunction(const ReturnStmt *, CheckerContext &C) const;
  ProgramStateRef checkPointerEscape(ProgramStateRef State,
                                     const InvalidatedSymbols &Escaped,
                                     const CallEvent *Call,
                                     PointerEscapeKind Kind) const;
  void checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const;

private:
  bool ensureInitialized(CheckerContext &C) const;
  bool ensureInitialized(CheckerManager &Mgr) const;
  bool validateConfiguration(std::string &Error) const;
  bool validateSelector(const config_modeling::Selector &S, const Model &M,
                        std::string &Error) const;
  int findProtocol(StringRef Id) const;
  int findState(const Protocol &P, StringRef Name) const;
  const Model *findModel(const CallEvent &Call) const;
  bool matches(const Model &M, const CallEvent &Call) const;
  ResolvedValue resolve(const config_modeling::Selector &S,
                        const CallEvent &Call,
                        ProgramStateRef State, CheckerContext &C) const;
  std::optional<ResourceKey> resourceKey(const config_modeling::Selector &S,
                                    unsigned ProtocolIndex,
                                    const CallEvent &Call,
                                    ProgramStateRef State,
                                    CheckerContext &C) const;
  ResourceKey canonicalKey(ProgramStateRef State, ResourceKey Key) const;
  ProgramStateRef applyCondition(ProgramStateRef State, const Condition &Cond,
                                 bool Assumption, const CallEvent &Call,
                                 CheckerContext &C) const;
  EffectResult applyEffects(ProgramStateRef State, ArrayRef<Effect> Effects,
                            const CallEvent &Call, CheckerContext &C) const;
  EffectResult applyEffect(EffectResult Result, const Effect &E,
                           const CallEvent &Call, CheckerContext &C) const;
  SVal makeValue(const ValueSpec &Spec, QualType TargetType,
                 const CallEvent &Call, ProgramStateRef State,
                 CheckerContext &C) const;
  void addSuppressEscapeTraits(const Model &M, const CallEvent &Call,
                               ProgramStateRef State, CheckerContext &C,
                               RegionAndSymbolInvalidationTraits &Traits) const;
  bool requirementSatisfied(const Requirement &R, const CallEvent &Call,
                            ProgramStateRef State, CheckerContext &C,
                            ResourceKey *Interesting) const;
  bool keyIsLive(const ResourceKey &Key, SymbolReaper &SR) const;
  bool hasLiveAlias(ProgramStateRef State, const ResourceKey &Canonical,
                    SymbolReaper &SR) const;
  bool isTerminal(const Protocol &P, const TrackedResource &R) const;
  ExplodedNode *report(CheckerContext &C, ProgramStateRef State,
                       const ResourceKey &Key, StringRef Message,
                       std::unique_ptr<BugType> &Type, StringRef TypeName,
                       ExplodedNode *Predecessor = nullptr,
                       const ProgramPointTag *Tag = nullptr,
                       bool AddProtocolVisitor = true,
                       bool AddAllocationVisitor = false) const;
};

static BinaryOperator::Opcode toBinaryOpcode(CompareOp Op) {
  switch (Op) {
  case CompareOp::EQ: return BO_EQ;
  case CompareOp::NE: return BO_NE;
  case CompareOp::LT: return BO_LT;
  case CompareOp::LE: return BO_LE;
  case CompareOp::GT: return BO_GT;
  case CompareOp::GE: return BO_GE;
  }
  llvm_unreachable("unknown comparison operator");
}

static bool compareCounts(int64_t LHS, CompareOp Op, int64_t RHS) {
  switch (Op) {
  case CompareOp::EQ: return LHS == RHS;
  case CompareOp::NE: return LHS != RHS;
  case CompareOp::LT: return LHS < RHS;
  case CompareOp::LE: return LHS <= RHS;
  case CompareOp::GT: return LHS > RHS;
  case CompareOp::GE: return LHS >= RHS;
  }
  llvm_unreachable("unknown comparison operator");
}

static allocation_state::AllocationFamilyKind
toAllocationFamily(AllocationFamily F) {
  switch (F) {
  case AllocationFamily::Malloc:
    return allocation_state::AllocationFamilyKind::Malloc;
  case AllocationFamily::CXXNew:
    return allocation_state::AllocationFamilyKind::CXXNew;
  case AllocationFamily::CXXNewArray:
    return allocation_state::AllocationFamilyKind::CXXNewArray;
  case AllocationFamily::Alloca:
    return allocation_state::AllocationFamilyKind::Alloca;
  }
  llvm_unreachable("unknown allocation family");
}

static const FunctionDecl *getFunctionDeclFromSVal(SVal V) {
  std::optional<loc::MemRegionVal> MRV = V.getAs<loc::MemRegionVal>();
  if (!MRV)
    return nullptr;
  const auto *FCR =
      dyn_cast_or_null<FunctionCodeRegion>(MRV->getRegion());
  return FCR ? dyn_cast_or_null<FunctionDecl>(FCR->getDecl()) : nullptr;
}

/// Return a definition that is safe to use as the callee of a configured
/// callback.  FunctionDecl::hasBody() only searches declarations already
/// present in the current AST.  A configured callback is an indirect,
/// synthetic call, so it never reaches AnyFunctionCall::getRuntimeDefinition(),
/// which is where ordinary calls request a CTU definition.  Perform that
/// request explicitly when the local declaration has no body.
static const FunctionDecl *
getConfiguredCallbackDefinition(const FunctionDecl *FD, ProgramStateRef State,
                                bool &IsForeign) {
  IsForeign = false;
  if (!FD || !State)
    return nullptr;

  ExprEngine &Engine = State->getStateManager().getOwningEngine();
  AnalyzerOptions &Opts = Engine.getAnalysisManager().options;
  cross_tu::CrossTranslationUnitContext *CTU =
      Engine.getCrossTranslationUnitContext();

  const FunctionDecl *BodyFD = nullptr;
  if (FD->hasBody(BodyFD)) {
    IsForeign = Opts.IsNaiveCTUEnabled && CTU &&
                CTU->isImportedAsNew(BodyFD);
    return BodyFD;
  }

  if (!Opts.IsNaiveCTUEnabled)
    return nullptr;

  if (!CTU)
    return nullptr;

  llvm::Expected<const FunctionDecl *> CTUDeclOrError =
      CTU->getCrossTUDefinition(FD, Opts.CTUDir, Opts.CTUIndexName,
                                Opts.DisplayCTUProgress);
  if (!CTUDeclOrError) {
    llvm::handleAllErrors(
        CTUDeclOrError.takeError(), [&](const cross_tu::IndexError &IE) {
          CTU->emitCrossTUDiagnostics(IE);
        });
    return nullptr;
  }

  const FunctionDecl *ImportedFD = *CTUDeclOrError;
  BodyFD = nullptr;
  if (!ImportedFD || !ImportedFD->hasBody(BodyFD))
    return nullptr;
  IsForeign = true;
  return BodyFD;
}

static const CallExpr *createSyntheticCallbackCall(
    ASTContext &Ctx, const FunctionDecl *FD, const Expr *Origin,
    SmallVectorImpl<Expr *> &ArgumentExprs) {
  if (!FD || !Origin)
    return nullptr;

  SourceLocation Loc = Origin->getExprLoc();
  auto *Callee = DeclRefExpr::Create(
      Ctx, NestedNameSpecifierLoc(), SourceLocation(),
      const_cast<FunctionDecl *>(FD),
      /*RefersToEnclosingVariableOrCapture=*/false, Loc, FD->getType(),
      VK_LValue);

  ArgumentExprs.clear();
  ArgumentExprs.reserve(FD->getNumParams());
  for (const ParmVarDecl *Parameter : FD->parameters()) {
    QualType Type = Parameter->getType();
    ExprValueKind ValueKind = VK_PRValue;
    if (Type->isLValueReferenceType()) {
      Type = Type->getPointeeType();
      ValueKind = VK_LValue;
    } else if (Type->isRValueReferenceType()) {
      Type = Type->getPointeeType();
      ValueKind = VK_XValue;
    }

    // EnvironmentEntry treats OpaqueValueExpr as transparent and recursively
    // follows its source expression.  A source-less OVE is therefore unsafe
    // when a StackHintGenerator later asks for the SVal of a synthetic
    // callback argument: the transparency walk reaches nullptr and
    // Expr::IgnoreParens() crashes while the plist path is being generated.
    //
    // The actual argument SVal is kept separately in Invocation::Arguments.
    // Use a synthetic DeclRefExpr as the durable, non-transparent Environment
    // key.  An ImplicitValueInitExpr cannot be used here: Environment::getSVal
    // always constant-folds it to zero before consulting ExprBindings, which
    // would turn every pointer callback argument into null.  DeclRefExpr takes
    // the normal lookup path, so BindExpr/getArgSVal recover the configured
    // SVal while the OVE still carries the required expression value kind.
    auto *Anchor = DeclRefExpr::Create(
        Ctx, NestedNameSpecifierLoc(), SourceLocation(),
        const_cast<ParmVarDecl *>(Parameter),
        /*RefersToEnclosingVariableOrCapture=*/false, Loc, Type, VK_LValue);
    ArgumentExprs.push_back(new (Ctx) OpaqueValueExpr(
        Loc, Type, ValueKind, OK_Ordinary, Anchor));
  }

  QualType ResultType = FD->getReturnType();
  ExprValueKind ResultValueKind = VK_PRValue;
  if (ResultType->isLValueReferenceType()) {
    ResultType = ResultType->getPointeeType();
    ResultValueKind = VK_LValue;
  } else if (ResultType->isRValueReferenceType()) {
    ResultType = ResultType->getPointeeType();
    ResultValueKind = VK_XValue;
  }

  return CallExpr::Create(Ctx, Callee, ArgumentExprs, ResultType,
                          ResultValueKind, Loc, FPOptionsOverride());
}

void ConfigDrivenModelingChecker::initialize(CheckerManager &Mgr) const {
  (void)ensureInitialized(Mgr);
}

bool ConfigDrivenModelingChecker::ensureInitialized(CheckerContext &C) const {
  CheckerManager *Mgr = C.getAnalysisManager().getCheckerManager();
  assert(Mgr);
  return ensureInitialized(*Mgr);
}

bool ConfigDrivenModelingChecker::ensureInitialized(CheckerManager &Mgr) const {
  if (Initialized)
    return ConfigValid;
  Initialized = true;

  StringRef Option = "Config";
  StringRef Path =
      Mgr.getAnalyzerOptions().getCheckerStringOption(this, Option);
  if (Path.trim().empty()) {
    Mgr.reportInvalidCheckerOptionValue(
        this, Option, "a non-empty YAML configuration filename");
    return false;
  }

  std::optional<Configuration> Parsed =
      getConfiguration<Configuration>(Mgr, this, Option, Path);
  if (!Parsed)
    return false;
  Config = std::move(*Parsed);

  std::string Error;
  if (!validateConfiguration(Error)) {
    Mgr.reportInvalidCheckerOptionValue(
        this, Option, (Twine("a valid version 1 model configuration (") +
                       Error + ")").str());
    Config.reset();
    return false;
  }
  ConfigValid = true;
  return true;
}

int ConfigDrivenModelingChecker::findProtocol(StringRef Id) const {
  if (!Config)
    return -1;
  for (unsigned I = 0; I < Config->Protocols.size(); ++I)
    if (Config->Protocols[I].Id == Id)
      return static_cast<int>(I);
  return -1;
}

int ConfigDrivenModelingChecker::findState(const Protocol &P,
                                            StringRef Name) const {
  for (unsigned I = 0; I < P.States.size(); ++I)
    if (P.States[I] == Name)
      return static_cast<int>(I);
  return -1;
}

bool ConfigDrivenModelingChecker::validateSelector(
    const config_modeling::Selector &S, const Model &M,
    std::string &Error) const {
  if (S.Base != SelectorBase::Argument && S.Index != 0) {
    Error = (Twine("model '") + M.Id +
             "' sets index on a non-argument selector").str();
    return false;
  }
  if (S.Base == SelectorBase::Argument &&
      S.Index >= M.Match.ParameterCount) {
    Error = (Twine("model '") + M.Id + "' uses out-of-range argument " +
             Twine(S.Index)).str();
    return false;
  }
  if (S.Base == SelectorBase::Return &&
      (M.Match.Kind == CallKind::Constructor ||
       M.Match.Kind == CallKind::Destructor)) {
    Error = (Twine("model '") + M.Id +
             "' selects the nonexistent constructor/destructor return value")
                .str();
    return false;
  }
  if (S.Base == SelectorBase::This &&
      M.Match.Kind != CallKind::InstanceMethod &&
      M.Match.Kind != CallKind::Constructor &&
      M.Match.Kind != CallKind::Destructor) {
    Error = (Twine("model '") + M.Id +
             "' uses this for a non-instance call").str();
    return false;
  }
  for (const std::string &Field : S.Fields)
    if (Field.empty()) {
      Error = (Twine("model '") + M.Id +
               "' has an empty field path component").str();
      return false;
    }
  return true;
}

bool ConfigDrivenModelingChecker::validateConfiguration(
    std::string &Error) const {
  if (!Config || Config->Version != 1) {
    Error = "version must be exactly 1";
    return false;
  }

  llvm::StringSet<> ProtocolIds;
  for (const Protocol &P : Config->Protocols) {
    if (P.Id.empty() || !ProtocolIds.insert(P.Id).second) {
      Error = "protocol ids must be non-empty and unique";
      return false;
    }
    if (P.MaxTrackedCount == 0) {
      Error = (Twine("protocol '") + P.Id +
               "' has zero max_tracked_count").str();
      return false;
    }
    if (P.Kind == ProtocolKind::Typestate) {
      if (P.States.empty()) {
        Error = (Twine("typestate protocol '") + P.Id +
                 "' has no states").str();
        return false;
      }
      llvm::StringSet<> States;
      for (const std::string &S : P.States)
        if (S.empty() || !States.insert(S).second) {
          Error = (Twine("protocol '") + P.Id +
                   "' has duplicate/empty states").str();
          return false;
        }
      for (const std::string &S : P.TerminalStates)
        if (!States.count(S)) {
          Error = (Twine("protocol '") + P.Id +
                   "' names an unknown terminal state '" + S + "'").str();
          return false;
        }
    } else if (!P.States.empty() || !P.TerminalStates.empty()) {
      Error = (Twine("counter protocol '") + P.Id +
               "' must not declare typestates").str();
      return false;
    }
  }

  llvm::StringSet<> ModelIds;
  llvm::StringSet<> MatcherBases;
  llvm::StringSet<> UntypedMatchers;
  llvm::StringSet<> FullMatchers;
  for (const Model &M : Config->Models) {
    if (M.Id.empty() || !ModelIds.insert(M.Id).second) {
      Error = "model ids must be non-empty and unique";
      return false;
    }
    if (M.Match.QualifiedName.empty()) {
      Error = (Twine("model '") + M.Id + "' has an empty qualified_name").str();
      return false;
    }
    if (!M.Match.ParameterTypes.empty() &&
        M.Match.ParameterTypes.size() != M.Match.ParameterCount) {
      Error = (Twine("model '") + M.Id +
               "' parameter_types size does not match parameter_count").str();
      return false;
    }
    std::string MatcherKey =
        (Twine(M.Match.QualifiedName) + "#" +
         Twine(static_cast<unsigned>(M.Match.Kind)) + "#" +
         Twine(M.Match.ParameterCount)).str();
    if (M.Match.ParameterTypes.empty() && MatcherBases.count(MatcherKey)) {
      Error = (Twine("model '") + M.Id + "' conflicts with another matcher").str();
      return false;
    }
    if (!M.Match.ParameterTypes.empty() && UntypedMatchers.count(MatcherKey)) {
      Error = (Twine("model '") + M.Id + "' overlaps an untyped matcher").str();
      return false;
    }
    std::string FullKey = MatcherKey;
    for (const std::string &T : M.Match.ParameterTypes)
      FullKey += "#" + T;
    FullKey += "->" + M.Match.ReturnType;
    if (!FullMatchers.insert(FullKey).second) {
      Error = (Twine("model '") + M.Id + "' duplicates another matcher").str();
      return false;
    }
    MatcherBases.insert(MatcherKey);
    if (M.Match.ParameterTypes.empty())
      UntypedMatchers.insert(MatcherKey);
    if (M.Outcomes.empty() || !M.Outcomes.back().IsDefault) {
      Error = (Twine("model '") + M.Id +
               "' must end with a default outcome").str();
      return false;
    }
    llvm::StringSet<> OutcomeNames;
    for (unsigned OI = 0; OI < M.Outcomes.size(); ++OI) {
      const Outcome &O = M.Outcomes[OI];
      if (O.Name.empty() || !OutcomeNames.insert(O.Name).second) {
        Error = (Twine("model '") + M.Id +
                 "' has duplicate/empty outcome names").str();
        return false;
      }
      if (O.IsDefault && OI + 1 != M.Outcomes.size()) {
        Error = (Twine("model '") + M.Id +
                 "' has a non-final default outcome").str();
        return false;
      }
      if (O.IsDefault && !O.When.empty()) {
        Error = (Twine("model '") + M.Id +
                 "' default outcome must not contain conditions").str();
        return false;
      }
      if (!O.IsDefault && O.When.empty()) {
        Error = (Twine("model '") + M.Id +
                 "' has an unconditional non-default outcome").str();
        return false;
      }
      for (const Condition &C : O.When) {
        switch (C.Kind) {
        case ConditionKind::Value:
          if (!validateSelector(C.LHS, M, Error))
            return false;
          if (C.RHS.Kind == ValueKind::Selector &&
              !validateSelector(C.RHS.Select, M, Error))
            return false;
          if (C.RHS.Kind != ValueKind::Integer &&
              C.RHS.Kind != ValueKind::Boolean &&
              C.RHS.Kind != ValueKind::Null &&
              C.RHS.Kind != ValueKind::Selector) {
            Error = (Twine("model '") + M.Id +
                     "' uses an invalid value condition RHS").str();
            return false;
          }
          break;
        case ConditionKind::Choice:
          if (O.When.size() != 1) {
            Error = (Twine("model '") + M.Id +
                     "' must use choice as the outcome's only condition").str();
            return false;
          }
          break;
        case ConditionKind::ProtocolState: {
          if (!validateSelector(C.Resource, M, Error))
            return false;
          int PI = findProtocol(C.Protocol);
          if (PI < 0 ||
              Config->Protocols[PI].Kind != ProtocolKind::Typestate) {
            Error = (Twine("model '") + M.Id +
                     "' uses protocol_state with an unknown/non-typestate "
                     "protocol '" + C.Protocol + "'").str();
            return false;
          }
          if (C.States.empty()) {
            Error = (Twine("model '") + M.Id +
                     "' has a protocol_state condition with no states").str();
            return false;
          }
          const Protocol &P = Config->Protocols[PI];
          llvm::StringSet<> ConditionStates;
          for (const std::string &S : C.States) {
            if (!ConditionStates.insert(S).second) {
              Error = (Twine("model '") + M.Id +
                       "' repeats condition state '" + S + "'").str();
              return false;
            }
            if (findState(P, S) < 0) {
              Error = (Twine("model '") + M.Id +
                       "' uses unknown condition state '" + S + "'").str();
              return false;
            }
          }
          break;
        }
        case ConditionKind::Counter: {
          if (!validateSelector(C.Resource, M, Error))
            return false;
          int PI = findProtocol(C.Protocol);
          if (PI < 0 || Config->Protocols[PI].Kind != ProtocolKind::Counter) {
            Error = (Twine("model '") + M.Id +
                     "' uses counter condition with an unknown/non-counter "
                     "protocol '" + C.Protocol + "'").str();
            return false;
          }
          if (C.Count < 0) {
            Error = (Twine("model '") + M.Id +
                     "' compares a counter with a negative value").str();
            return false;
          }
          break;
        }
        }
      }
      unsigned CallbackCount = 0;
      bool SeenCallback = false;
      bool HasTerminate = false;
      for (unsigned EI = 0; EI < O.Effects.size(); ++EI) {
        const Effect &E = O.Effects[EI];
        if (SeenCallback && E.Kind != EffectKind::InvokeCallback &&
            E.Kind != EffectKind::Terminate) {
          Error = (Twine("model '") + M.Id + "' outcome '" + O.Name +
                   "' must place callback invocations after all other "
                   "effects").str();
          return false;
        }
        SeenCallback |= E.Kind == EffectKind::InvokeCallback;
        if (E.Kind == EffectKind::Terminate &&
            EI + 1 != O.Effects.size()) {
          Error = (Twine("model '") + M.Id +
                   "' must place terminate last in an outcome").str();
          return false;
        }
        HasTerminate |= E.Kind == EffectKind::Terminate;
        if (E.Kind != EffectKind::Terminate &&
            E.Kind != EffectKind::InvokeCallback &&
            !validateSelector(E.Target, M, Error))
          return false;
        if ((E.Kind == EffectKind::ProtocolAlias ||
             (E.Kind == EffectKind::Bind &&
              E.Value.Kind == ValueKind::Selector)) &&
            !validateSelector(E.Kind == EffectKind::ProtocolAlias
                                  ? E.Source : E.Value.Select,
                              M, Error))
          return false;
        switch (E.Kind) {
        case EffectKind::InvokeCallback:
          ++CallbackCount;
          if (CallbackCount > configured_callback::MaxCallbacks) {
            Error = (Twine("model '") + M.Id + "' outcome '" + O.Name +
                     "' invokes more than " +
                     Twine(configured_callback::MaxCallbacks) +
                     " callbacks").str();
            return false;
          }
          if (!validateSelector(E.Callback, M, Error))
            return false;
          if (E.Callback.Mode != SelectorMode::Value) {
            Error = (Twine("model '") + M.Id +
                     "' uses a region selector as a callback").str();
            return false;
          }
          if (E.CallbackArguments.size() >
              configured_callback::MaxArguments) {
            Error = (Twine("model '") + M.Id +
                     "' has more than " +
                     Twine(configured_callback::MaxArguments) +
                     " callback arguments").str();
            return false;
          }
          for (const ValueSpec &Argument : E.CallbackArguments)
            if (Argument.Kind == ValueKind::Selector &&
                !validateSelector(Argument.Select, M, Error))
              return false;
          break;
        case EffectKind::ProtocolSet:
        case EffectKind::ProtocolTransition:
        case EffectKind::ProtocolAlias:
        case EffectKind::ProtocolEscape:
        case EffectKind::CounterSet:
        case EffectKind::CounterAdjust: {
          int PI = findProtocol(E.Protocol);
          if (PI < 0) {
            Error = (Twine("model '") + M.Id + "' references protocol '" +
                     E.Protocol + "'").str();
            return false;
          }
          const Protocol &P = Config->Protocols[PI];
          if ((E.Kind == EffectKind::ProtocolSet ||
               E.Kind == EffectKind::ProtocolTransition) &&
              (P.Kind != ProtocolKind::Typestate ||
               findState(P, E.ToState) < 0)) {
            Error = (Twine("model '") + M.Id +
                     "' uses an unknown typestate target").str();
            return false;
          }
          for (const std::string &S : E.FromStates)
            if (findState(P, S) < 0) {
              Error = (Twine("model '") + M.Id +
                       "' uses an unknown source state '" + S + "'").str();
              return false;
            }
          if ((E.Kind == EffectKind::CounterSet ||
               E.Kind == EffectKind::CounterAdjust) &&
              P.Kind != ProtocolKind::Counter) {
            Error = (Twine("model '") + M.Id +
                     "' applies a counter effect to typestate").str();
            return false;
          }
          if (E.Kind == EffectKind::CounterSet && E.Count < 0) {
            Error = (Twine("model '") + M.Id +
                     "' initializes a counter to a negative value").str();
            return false;
          }
          if (E.Kind == EffectKind::CounterAdjust &&
              E.Delta == std::numeric_limits<int64_t>::min()) {
            Error = (Twine("model '") + M.Id +
                     "' uses an unrepresentable counter decrement").str();
            return false;
          }
          break;
        }
        default:
          break;
        }
      }
      if (HasTerminate && CallbackCount != 0) {
        Error = (Twine("model '") + M.Id + "' outcome '" + O.Name +
                 "' combines callback invocation with terminate").str();
        return false;
      }
    }
    for (const Requirement &R : M.Requires) {
      if (!validateSelector(R.Resource, M, Error))
        return false;
      int PI = findProtocol(R.Protocol);
      if (PI < 0) {
        Error = (Twine("model '") + M.Id + "' requires protocol '" +
                 R.Protocol + "'").str();
        return false;
      }
      const Protocol &P = Config->Protocols[PI];
      if (P.Kind == ProtocolKind::Counter && R.MinimumCount < 0) {
        Error = (Twine("model '") + M.Id +
                 "' requires a negative minimum counter value").str();
        return false;
      }
      for (const std::string &S : R.AllowedStates)
        if (findState(P, S) < 0) {
          Error = (Twine("model '") + M.Id +
                   "' requires unknown state '" + S + "'").str();
          return false;
        }
    }
  }
  return true;
}

static CallKind getCallKind(const FunctionDecl *FD) {
  if (isa<CXXConstructorDecl>(FD))
    return CallKind::Constructor;
  if (isa<CXXDestructorDecl>(FD))
    return CallKind::Destructor;
  if (const auto *MD = dyn_cast<CXXMethodDecl>(FD))
    return MD->isStatic() ? CallKind::StaticMethod : CallKind::InstanceMethod;
  return CallKind::Function;
}

bool ConfigDrivenModelingChecker::matches(const Model &M,
                                          const CallEvent &Call) const {
  const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!FD || FD->getQualifiedNameAsString() != M.Match.QualifiedName ||
      getCallKind(FD) != M.Match.Kind ||
      FD->getNumParams() != M.Match.ParameterCount ||
      FD->isVariadic() != M.Match.Variadic)
    return false;

  if (!M.Match.ParameterTypes.empty())
    for (unsigned I = 0; I < FD->getNumParams(); ++I)
      if (FD->getParamDecl(I)
              ->getType()
              .getCanonicalType()
              .getAsString(FD->getASTContext().getPrintingPolicy()) !=
          M.Match.ParameterTypes[I])
        return false;
  if (!M.Match.ReturnType.empty() &&
      FD->getReturnType().getCanonicalType().getAsString(
          FD->getASTContext().getPrintingPolicy()) !=
          M.Match.ReturnType)
    return false;
  return true;
}

const Model *ConfigDrivenModelingChecker::findModel(
    const CallEvent &Call) const {
  if (!ConfigValid || !Config)
    return nullptr;
  for (const Model &M : Config->Models)
    if (matches(M, Call))
      return &M;
  return nullptr;
}

ResolvedValue ConfigDrivenModelingChecker::resolve(
    const config_modeling::Selector &S, const CallEvent &Call,
    ProgramStateRef State, CheckerContext &C) const {
  ResolvedValue R;
  bool IsLocation = false;
  switch (S.Base) {
  case SelectorBase::Return:
    if (!Call.getOriginExpr())
      return R;
    R.Type = Call.getResultType();
    if (!R.Type.isNull() && R.Type->isRecordType() &&
        S.Mode == SelectorMode::Region) {
      std::optional<SVal> ReturnObject = Call.getReturnValueUnderConstruction();
      if (!ReturnObject || !ReturnObject->getAsRegion())
        return R;
      R.Value = *ReturnObject;
      IsLocation = true;
    } else {
      R.Value = State->getSVal(Call.getOriginExpr(), C.getLocationContext());
    }
    break;
  case SelectorBase::Argument: {
    if (S.Index >= Call.getNumArgs())
      return R;
    R.Value = Call.getArgSVal(S.Index);
    const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
    if (FD && S.Index < FD->getNumParams())
      R.Type = FD->getParamDecl(S.Index)->getType();
    else if (const Expr *E = Call.getArgExpr(S.Index))
      R.Type = E->getType();
    break;
  }
  case SelectorBase::This:
    if (const auto *IC = dyn_cast<CXXInstanceCall>(&Call))
      R.Value = IC->getCXXThisVal();
    else if (const auto *CC = dyn_cast<AnyCXXConstructorCall>(&Call))
      R.Value = CC->getCXXThisVal();
    else
      return R;
    if (const auto *MD = dyn_cast_or_null<CXXMethodDecl>(Call.getDecl()))
      R.Type = C.getASTContext().getPointerType(
          C.getASTContext().getRecordType(MD->getParent()));
    break;
  }
  if (R.Value.isUnknownOrUndef() || R.Type.isNull())
    return R;

  for (unsigned I = 0; I < S.Dereference; ++I) {
    if (!R.Type->isPointerType() && !R.Type->isReferenceType())
      return ResolvedValue();
    R.Type = R.Type->getPointeeType();
    IsLocation = true;
    // A field path needs the location of its containing record.  Loading the
    // final pointee here would turn it into a LazyCompoundVal, which cannot be
    // used as the base of getLValue(FieldDecl, ...).
    if (I + 1 == S.Dereference &&
        (!S.Fields.empty() || S.Mode == SelectorMode::Region))
      break;
    std::optional<Loc> L = R.Value.getAs<Loc>();
    if (!L)
      return ResolvedValue();
    R.Value = State->getSVal(*L);
    IsLocation = false;
  }

  for (unsigned I = 0; I < S.Fields.size(); ++I) {
    QualType RecordTy = R.Type;
    if (RecordTy->isPointerType() || RecordTy->isReferenceType())
      RecordTy = RecordTy->getPointeeType();
    const RecordDecl *RD = RecordTy->getAsRecordDecl();
    RD = RD ? RD->getDefinition() : nullptr;
    if (!RD)
      return ResolvedValue();
    const FieldDecl *Found = nullptr;
    for (const FieldDecl *F : RD->fields())
      if (F->getName() == S.Fields[I]) {
        Found = F;
        break;
      }
    if (!Found)
      return ResolvedValue();
    std::optional<Loc> Base = R.Value.getAs<Loc>();
    if (!Base)
      return ResolvedValue();
    R.Value = State->getLValue(Found, *Base);
    R.Type = Found->getType();
    IsLocation = true;
    if (I + 1 != S.Fields.size()) {
      if (R.Type->isPointerType() || R.Type->isReferenceType()) {
        std::optional<Loc> L = R.Value.getAs<Loc>();
        if (!L)
          return ResolvedValue();
        R.Value = State->getSVal(*L);
        IsLocation = false;
      }
    }
  }

  if (S.Mode == SelectorMode::Value && IsLocation) {
    std::optional<Loc> L = R.Value.getAs<Loc>();
    if (!L)
      return ResolvedValue();
    R.Value = State->getSVal(*L);
  }
  R.Valid = !R.Value.isUnknownOrUndef();
  return R;
}

std::optional<ResourceKey> ConfigDrivenModelingChecker::resourceKey(
    const config_modeling::Selector &S, unsigned ProtocolIndex,
    const CallEvent &Call, ProgramStateRef State, CheckerContext &C) const {
  ResolvedValue R = resolve(S, Call, State, C);
  if (!R.Valid)
    return std::nullopt;
  ResourceKey Key;
  Key.Protocol = ProtocolIndex;
  if (S.Mode == SelectorMode::Region)
    Key.Region = R.Value.getAsRegion();
  else {
    Key.Symbol = R.Value.getAsSymbol();
    if (!Key.Symbol)
      Key.Symbol = R.Value.getAsLocSymbol();
  }
  if (!Key.Symbol && !Key.Region)
    return std::nullopt;
  const Protocol &P = Config->Protocols[ProtocolIndex];
  if (P.Kind == ProtocolKind::Counter &&
      P.CountMode == CounterMode::Balance)
    Key.Frame = C.getStackFrame();
  return Key;
}

ResourceKey ConfigDrivenModelingChecker::canonicalKey(ProgramStateRef State,
                                                       ResourceKey Key) const {
  for (unsigned I = 0; I != 32; ++I) {
    const ResourceKey *Next = State->get<ConfigProtocolAlias>(Key);
    if (!Next || *Next == Key)
      break;
    Key = *Next;
  }
  return Key;
}

SVal ConfigDrivenModelingChecker::makeValue(
    const ValueSpec &Spec, QualType TargetType, const CallEvent &Call,
    ProgramStateRef State, CheckerContext &C) const {
  SValBuilder &SVB = C.getSValBuilder();
  if (TargetType.isNull())
    TargetType = C.getASTContext().IntTy;
  switch (Spec.Kind) {
  case ValueKind::Integer:
    return SVB.makeIntVal(Spec.Integer, TargetType);
  case ValueKind::Boolean:
    return SVB.makeTruthVal(Spec.Boolean, TargetType);
  case ValueKind::Null:
    return SVB.makeZeroVal(TargetType);
  case ValueKind::Selector: {
    ResolvedValue R = resolve(Spec.Select, Call, State, C);
    return R.Valid ? R.Value : UnknownVal();
  }
  case ValueKind::Conjured:
    if (!Call.getOriginExpr() || TargetType->isVoidType())
      return UnknownVal();
    return SVB.conjureSymbolVal(&Spec, Call.getOriginExpr(),
                                C.getLocationContext(), TargetType,
                                C.blockCount());
  case ValueKind::Unknown:
    return UnknownVal();
  }
  llvm_unreachable("unknown value kind");
}

ProgramStateRef ConfigDrivenModelingChecker::applyCondition(
    ProgramStateRef State, const Condition &Cond, bool Assumption,
    const CallEvent &Call, CheckerContext &C) const {
  switch (Cond.Kind) {
  case ConditionKind::Value: {
    ResolvedValue L = resolve(Cond.LHS, Call, State, C);
    if (!L.Valid)
      return Assumption ? nullptr : State;
    SVal RV = makeValue(Cond.RHS, L.Type, Call, State, C);
    if (RV.isUnknownOrUndef())
      return Assumption ? nullptr : State;
    SVal Comparison = C.getSValBuilder().evalBinOp(
        State, toBinaryOpcode(Cond.Op), L.Value, RV,
        C.getSValBuilder().getConditionType());
    std::optional<DefinedOrUnknownSVal> D =
        Comparison.getAs<DefinedOrUnknownSVal>();
    return D ? State->assume(*D, Assumption)
             : (Assumption ? nullptr : State);
  }
  case ConditionKind::Choice: {
    const Expr *Origin = Call.getOriginExpr();
    if (!Origin)
      return Assumption ? nullptr : State;
    SVal Choice = C.getSValBuilder().conjureSymbolVal(
        &Cond, Origin, C.getLocationContext(), C.getASTContext().BoolTy,
        C.blockCount());
    std::optional<DefinedOrUnknownSVal> D =
        Choice.getAs<DefinedOrUnknownSVal>();
    return D ? State->assume(*D, Assumption)
             : (Assumption ? nullptr : State);
  }
  case ConditionKind::ProtocolState: {
    int PI = findProtocol(Cond.Protocol);
    if (PI < 0)
      return Assumption ? nullptr : State;
    std::optional<ResourceKey> Key =
        resourceKey(Cond.Resource, PI, Call, State, C);
    if (!Key)
      return Assumption ? nullptr : State;
    Key = canonicalKey(State, *Key);
    const TrackedResource *Tracked =
        State->get<ConfigProtocolState>(*Key);
    if (!Tracked)
      return Assumption ? nullptr : State;
    const Protocol &P = Config->Protocols[PI];
    bool Matches = false;
    for (const std::string &Name : Cond.States)
      if (Tracked->State == static_cast<unsigned>(findState(P, Name))) {
        Matches = true;
        break;
      }
    return Matches == Assumption ? State : nullptr;
  }
  case ConditionKind::Counter: {
    int PI = findProtocol(Cond.Protocol);
    if (PI < 0)
      return Assumption ? nullptr : State;
    std::optional<ResourceKey> Key =
        resourceKey(Cond.Resource, PI, Call, State, C);
    if (!Key)
      return Assumption ? nullptr : State;
    Key = canonicalKey(State, *Key);
    const TrackedResource *Tracked =
        State->get<ConfigProtocolState>(*Key);
    if (!Tracked || Tracked->CountUnknown)
      return Assumption ? nullptr : State;
    bool Matches = compareCounts(Tracked->Count, Cond.Op, Cond.Count);
    return Matches == Assumption ? State : nullptr;
  }
  }
  llvm_unreachable("unknown condition kind");
}

ExplodedNode *ConfigDrivenModelingChecker::report(
    CheckerContext &C, ProgramStateRef State, const ResourceKey &Key,
    StringRef Message, std::unique_ptr<BugType> &Type,
    StringRef TypeName, ExplodedNode *Predecessor,
    const ProgramPointTag *Tag, bool AddProtocolVisitor,
    bool AddAllocationVisitor) const {
  ExplodedNode *N =
      Predecessor ? C.generateNonFatalErrorNode(State, Predecessor, Tag)
                  : C.generateNonFatalErrorNode(State, Tag);
  if (!N)
    return Predecessor;
  if (!Type)
    Type.reset(new BugType(this, TypeName, "Configured API protocol"));
  auto R = std::make_unique<PathSensitiveBugReport>(*Type, Message, N);
  if (Key.Symbol)
    R->markInteresting(Key.Symbol);
  if (Key.Region)
    R->markInteresting(Key.Region);
  if (AddProtocolVisitor && Config &&
      Key.Protocol < Config->Protocols.size())
    R->addVisitor<ConfiguredProtocolBugVisitor>(
        Key, Config->Protocols[Key.Protocol]);
  if (AddAllocationVisitor && Key.Symbol)
    R->addVisitor(
        allocation_state::getAllocationBRVisitor(Key.Symbol));
  C.emitReport(std::move(R));
  return N;
}

EffectResult ConfigDrivenModelingChecker::applyEffect(
    EffectResult Result, const Effect &E, const CallEvent &Call,
    CheckerContext &C) const {
  ProgramStateRef State = Result.State;
  const Expr *Origin = Call.getOriginExpr();
  switch (E.Kind) {
  case EffectKind::Bind: {
    ResolvedValue Target = resolve(E.Target, Call, State, C);
    QualType Ty = Target.Type;
    SVal V = makeValue(E.Value, Ty, Call, State, C);
    if (E.Target.Base == SelectorBase::Return &&
        E.Target.Dereference == 0 && E.Target.Fields.empty() && Origin) {
      Result.State = State->BindExpr(Origin, C.getLocationContext(), V);
      return Result;
    }
    if (!Target.Valid || E.Target.Mode != SelectorMode::Region)
      return Result;
    Result.State =
        State->bindLoc(Target.Value, V, C.getLocationContext());
    return Result;
  }
  case EffectKind::Invalidate: {
    ResolvedValue Target = resolve(E.Target, Call, State, C);
    if (!Target.Valid || !Origin)
      return Result;
    SmallVector<SVal, 1> Values(1, Target.Value);
    Result.State =
        State->invalidateRegions(Values, Origin, C.blockCount(),
                                 C.getLocationContext(), true, nullptr,
                                 &Call, nullptr);
    return Result;
  }
  case EffectKind::OwnershipAcquire:
  case EffectKind::OwnershipRelease:
  case EffectKind::OwnershipTransfer:
  case EffectKind::OwnershipEscape: {
    ResolvedValue Target = resolve(E.Target, Call, State, C);
    if (!Target.Valid)
      return Result;
    SymbolRef Sym = Target.Value.getAsSymbol();
    if (!Sym)
      Sym = Target.Value.getAsLocSymbol();
    if (!Sym)
      return Result;
    if (E.Kind == EffectKind::OwnershipAcquire) {
      Result.State = allocation_state::markAllocated(
          State, Sym, Origin, toAllocationFamily(E.Family));
      return Result;
    }
    if (E.Kind == EffectKind::OwnershipRelease) {
      if (allocation_state::isReleased(State, Sym)) {
        ResourceKey Key;
        Key.Symbol = Sym;
        static CheckerProgramPointTag Tag(
            this, "ConfiguredOwnershipDoubleRelease");
        Result.Predecessor =
            report(C, State, Key,
                   "Attempt to release already released memory through a "
                   "configured model",
                   DoubleReleaseBug, "Configured double release",
                   Result.Predecessor, &Tag,
                   /*AddProtocolVisitor=*/false,
                   /*AddAllocationVisitor=*/true);
        return Result;
      }
      Result.State =
          allocation_state::isTracked(State, Sym)
              ? allocation_state::markReleased(
                    State, Sym, Origin,
                    dyn_cast_or_null<FunctionDecl>(Call.getDecl()))
              : State;
      return Result;
    }
    if (E.Kind == EffectKind::OwnershipTransfer) {
      Result.State = allocation_state::markRelinquished(State, Sym, Origin);
      return Result;
    }
    Result.State = allocation_state::markEscaped(State, Sym);
    return Result;
  }
  case EffectKind::ProtocolSet:
  case EffectKind::ProtocolTransition:
  case EffectKind::ProtocolAlias:
  case EffectKind::ProtocolEscape:
  case EffectKind::CounterSet:
  case EffectKind::CounterAdjust: {
    int PI = findProtocol(E.Protocol);
    if (PI < 0)
      return Result;
    std::optional<ResourceKey> MaybeKey =
        resourceKey(E.Target, PI, Call, State, C);
    if (!MaybeKey)
      return Result;
    ResourceKey Key = canonicalKey(State, *MaybeKey);
    const Protocol &P = Config->Protocols[PI];

    if (E.Kind == EffectKind::ProtocolAlias) {
      std::optional<ResourceKey> Source =
          resourceKey(E.Source, PI, Call, State, C);
      if (!Source)
        return Result;
      Source = canonicalKey(State, *Source);
      if (*MaybeKey == *Source)
        return Result;
      State = State->remove<ConfigProtocolState>(Key);
      Result.State =
          State->set<ConfigProtocolAlias>(*MaybeKey, *Source);
      return Result;
    }
    if (E.Kind == EffectKind::ProtocolEscape) {
      State = State->remove<ConfigProtocolState>(Key);
      Result.State = State->remove<ConfigProtocolAlias>(*MaybeKey);
      return Result;
    }

    const TrackedResource *Old = State->get<ConfigProtocolState>(Key);
    TrackedResource New;
    if (Old)
      New = *Old;
    New.Origin = Origin;

    if (E.Kind == EffectKind::ProtocolSet) {
      New.State = static_cast<unsigned>(findState(P, E.ToState));
      Result.State = State->set<ConfigProtocolState>(Key, New);
      return Result;
    }
    if (E.Kind == EffectKind::ProtocolTransition) {
      bool Allowed = Old != nullptr;
      if (Allowed && !E.FromStates.empty()) {
        Allowed = false;
        for (const std::string &S : E.FromStates)
          if (Old->State == static_cast<unsigned>(findState(P, S)))
            Allowed = true;
      }
      if (!Allowed) {
        if (P.Diagnostics.InvalidTransition) {
          static CheckerProgramPointTag Tag(
              this, "ConfiguredProtocolInvalidTransition");
          Result.Predecessor =
              report(C, State, Key,
                     (Twine("Invalid transition in configured protocol '") +
                      P.Id + "'").str(),
                     TransitionBug,
                     "Invalid configured protocol transition",
                     Result.Predecessor, &Tag);
        }
        return Result;
      }
      New.State = static_cast<unsigned>(findState(P, E.ToState));
      Result.State = State->set<ConfigProtocolState>(Key, New);
      return Result;
    }

    if (E.Kind == EffectKind::CounterSet) {
      New.Count = E.Count;
      New.CountUnknown =
          E.Count < 0 || static_cast<uint64_t>(E.Count) > P.MaxTrackedCount;
      Result.State = State->set<ConfigProtocolState>(Key, New);
      return Result;
    }

    if (!Old) {
      if (P.CountMode == CounterMode::Balance) {
        New.Count = 0;
        New.CountUnknown = false;
      } else {
        if (P.Diagnostics.InvalidTransition) {
          static CheckerProgramPointTag Tag(
              this, "ConfiguredProtocolUntrackedCounter");
          Result.Predecessor =
              report(C, State, Key,
                     (Twine("Counter operation on an untracked resource in '") +
                      P.Id + "'").str(),
                     TransitionBug,
                     "Invalid configured protocol transition",
                     Result.Predecessor, &Tag);
        }
        return Result;
      }
    }
    if (New.CountUnknown) {
      Result.State = State->set<ConfigProtocolState>(Key, New);
      return Result;
    }
    if ((E.Delta < 0 && New.Count < -E.Delta)) {
      New.Count = 0;
      Result.State = State->set<ConfigProtocolState>(Key, New);
      if (P.Diagnostics.CounterUnderflow) {
        static CheckerProgramPointTag Tag(
            this, "ConfiguredProtocolCounterUnderflow");
        Result.Predecessor =
            report(C, Result.State, Key,
                   (Twine("Counter underflow in configured protocol '") +
                    P.Id + "'").str(),
                   UnderflowBug, "Configured protocol counter underflow",
                   Result.Predecessor, &Tag);
      }
      return Result;
    }
    if (E.Delta > 0 &&
        static_cast<uint64_t>(New.Count) + static_cast<uint64_t>(E.Delta) >
            P.MaxTrackedCount) {
      New.CountUnknown = true;
      Result.State = State->set<ConfigProtocolState>(Key, New);
      return Result;
    }
    New.Count += E.Delta;
    Result.State = State->set<ConfigProtocolState>(Key, New);
    return Result;
  }
  case EffectKind::InvokeCallback: {
    if (Result.CallbackSchedulingFailed)
      return Result;

    auto FailSequence = [&]() {
      Result.State =
          State->remove<PendingConfiguredCallbackSequence>();
      Result.CallbackSchedulingFailed = true;
    };

    ResolvedValue Callback = resolve(E.Callback, Call, State, C);
    const FunctionDecl *CallbackFD =
        Callback.Valid ? getFunctionDeclFromSVal(Callback.Value) : nullptr;
    const auto *CallbackMethod =
        dyn_cast_or_null<CXXMethodDecl>(CallbackFD);
    if (!Call.getOriginExpr() || !CallbackFD || CallbackFD->isVariadic() ||
        (CallbackMethod && !CallbackMethod->isStatic())) {
      FailSequence();
      return Result;
    }

    bool CallbackIsForeign = false;
    const FunctionDecl *BodyFD = getConfiguredCallbackDefinition(
        CallbackFD, State, CallbackIsForeign);
    if (!BodyFD || BodyFD->getNumParams() != E.CallbackArguments.size()) {
      FailSequence();
      return Result;
    }

    SVal Arguments[configured_callback::MaxArguments];
    for (unsigned I = 0; I < E.CallbackArguments.size(); ++I) {
      Arguments[I] =
          makeValue(E.CallbackArguments[I],
                    BodyFD->getParamDecl(I)->getType(), Call, State, C);
      if (Arguments[I].isUndef()) {
        FailSequence();
        return Result;
      }
    }

    SmallVector<Expr *, configured_callback::MaxArguments>
        SyntheticArgumentExprs;
    const CallExpr *SyntheticCall = createSyntheticCallbackCall(
        C.getASTContext(), BodyFD, Call.getOriginExpr(),
        SyntheticArgumentExprs);
    if (!SyntheticCall ||
        SyntheticArgumentExprs.size() != E.CallbackArguments.size()) {
      FailSequence();
      return Result;
    }

    const configured_callback::Sequence *Existing =
        State->get<PendingConfiguredCallbackSequence>();
    if (Existing &&
        Existing->Count >= configured_callback::MaxCallbacks) {
      FailSequence();
      return Result;
    }
    if (!Existing) {
      unsigned Depth = 0;
      for (const configured_callback::Sequence *Parent =
               State->get<ActiveConfiguredCallbackSequence>();
           Parent; Parent = Parent->Parent) {
        if (++Depth >= configured_callback::MaxNestingDepth) {
          FailSequence();
          return Result;
        }
      }
    }

    auto &Allocator = State->getStateManager().getAllocator();
    void *Storage =
        Allocator.Allocate<configured_callback::Sequence>();
    auto *Next = new (Storage) configured_callback::Sequence();
    if (Existing) {
      *Next = *Existing;
    } else {
      Next->Origin = Call.getOriginExpr();
      Next->Parent = State->get<ActiveConfiguredCallbackSequence>();
      Next->ParentIndex = State->get<ActiveConfiguredCallbackIndex>();
      Next->ParentFrame = State->get<ActiveConfiguredCallbackFrame>();
    }

    configured_callback::Invocation &Invocation =
        Next->Invocations[Next->Count++];
    Invocation.Callee = BodyFD->getCanonicalDecl();
    Invocation.IsForeign = CallbackIsForeign;
    Invocation.Call = SyntheticCall;
    Invocation.ArgumentCount =
        static_cast<unsigned>(E.CallbackArguments.size());
    for (unsigned I = 0; I < Invocation.ArgumentCount; ++I)
      Invocation.Arguments[I] = Arguments[I];

    Result.State =
        State->set<PendingConfiguredCallbackSequence>(Next);
    return Result;
  }
  case EffectKind::Terminate: {
    static CheckerProgramPointTag Tag(this, "ConfiguredCallTerminates");
    C.generateSink(State, Result.Predecessor, &Tag);
    Result.Terminated = true;
    return Result;
  }
  }
  llvm_unreachable("unknown effect kind");
}

EffectResult ConfigDrivenModelingChecker::applyEffects(
    ProgramStateRef State, ArrayRef<Effect> Effects, const CallEvent &Call,
    CheckerContext &C) const {
  EffectResult Result{State, C.getPredecessor(), false, false};
  for (const Effect &E : Effects) {
    Result = applyEffect(Result, E, Call, C);
    if (Result.Terminated)
      break;
  }
  return Result;
}

bool ConfigDrivenModelingChecker::requirementSatisfied(
    const Requirement &R, const CallEvent &Call, ProgramStateRef State,
    CheckerContext &C, ResourceKey *Interesting) const {
  int PI = findProtocol(R.Protocol);
  if (PI < 0)
    return true;
  std::optional<ResourceKey> Key = resourceKey(R.Resource, PI, Call, State, C);
  if (!Key)
    return true;
  *Interesting = canonicalKey(State, *Key);
  const TrackedResource *Tracked =
      State->get<ConfigProtocolState>(*Interesting);
  if (!Tracked)
    return false;
  const Protocol &P = Config->Protocols[PI];
  if (P.Kind == ProtocolKind::Counter)
    return Tracked->CountUnknown || Tracked->Count >= R.MinimumCount;
  if (R.AllowedStates.empty())
    return true;
  for (const std::string &S : R.AllowedStates)
    if (Tracked->State == static_cast<unsigned>(findState(P, S)))
      return true;
  return false;
}

void ConfigDrivenModelingChecker::checkPreCall(const CallEvent &Call,
                                                CheckerContext &C) const {
  if (!ensureInitialized(C))
    return;
  const Model *M = findModel(Call);
  if (!M)
    return;
  const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!M->OverrideBody && FD && FD->hasBody())
    return;
  if (M->Match.Kind != CallKind::Constructor &&
      !Call.getResultType().isNull() &&
      Call.getResultType()->isRecordType()) {
    std::optional<SVal> ReturnObject = Call.getReturnValueUnderConstruction();
    if (!ReturnObject || !ReturnObject->getAsRegion())
      return;
  }
  for (const Requirement &R : M->Requires) {
    ResourceKey Key;
    if (requirementSatisfied(R, Call, C.getState(), C, &Key))
      continue;
    int PI = findProtocol(R.Protocol);
    const Protocol &P = Config->Protocols[PI];
    if (P.Diagnostics.InvalidUse)
      report(C, C.getState(), Key,
             (Twine("Call violates the required state of configured protocol '") +
              P.Id + "'").str(),
             UseBug, "Use of resource in an invalid configured state");
  }
}

void ConfigDrivenModelingChecker::addSuppressEscapeTraits(
    const Model &M, const CallEvent &Call, ProgramStateRef State,
    CheckerContext &C, RegionAndSymbolInvalidationTraits &Traits) const {
  auto Add = [&](const config_modeling::Selector &S) {
    if (S.Base == SelectorBase::Return)
      return;
    ResolvedValue R = resolve(S, Call, State, C);
    if (!R.Valid)
      return;
    if (SymbolRef Sym = R.Value.getAsSymbol())
      Traits.setTrait(Sym,
                      RegionAndSymbolInvalidationTraits::TK_SuppressEscape);
    if (SymbolRef Sym = R.Value.getAsLocSymbol())
      Traits.setTrait(Sym,
                      RegionAndSymbolInvalidationTraits::TK_SuppressEscape);
    if (const MemRegion *MR = R.Value.getAsRegion())
      Traits.setTrait(MR,
                      RegionAndSymbolInvalidationTraits::TK_SuppressEscape);
  };
  for (const Requirement &R : M.Requires)
    Add(R.Resource);
  for (const Outcome &O : M.Outcomes)
    for (const Effect &E : O.Effects)
      switch (E.Kind) {
      case EffectKind::OwnershipAcquire:
      case EffectKind::OwnershipRelease:
      case EffectKind::OwnershipTransfer:
      case EffectKind::OwnershipEscape:
      case EffectKind::ProtocolSet:
      case EffectKind::ProtocolTransition:
      case EffectKind::ProtocolAlias:
      case EffectKind::ProtocolEscape:
      case EffectKind::CounterSet:
      case EffectKind::CounterAdjust:
        Add(E.Target);
        if (E.Kind == EffectKind::ProtocolAlias)
          Add(E.Source);
        break;
      case EffectKind::InvokeCallback:
        Add(E.Callback);
        for (const ValueSpec &Argument : E.CallbackArguments)
          if (Argument.Kind == ValueKind::Selector)
            Add(Argument.Select);
        break;
      default:
        break;
      }
}

bool ConfigDrivenModelingChecker::evalCall(const CallEvent &Call,
                                           CheckerContext &C) const {
  if (!ensureInitialized(C))
    return false;
  const Model *M = findModel(Call);
  if (!M)
    return false;
  const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!M->OverrideBody && FD && FD->hasBody())
    return false;

  ProgramStateRef State = C.getState();
  const Expr *Origin = Call.getOriginExpr();
  QualType ResultType = Call.getResultType();
  std::optional<SVal> ReturnObject;
  if (Origin && M->Match.Kind != CallKind::Constructor &&
      !ResultType.isNull() && ResultType->isRecordType()) {
    ReturnObject = Call.getReturnValueUnderConstruction();
    if (!ReturnObject || !ReturnObject->getAsRegion())
      return false;
  }

  if (M->Evaluation == EvaluationKind::Conservative) {
    RegionAndSymbolInvalidationTraits Traits;
    addSuppressEscapeTraits(*M, Call, State, C, Traits);
    State = Call.invalidateRegions(C.blockCount(), State, &Traits);
  }

  if (ReturnObject) {
    SmallVector<SVal, 1> Values(1, *ReturnObject);
    State = State->invalidateRegions(
        Values, Origin, C.blockCount(), C.getLocationContext(),
        /*CausesPointerEscape=*/false, nullptr, nullptr, nullptr);
    auto &Engine = State->getStateManager().getOwningEngine();
    State = Engine.updateObjectsUnderConstruction(
        *ReturnObject, nullptr, State, C.getLocationContext(),
        Call.getConstructionContext(), {});
  } else if (Origin && M->Match.Kind != CallKind::Constructor &&
             !ResultType.isNull() && !ResultType->isVoidType()) {
    SVal Ret = C.getSValBuilder().conjureSymbolVal(
        Origin, C.getLocationContext(), ResultType, C.blockCount());
    State = State->BindExpr(Origin, C.getLocationContext(), Ret);
  }
  C.addTransition(State);
  return true;
}

void ConfigDrivenModelingChecker::checkPostCall(const CallEvent &Call,
                                                 CheckerContext &C) const {
  if (!ensureInitialized(C))
    return;
  const Model *M = findModel(Call);
  if (C.wasInlined)
    return;

  const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (M && !M->OverrideBody && FD && FD->hasBody())
    M = nullptr;
  if (M && M->Match.Kind != CallKind::Constructor &&
      !Call.getResultType().isNull() &&
      Call.getResultType()->isRecordType()) {
    std::optional<SVal> ReturnObject = Call.getReturnValueUnderConstruction();
    if (!ReturnObject || !ReturnObject->getAsRegion())
      return;
  }

  if (!M) {
    ProgramStateRef State = C.getState();
    auto Tracked = State->get<ConfigProtocolState>();
    auto Aliases = State->get<ConfigProtocolAlias>();
    SmallVector<ResourceKey, 8> EscapedKeys;
    auto RecordArgument = [&](SVal Arg) {
      SymbolRef ArgSymbol = Arg.getAsSymbol();
      if (!ArgSymbol)
        ArgSymbol = Arg.getAsLocSymbol();
      const MemRegion *ArgRegion = Arg.getAsRegion();
      for (const auto &Entry : Tracked) {
        const ResourceKey &Key = Entry.first;
        const Protocol &P = Config->Protocols[Key.Protocol];
        if (P.UnknownCall == UnknownCallPolicy::Preserve)
          continue;
        if ((ArgSymbol && Key.Symbol == ArgSymbol) ||
            (ArgRegion && Key.Region &&
             ArgRegion->getBaseRegion() == Key.Region->getBaseRegion()))
          EscapedKeys.push_back(Key);
      }
      for (const auto &Alias : Aliases) {
        const ResourceKey &Key = Alias.first;
        const Protocol &P = Config->Protocols[Key.Protocol];
        if (P.UnknownCall == UnknownCallPolicy::Preserve)
          continue;
        if ((ArgSymbol && Key.Symbol == ArgSymbol) ||
            (ArgRegion && Key.Region &&
             ArgRegion->getBaseRegion() == Key.Region->getBaseRegion()))
          EscapedKeys.push_back(canonicalKey(State, Alias.second));
      }
    };
    for (unsigned I = 0; I < Call.getNumArgs(); ++I)
      RecordArgument(Call.getArgSVal(I));
    for (const ResourceKey &Key : EscapedKeys)
      State = State->remove<ConfigProtocolState>(Key);
    for (const auto &Alias : Aliases)
      for (const ResourceKey &Key : EscapedKeys)
        if (canonicalKey(State, Alias.second) == Key) {
          State = State->remove<ConfigProtocolAlias>(Alias.first);
          break;
        }
    if (State != C.getState())
      C.addTransition(State);
    return;
  }

  SmallVector<ProgramStateRef, 4> Remaining(1, C.getState());
  for (const Outcome &O : M->Outcomes) {
    if (O.IsDefault) {
      for (ProgramStateRef S : Remaining) {
        EffectResult Result = applyEffects(S, O.Effects, Call, C);
        if (!Result.Terminated)
          C.addTransition(Result.State, Result.Predecessor);
      }
      return;
    }

    SmallVector<ProgramStateRef, 8> NextRemaining;
    for (ProgramStateRef Base : Remaining) {
      ProgramStateRef Matched = Base;
      for (const Condition &Cond : O.When) {
        Matched = applyCondition(Matched, Cond, true, Call, C);
        if (!Matched)
          break;
      }
      if (Matched) {
        EffectResult Result = applyEffects(Matched, O.Effects, Call, C);
        if (!Result.Terminated)
          C.addTransition(Result.State, Result.Predecessor);
      }

      ProgramStateRef Prefix = Base;
      for (const Condition &Cond : O.When) {
        ProgramStateRef Failed = applyCondition(Prefix, Cond, false, Call, C);
        if (Failed)
          NextRemaining.push_back(Failed);
        Prefix = applyCondition(Prefix, Cond, true, Call, C);
        if (!Prefix)
          break;
      }
    }
    Remaining.swap(NextRemaining);
  }
}

bool ConfigDrivenModelingChecker::isTerminal(
    const Protocol &P, const TrackedResource &R) const {
  if (P.Kind == ProtocolKind::Counter)
    return R.CountUnknown || R.Count == 0;
  for (const std::string &S : P.TerminalStates)
    if (R.State == static_cast<unsigned>(findState(P, S)))
      return true;
  return false;
}

bool ConfigDrivenModelingChecker::keyIsLive(const ResourceKey &Key,
                                             SymbolReaper &SR) const {
  if (Key.Symbol)
    return !SR.isDead(Key.Symbol);
  return Key.Region && SR.isLiveRegion(Key.Region);
}

bool ConfigDrivenModelingChecker::hasLiveAlias(
    ProgramStateRef State, const ResourceKey &Canonical,
    SymbolReaper &SR) const {
  for (const auto &A : State->get<ConfigProtocolAlias>())
    if (canonicalKey(State, A.second) == Canonical && keyIsLive(A.first, SR))
      return true;
  return false;
}

void ConfigDrivenModelingChecker::checkLiveSymbols(
    ProgramStateRef State, SymbolReaper &SR) const {
  class CallbackSymbolMarker final : public SymbolVisitor {
    SymbolReaper &SR;

  public:
    explicit CallbackSymbolMarker(SymbolReaper &SR) : SR(SR) {}

    bool VisitSymbol(SymbolRef Sym) override {
      SR.markInUse(Sym);
      return true;
    }
  } Marker(SR);

  llvm::SmallPtrSet<const configured_callback::Sequence *, 8> Seen;
  auto MarkSequence = [&](const configured_callback::Sequence *Sequence) {
    for (; Sequence && Seen.insert(Sequence).second;
         Sequence = Sequence->Parent)
      for (unsigned I = 0; I < Sequence->Count; ++I)
        for (unsigned A = 0;
             A < Sequence->Invocations[I].ArgumentCount; ++A)
          State->scanReachableSymbols(
              Sequence->Invocations[I].Arguments[A], Marker);
  };

  MarkSequence(State->get<PendingConfiguredCallbackSequence>());
  MarkSequence(State->get<ActiveConfiguredCallbackSequence>());
}

void ConfigDrivenModelingChecker::checkDeadSymbols(SymbolReaper &SR,
                                                    CheckerContext &C) const {
  if (!ensureInitialized(C))
    return;
  ProgramStateRef State = C.getState();
  for (const auto &Entry : State->get<ConfigProtocolState>()) {
    const ResourceKey &Key = Entry.first;
    const Protocol &P = Config->Protocols[Key.Protocol];
    if (P.Kind == ProtocolKind::Counter &&
        P.CountMode == CounterMode::Balance)
      continue;
    if (keyIsLive(Key, SR) || hasLiveAlias(State, Key, SR))
      continue;
    if (P.Diagnostics.Leak && !isTerminal(P, Entry.second))
      report(C, State, Key,
             (Twine("Resource governed by configured protocol '") + P.Id +
              "' is lost in a non-terminal state").str(),
             LeakBug, "Configured protocol resource leak");
    State = State->remove<ConfigProtocolState>(Key);
  }
  for (const auto &A : State->get<ConfigProtocolAlias>())
    if (!keyIsLive(A.first, SR))
      State = State->remove<ConfigProtocolAlias>(A.first);
  if (State != C.getState())
    C.addTransition(State);
}

void ConfigDrivenModelingChecker::checkEndFunction(const ReturnStmt *,
                                                   CheckerContext &C) const {
  if (!ensureInitialized(C))
    return;
  ProgramStateRef State = C.getState();
  const StackFrameContext *Frame = C.getStackFrame();
  for (const auto &Entry : State->get<ConfigProtocolState>()) {
    const ResourceKey &Key = Entry.first;
    const Protocol &P = Config->Protocols[Key.Protocol];
    if (P.Kind != ProtocolKind::Counter ||
        P.CountMode != CounterMode::Balance || Key.Frame != Frame)
      continue;
    if (P.Diagnostics.Leak && !Entry.second.CountUnknown &&
        Entry.second.Count != 0)
      report(C, State, Key,
             (Twine("Unbalanced operations for configured protocol '") +
              P.Id + "' at function exit").str(),
             LeakBug, "Configured protocol resource leak");
    State = State->remove<ConfigProtocolState>(Key);
  }
  if (State != C.getState())
    C.addTransition(State);
}

ProgramStateRef ConfigDrivenModelingChecker::checkPointerEscape(
    ProgramStateRef State, const InvalidatedSymbols &Escaped,
    const CallEvent *Call, PointerEscapeKind) const {
  if (!ConfigValid || !Config)
    return State;
  (void)Call;
  auto Tracked = State->get<ConfigProtocolState>();
  for (const auto &Entry : Tracked) {
    const ResourceKey &Key = Entry.first;
    const Protocol &P = Config->Protocols[Key.Protocol];
    if (P.UnknownCall == UnknownCallPolicy::Preserve)
      continue;
    bool DidEscape = Key.Symbol && Escaped.count(Key.Symbol);
    if (!DidEscape && Key.Region)
      if (SymbolRef Base = loc::MemRegionVal(Key.Region).getAsSymbol(true))
        DidEscape = Escaped.count(Base);
    if (!DidEscape)
      continue;

    // Explicitly modeled resources are marked TK_SuppressEscape before call
    // invalidation, so any resource reaching this callback is genuinely
    // outside the configured contract.
    State = State->remove<ConfigProtocolState>(Key);
  }
  auto Aliases = State->get<ConfigProtocolAlias>();
  for (const auto &Alias : Aliases) {
    const ResourceKey &Key = Alias.first;
    const Protocol &P = Config->Protocols[Key.Protocol];
    if (P.UnknownCall == UnknownCallPolicy::Preserve)
      continue;
    bool DidEscape = Key.Symbol && Escaped.count(Key.Symbol);
    if (!DidEscape && Key.Region)
      if (SymbolRef Base = loc::MemRegionVal(Key.Region).getAsSymbol(true))
        DidEscape = Escaped.count(Base);
    if (!DidEscape)
      continue;
    State = State->remove<ConfigProtocolState>(
        canonicalKey(State, Alias.second));
    State = State->remove<ConfigProtocolAlias>(Key);
  }
  return State;
}

void ConfigDrivenModelingChecker::checkPreStmt(const ReturnStmt *RS,
                                                CheckerContext &C) const {
  if (!ensureInitialized(C) || !RS->getRetValue())
    return;
  SVal Returned = C.getSVal(RS->getRetValue());
  SymbolRef Sym = Returned.getAsSymbol();
  if (!Sym)
    Sym = Returned.getAsLocSymbol();
  const MemRegion *ReturnedRegion = Returned.getAsRegion();
  if (!Sym && !ReturnedRegion)
    return;
  ProgramStateRef State = C.getState();
  auto Tracked = State->get<ConfigProtocolState>();
  for (const auto &Entry : Tracked)
    if ((Sym && Entry.first.Symbol == Sym) ||
        (ReturnedRegion && Entry.first.Region &&
         ReturnedRegion->getBaseRegion() ==
             Entry.first.Region->getBaseRegion()))
      State = State->remove<ConfigProtocolState>(Entry.first);
  auto Aliases = State->get<ConfigProtocolAlias>();
  for (const auto &Alias : Aliases)
    if ((Sym && Alias.first.Symbol == Sym) ||
        (ReturnedRegion && Alias.first.Region &&
         ReturnedRegion->getBaseRegion() ==
             Alias.first.Region->getBaseRegion())) {
      State = State->remove<ConfigProtocolState>(
          canonicalKey(State, Alias.second));
      State = State->remove<ConfigProtocolAlias>(Alias.first);
    }
  if (State != C.getState())
    C.addTransition(State);
}

} // end anonymous namespace

void ento::registerConfigDrivenModelingChecker(CheckerManager &Mgr) {
  ConfigDrivenModelingChecker *Checker =
      Mgr.registerChecker<ConfigDrivenModelingChecker>();
  Checker->initialize(Mgr);
}

bool ento::shouldRegisterConfigDrivenModelingChecker(
    const CheckerManager &) {
  return true;
}
