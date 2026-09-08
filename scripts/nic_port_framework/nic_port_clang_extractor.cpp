// nic_port_clang_extractor.cpp
//
// Dedicated LibTooling backend for NIC Port Framework V3.3.
// Emits compiler-derived AST + preprocessor facts without materializing the
// enormous `clang -ast-dump=json` tree.  The JSON contract is intentionally
// close to the Python builder's KB schema and is additive/backward compatible.
//
// Build (typical LLVM/Clang installation):
//   clang++ -std=c++17 -O2 nic_port_clang_extractor.cpp \
//     $(llvm-config --cxxflags --ldflags --system-libs --libs) \
//     -lclangTooling -lclangFrontend -lclangAST -lclangASTMatchers \
//     -lclangBasic -lclangLex -lclangSerialization -o nic-port-clang-extractor
//
// Some distributions expose a monolithic libclang-cpp instead:
//   clang++ -std=c++17 -O2 nic_port_clang_extractor.cpp \
//     $(llvm-config --cxxflags --ldflags --system-libs --libs) \
//     -lclang-cpp -o nic-port-clang-extractor
//
// Runtime:
//   nic-port-clang-extractor \
//       --compile-commands /build/compile_commands.json \
//       --source-root /src/driver \
//       --output /tmp/extractor.json
//
// The tool deliberately performs conservative alias/callback analysis.  It
// records evidence and candidate targets; it does not invent a unique indirect
// target when data flow does not prove one.

#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/JSONCompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::tooling;

namespace {

llvm::cl::OptionCategory Category("nic-port-clang-extractor");
llvm::cl::opt<std::string> CompileCommands(
    "compile-commands", llvm::cl::Required,
    llvm::cl::desc("Path to compile_commands.json"), llvm::cl::cat(Category));
llvm::cl::opt<std::string> SourceRoot(
    "source-root", llvm::cl::Required, llvm::cl::desc("Driver source root"),
    llvm::cl::cat(Category));
llvm::cl::opt<std::string> Output(
    "output", llvm::cl::Required, llvm::cl::desc("Output JSON file"),
    llvm::cl::cat(Category));
llvm::cl::list<std::string> OnlyFiles(
    "file", llvm::cl::ZeroOrMore,
    llvm::cl::desc("Optional source file(s) to process; default: all compile DB files under source root"),
    llvm::cl::cat(Category));
llvm::cl::opt<std::string> PPScope(
    "pp-scope", llvm::cl::init("project"),
    llvm::cl::desc("Preprocessor event scope: project, main-file, or all"),
    llvm::cl::cat(Category));
llvm::cl::list<std::string> PPEventKinds(
    "pp-event-kind", llvm::cl::ZeroOrMore,
    llvm::cl::desc("Optional preprocessor event kind allow-list; repeat option"),
    llvm::cl::cat(Category));

static std::string normalizePath(llvm::StringRef P) {
  llvm::SmallString<512> S(P);
  llvm::sys::path::remove_dots(S, true);
  return std::string(S.str());
}

static bool pathUnder(llvm::StringRef Path, llvm::StringRef Root) {
  std::string P = normalizePath(Path);
  std::string R = normalizePath(Root);
  if (!R.empty() && R.back() != '/') R.push_back('/');
  return P == normalizePath(Root) || llvm::StringRef(P).starts_with(R);
}

static std::string relPath(llvm::StringRef Path, llvm::StringRef Root) {
  std::string P = normalizePath(Path);
  std::string R = normalizePath(Root);
  if (!R.empty() && R.back() != '/') R.push_back('/');
  if (llvm::StringRef(P).starts_with(R)) return P.substr(R.size());
  return P;
}

static std::string locFile(const SourceManager &SM, SourceLocation Loc,
                           llvm::StringRef Root) {
  if (Loc.isInvalid()) return {};
  SourceLocation E = SM.getExpansionLoc(Loc);
  auto F = SM.getFilename(E);
  return F.empty() ? std::string() : relPath(F, Root);
}

static unsigned locLine(const SourceManager &SM, SourceLocation Loc) {
  if (Loc.isInvalid()) return 0;
  return SM.getExpansionLineNumber(SM.getExpansionLoc(Loc));
}

static unsigned locCol(const SourceManager &SM, SourceLocation Loc) {
  if (Loc.isInvalid()) return 0;
  return SM.getExpansionColumnNumber(SM.getExpansionLoc(Loc));
}

static std::string sourceText(const SourceManager &SM, const LangOptions &LO,
                              SourceRange R) {
  if (R.isInvalid()) return {};
  bool Invalid = false;
  auto T = Lexer::getSourceText(CharSourceRange::getTokenRange(R), SM, LO, &Invalid);
  return Invalid ? std::string() : T.str();
}

static std::string sourceText(const ASTContext &Ctx, const Stmt *S) {
  if (!S) return {};
  return sourceText(Ctx.getSourceManager(), Ctx.getLangOpts(), S->getSourceRange());
}

static std::string sourceText(const ASTContext &Ctx, const Decl *D) {
  if (!D) return {};
  return sourceText(Ctx.getSourceManager(), Ctx.getLangOpts(), D->getSourceRange());
}

static std::string stripSpaces(std::string S) {
  std::string O;
  O.reserve(S.size());
  bool WasSpace = false;
  for (char C : S) {
    bool Is = std::isspace(static_cast<unsigned char>(C));
    if (Is) {
      if (!WasSpace) O.push_back(' ');
    } else {
      O.push_back(C);
    }
    WasSpace = Is;
  }
  return O;
}

static const Expr *stripExpr(const Expr *E) {
  if (!E) return nullptr;
  return E->IgnoreParenImpCasts();
}

struct TUData {
  std::string TUFile;
  llvm::json::Array Functions;
  llvm::json::Array Structs;
  llvm::json::Array Enums;
  llvm::json::Array CallbackRefs;
  llvm::json::Array FunctionPointerBindings;
  llvm::json::Array PPEvents;
  llvm::json::Array Errors;
  uint64_t PPEventsSeen = 0;
  uint64_t PPEventsRetained = 0;
  uint64_t PPEventsDroppedScope = 0;
  uint64_t PPEventsDroppedKind = 0;
};

struct SharedData {
  std::string Root;
  std::vector<TUData> TUs;
};

static llvm::json::Object locationObj(const SourceManager &SM, SourceLocation L,
                                      llvm::StringRef Root) {
  return llvm::json::Object{{"file", locFile(SM, L, Root)},
                            {"line", static_cast<int64_t>(locLine(SM, L))},
                            {"column", static_cast<int64_t>(locCol(SM, L))}};
}

class PPRecorder final : public PPCallbacks {
 public:
  PPRecorder(CompilerInstance &CI, TUData &Out, std::string Root)
      : CI(CI), SM(CI.getSourceManager()), Out(Out), Root(std::move(Root)) {}

  bool eventEnabled(llvm::StringRef Kind) const {
    if (PPEventKinds.empty()) return true;
    return std::find(PPEventKinds.begin(), PPEventKinds.end(), Kind.str()) != PPEventKinds.end();
  }

  bool locationInScope(SourceLocation Loc) const {
    if (PPScope.getValue() == "all") return true;
    if (Loc.isInvalid()) return false;
    SourceLocation E = SM.getExpansionLoc(Loc);
    if (PPScope.getValue() == "main-file") return SM.isWrittenInMainFile(E);
    auto F = SM.getFilename(E);
    return !F.empty() && pathUnder(F, Root);
  }

  bool shouldRecord(llvm::StringRef Kind, SourceLocation Loc) {
    ++Out.PPEventsSeen;
    if (!eventEnabled(Kind)) { ++Out.PPEventsDroppedKind; return false; }
    if (!locationInScope(Loc)) { ++Out.PPEventsDroppedScope; return false; }
    ++Out.PPEventsRetained;
    return true;
  }

  void MacroDefined(const Token &Tok, const MacroDirective *MD) override {
    if (!shouldRecord("macro_defined", Tok.getLocation())) return;
    llvm::json::Object O{{"kind", "macro_defined"},
                         {"name", Tok.getIdentifierInfo() ? Tok.getIdentifierInfo()->getName().str() : ""},
                         {"location", locationObj(SM, Tok.getLocation(), Root)}};
    Out.PPEvents.push_back(std::move(O));
  }

  void MacroUndefined(const Token &Tok, const MacroDefinition &,
                      const MacroDirective *) override {
    if (!shouldRecord("macro_undefined", Tok.getLocation())) return;
    Out.PPEvents.push_back(llvm::json::Object{
        {"kind", "macro_undefined"},
        {"name", Tok.getIdentifierInfo() ? Tok.getIdentifierInfo()->getName().str() : ""},
        {"location", locationObj(SM, Tok.getLocation(), Root)}});
  }

  void MacroExpands(const Token &Tok, const MacroDefinition &, SourceRange R,
                    const MacroArgs *) override {
    if (!shouldRecord("macro_expands", Tok.getLocation())) return;
    Out.PPEvents.push_back(llvm::json::Object{
        {"kind", "macro_expands"},
        {"name", Tok.getIdentifierInfo() ? Tok.getIdentifierInfo()->getName().str() : ""},
        {"location", locationObj(SM, Tok.getLocation(), Root)},
        {"range_text", stripSpaces(sourceText(SM, CI.getLangOpts(), R))}});
  }

  void Defined(const Token &Tok, const MacroDefinition &, SourceRange R) override {
    if (!shouldRecord("defined_operator", Tok.getLocation())) return;
    Out.PPEvents.push_back(llvm::json::Object{
        {"kind", "defined_operator"},
        {"name", Tok.getIdentifierInfo() ? Tok.getIdentifierInfo()->getName().str() : ""},
        {"location", locationObj(SM, Tok.getLocation(), Root)},
        {"condition", stripSpaces(sourceText(SM, CI.getLangOpts(), R))}});
  }

  void If(SourceLocation Loc, SourceRange R, ConditionValueKind V) override {
    if (!shouldRecord("if", Loc)) return;
    conditional("if", Loc, sourceText(SM, CI.getLangOpts(), R), valueName(V));
  }

  void Ifdef(SourceLocation Loc, const Token &Tok, const MacroDefinition &MD) override {
    if (!shouldRecord("ifdef", Loc)) return;
    conditional("ifdef", Loc,
                Tok.getIdentifierInfo() ? Tok.getIdentifierInfo()->getName().str() : "",
                MD.getMacroInfo() ? "true" : "false");
  }

  void Ifndef(SourceLocation Loc, const Token &Tok, const MacroDefinition &MD) override {
    if (!shouldRecord("ifndef", Loc)) return;
    conditional("ifndef", Loc,
                Tok.getIdentifierInfo() ? Tok.getIdentifierInfo()->getName().str() : "",
                MD.getMacroInfo() ? "false" : "true");
  }

  void Elif(SourceLocation Loc, SourceRange R, ConditionValueKind V,
            SourceLocation IfLoc) override {
    if (!shouldRecord("elif", Loc)) return;
    llvm::json::Object O{{"kind", "elif"},
                         {"location", locationObj(SM, Loc, Root)},
                         {"condition", stripSpaces(sourceText(SM, CI.getLangOpts(), R))},
                         {"value", valueName(V)},
                         {"if_location", locationObj(SM, IfLoc, Root)}};
    Out.PPEvents.push_back(std::move(O));
  }

  void Else(SourceLocation Loc, SourceLocation IfLoc) override {
    if (!shouldRecord("else", Loc)) return;
    Out.PPEvents.push_back(llvm::json::Object{{"kind", "else"},
                                              {"location", locationObj(SM, Loc, Root)},
                                              {"if_location", locationObj(SM, IfLoc, Root)}});
  }

  void Endif(SourceLocation Loc, SourceLocation IfLoc) override {
    if (!shouldRecord("endif", Loc)) return;
    Out.PPEvents.push_back(llvm::json::Object{{"kind", "endif"},
                                              {"location", locationObj(SM, Loc, Root)},
                                              {"if_location", locationObj(SM, IfLoc, Root)}});
  }

  void SourceRangeSkipped(SourceRange R, SourceLocation EndifLoc) override {
    if (!shouldRecord("source_range_skipped", R.getBegin())) return;
    Out.PPEvents.push_back(llvm::json::Object{
        {"kind", "source_range_skipped"},
        {"location", locationObj(SM, R.getBegin(), Root)},
        {"end_location", locationObj(SM, R.getEnd(), Root)},
        {"endif_location", locationObj(SM, EndifLoc, Root)}});
  }

 private:
  static std::string valueName(ConditionValueKind V) {
    switch (V) {
      case CVK_False: return "false";
      case CVK_True: return "true";
      case CVK_NotEvaluated: return "not_evaluated";
    }
    return "unknown";
  }

  void conditional(llvm::StringRef Kind, SourceLocation Loc, std::string Cond,
                   llvm::StringRef Value) {
    Out.PPEvents.push_back(llvm::json::Object{{"kind", Kind.str()},
                                              {"location", locationObj(SM, Loc, Root)},
                                              {"condition", stripSpaces(std::move(Cond))},
                                              {"value", Value.str()}});
  }

  CompilerInstance &CI;
  SourceManager &SM;
  TUData &Out;
  std::string Root;
};

struct AliasInfo {
  std::string Target;
  std::vector<std::string> Chain;
};

class FunctionAnalyzer : public RecursiveASTVisitor<FunctionAnalyzer> {
 public:
  FunctionAnalyzer(ASTContext &Ctx, const FunctionDecl *FD, std::string Root)
      : Ctx(Ctx), SM(Ctx.getSourceManager()), FD(FD), Root(std::move(Root)) {}

  bool VisitVarDecl(VarDecl *VD) {
    if (!VD || !VD->hasInit() || !VD->getIdentifier()) return true;
    std::string RHS = canonical(VD->getInit());
    if (!RHS.empty() && (VD->getType()->isPointerType() || VD->getType()->isReferenceType())) {
      AliasInfo A{RHS, {VD->getNameAsString(), RHS}};
      Aliases[VD->getNameAsString()] = std::move(A);
      AliasRelations.push_back(llvm::json::Object{
          {"alias", VD->getNameAsString()}, {"target", RHS},
          {"line", static_cast<int64_t>(locLine(SM, VD->getLocation()))},
          {"kind", "declaration"}});
    }
    if (containsFunctionRef(VD->getInit())) {
      FunctionPointerBindings.push_back(llvm::json::Object{
          {"lhs", VD->getNameAsString()}, {"targets", functionRefs(VD->getInit())},
          {"line", static_cast<int64_t>(locLine(SM, VD->getLocation()))},
          {"kind", "initializer"}});
    }
    return true;
  }

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (!BO || !BO->isAssignmentOp()) return true;
    const Expr *L = stripExpr(BO->getLHS());
    const Expr *R = stripExpr(BO->getRHS());
    if (const auto *DR = dyn_cast_or_null<DeclRefExpr>(L)) {
      if (const auto *VD = dyn_cast<VarDecl>(DR->getDecl())) {
        std::string RHS = canonical(R);
        if (!RHS.empty() && VD->getType()->isPointerType()) {
          Aliases[VD->getNameAsString()] = AliasInfo{RHS, {VD->getNameAsString(), RHS}};
          AliasRelations.push_back(llvm::json::Object{
              {"alias", VD->getNameAsString()}, {"target", RHS},
              {"line", static_cast<int64_t>(locLine(SM, BO->getExprLoc()))},
              {"kind", "assignment"}});
        }
      }
    }
    if (containsFunctionRef(R)) {
      llvm::json::Array Targets = functionRefs(R);
      FunctionPointerBindings.push_back(llvm::json::Object{
          {"lhs", stripSpaces(sourceText(Ctx, L))}, {"targets", std::move(Targets)},
          {"line", static_cast<int64_t>(locLine(SM, BO->getExprLoc()))},
          {"kind", "assignment"}});
    }
    return true;
  }

  bool VisitMemberExpr(MemberExpr *ME) {
    if (!ME) return true;
    std::string Base = canonical(ME->getBase());
    std::string Field = ME->getMemberDecl()->getNameAsString();
    std::string Mode = accessMode(ME);
    std::string ObjText = stripSpaces(sourceText(Ctx, ME->getBase()));
    bool ThroughAlias = false;
    std::vector<std::string> Chain;
    if (const auto *DR = dyn_cast_or_null<DeclRefExpr>(stripExpr(ME->getBase()))) {
      auto It = Aliases.find(DR->getDecl()->getNameAsString());
      if (It != Aliases.end()) {
        ThroughAlias = true;
        Chain = It->second.Chain;
      }
    }
    llvm::json::Array JC;
    for (const auto &X : Chain) JC.push_back(X);
    FieldAccesses.push_back(llvm::json::Object{
        {"field", Field},
        {"mode", Mode},
        {"line", static_cast<int64_t>(locLine(SM, ME->getExprLoc()))},
        {"object_expr", ObjText},
        {"canonical_object", Base},
        {"object_path", Base.empty() ? Field : Base + "." + Field},
        {"through_alias", ThroughAlias},
        {"alias_chain", std::move(JC)}});
    return true;
  }

  bool VisitCallExpr(CallExpr *CE) {
    if (!CE) return true;
    if (const FunctionDecl *Callee = CE->getDirectCallee()) {
      Calls.push_back(llvm::json::Object{
          {"callee_name", Callee->getNameAsString()},
          {"decl_file", locFile(SM, Callee->getLocation(), Root)},
          {"line", static_cast<int64_t>(locLine(SM, CE->getExprLoc()))},
          {"indirect", false}});
      return true;
    }
    const Expr *CalleeExpr = stripExpr(CE->getCallee());
    std::string ExprText = stripSpaces(sourceText(Ctx, CalleeExpr));
    llvm::json::Array Candidates;
    auto CandidateSet = inferIndirectCandidates(CalleeExpr);
    for (const auto &X : CandidateSet) Candidates.push_back(X);
    Calls.push_back(llvm::json::Object{
        {"callee_name", "<indirect>"},
        {"callee_expr", ExprText},
        {"line", static_cast<int64_t>(locLine(SM, CE->getExprLoc()))},
        {"indirect", true},
        {"candidate_targets", std::move(Candidates)},
        {"resolution", CandidateSet.size() == 1 ? "single_candidate" :
                           (CandidateSet.empty() ? "unresolved" : "candidate_set")}});
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *DR) {
    if (!DR) return true;
    auto *Target = dyn_cast<FunctionDecl>(DR->getDecl());
    if (!Target) return true;
    if (isDirectCallCallee(DR)) return true;
    FunctionRefs.push_back(llvm::json::Object{
        {"target_name", Target->getNameAsString()},
        {"target_file", locFile(SM, Target->getLocation(), Root)},
        {"file", locFile(SM, DR->getExprLoc(), Root)},
        {"line", static_cast<int64_t>(locLine(SM, DR->getExprLoc()))},
        {"enclosing_function", FD->getNameAsString()},
        {"context", parentContext(DR)}});
    return true;
  }

  llvm::json::Array takeCalls() { return std::move(Calls); }
  llvm::json::Array takeFieldAccesses() { return std::move(FieldAccesses); }
  llvm::json::Array takeAliases() { return std::move(AliasRelations); }
  llvm::json::Array takeFunctionRefs() { return std::move(FunctionRefs); }
  llvm::json::Array takeFunctionPointerBindings() { return std::move(FunctionPointerBindings); }

 private:
  std::string canonical(const Expr *E, std::set<std::string> Seen = {}) const {
    E = stripExpr(E);
    if (!E) return {};
    if (const auto *DR = dyn_cast<DeclRefExpr>(E)) {
      std::string N = DR->getDecl()->getNameAsString();
      auto It = Aliases.find(N);
      if (It != Aliases.end() && !Seen.count(N)) {
        Seen.insert(N);
        return It->second.Target;
      }
      return N;
    }
    if (const auto *ME = dyn_cast<MemberExpr>(E)) {
      std::string B = canonical(ME->getBase(), Seen);
      std::string F = ME->getMemberDecl()->getNameAsString();
      return B.empty() ? F : B + "." + F;
    }
    if (const auto *U = dyn_cast<UnaryOperator>(E)) return canonical(U->getSubExpr(), Seen);
    if (const auto *A = dyn_cast<ArraySubscriptExpr>(E)) {
      std::string B = canonical(A->getBase(), Seen);
      return B.empty() ? "[]" : B + "[]";
    }
    if (const auto *C = dyn_cast<ConditionalOperator>(E)) {
      std::string A = canonical(C->getTrueExpr(), Seen);
      std::string B = canonical(C->getFalseExpr(), Seen);
      return A == B ? A : stripSpaces(sourceText(Ctx, E));
    }
    return stripSpaces(sourceText(Ctx, E));
  }

  std::string accessMode(const MemberExpr *ME) const {
    auto Parents = Ctx.getParents(*ME);
    if (Parents.empty()) return "read";
    if (const auto *BO = Parents[0].get<BinaryOperator>()) {
      if (BO->isAssignmentOp()) {
        const Expr *L = stripExpr(BO->getLHS());
        if (L == stripExpr(ME)) return BO->getOpcode() == BO_Assign ? "write" : "readwrite";
        return "read";
      }
    }
    if (const auto *UO = Parents[0].get<UnaryOperator>()) {
      if (UO->isIncrementDecrementOp()) return "readwrite";
      if (UO->getOpcode() == UO_AddrOf) return "address_taken";
    }
    return "read";
  }

  bool isDirectCallCallee(const DeclRefExpr *DR) const {
    auto P = Ctx.getParents(*DR);
    if (P.empty()) return false;
    if (const auto *ICE = P[0].get<ImplicitCastExpr>()) {
      auto P2 = Ctx.getParents(*ICE);
      if (!P2.empty()) {
        if (const auto *CE = P2[0].get<CallExpr>()) return CE->getDirectCallee() == DR->getDecl();
      }
    }
    if (const auto *CE = P[0].get<CallExpr>()) return CE->getDirectCallee() == DR->getDecl();
    return false;
  }

  std::string parentContext(const DeclRefExpr *DR) const {
    DynTypedNode Cur = DynTypedNode::create(*DR);
    for (int I = 0; I < 5; ++I) {
      auto P = Ctx.getParents(Cur);
      if (P.empty()) break;
      Cur = P[0];
      if (const auto *VD = Cur.get<VarDecl>()) return "var_init:" + VD->getNameAsString();
      if (const auto *BO = Cur.get<BinaryOperator>()) {
        if (BO->isAssignmentOp()) return "assignment:" + stripSpaces(sourceText(Ctx, BO->getLHS()));
      }
      if (Cur.get<InitListExpr>()) return "initializer";
      if (Cur.get<ReturnStmt>()) return "return";
    }
    return "function_reference";
  }

  bool containsFunctionRef(const Expr *E) const { return !functionRefs(E).empty(); }

  llvm::json::Array functionRefs(const Expr *E) const {
    class Finder : public RecursiveASTVisitor<Finder> {
     public:
      bool VisitDeclRefExpr(DeclRefExpr *D) {
        if (auto *F = dyn_cast<FunctionDecl>(D->getDecl())) Names.insert(F->getNameAsString());
        return true;
      }
      std::set<std::string> Names;
    } F;
    if (E) F.TraverseStmt(const_cast<Expr *>(E));
    llvm::json::Array A;
    for (const auto &N : F.Names) A.push_back(N);
    return A;
  }

  std::set<std::string> inferIndirectCandidates(const Expr *E) const {
    std::set<std::string> R;
    std::string Key = stripSpaces(sourceText(Ctx, E));
    for (const auto &V : FunctionPointerBindings) {
      const auto *O = V.getAsObject();
      if (!O) continue;
      auto LHS = O->getString("lhs");
      if (!LHS || *LHS != Key) continue;
      if (const auto *A = O->getArray("targets"))
        for (const auto &X : *A) if (auto S = X.getAsString()) R.insert(S->str());
    }
    return R;
  }

  ASTContext &Ctx;
  SourceManager &SM;
  const FunctionDecl *FD;
  std::string Root;
  mutable std::unordered_map<std::string, AliasInfo> Aliases;
  llvm::json::Array Calls;
  llvm::json::Array FieldAccesses;
  llvm::json::Array AliasRelations;
  llvm::json::Array FunctionRefs;
  llvm::json::Array FunctionPointerBindings;
};

class TUVisitor : public RecursiveASTVisitor<TUVisitor> {
 public:
  TUVisitor(ASTContext &Ctx, TUData &Out, std::string Root)
      : Ctx(Ctx), SM(Ctx.getSourceManager()), Out(Out), Root(std::move(Root)) {}

  bool shouldVisit(const Decl *D) const {
    std::string F = locFile(SM, D->getLocation(), Root);
    return !F.empty() && !llvm::StringRef(F).starts_with("../") && pathUnder(SM.getFilename(SM.getExpansionLoc(D->getLocation())), Root);
  }

  bool VisitFunctionDecl(FunctionDecl *FD) {
    if (!FD || !FD->doesThisDeclarationHaveABody() || !shouldVisit(FD)) return true;
    const Stmt *Body = FD->getBody();
    unsigned Start = locLine(SM, FD->getBeginLoc());
    unsigned End = locLine(SM, FD->getEndLoc());
    std::string File = locFile(SM, FD->getLocation(), Root);

    FunctionAnalyzer FA(Ctx, FD, Root);
    FA.TraverseStmt(const_cast<Stmt *>(Body));

    llvm::json::Array Params;
    for (const ParmVarDecl *P : FD->parameters()) {
      Params.push_back(llvm::json::Object{{"name", P->getNameAsString()},
                                         {"type", P->getType().getAsString()}});
    }

    llvm::json::Object F{
        {"stable_id", "FN::" + File + "::" + FD->getNameAsString()},
        {"key", File + ":" + std::to_string(Start) + ":" + FD->getNameAsString()},
        {"name", FD->getNameAsString()},
        {"file", File},
        {"line_start", static_cast<int64_t>(Start)},
        {"line_end", static_cast<int64_t>(End)},
        {"storage", FD->getStorageClass() == SC_Static ? "static" :
                    FD->getStorageClass() == SC_Extern ? "extern" : ""},
        {"return_type", FD->getReturnType().getAsString()},
        {"signature", stripSpaces(sourceText(Ctx, FD).substr(0, sourceText(Ctx, FD).find('{')))},
        {"body", sourceText(Ctx, Body)},
        {"full_source", sourceText(Ctx, FD)},
        {"parameters", std::move(Params)},
        {"calls_raw", FA.takeCalls()},
        {"field_accesses", FA.takeFieldAccesses()},
        {"alias_relations", FA.takeAliases()},
        {"callback_references", FA.takeFunctionRefs()},
        {"function_pointer_bindings", FA.takeFunctionPointerBindings()}};
    Out.Functions.push_back(std::move(F));
    return true;
  }

  bool VisitRecordDecl(RecordDecl *RD) {
    if (!RD || !RD->isCompleteDefinition() || !shouldVisit(RD)) return true;
    std::string File = locFile(SM, RD->getLocation(), Root);
    unsigned Line = locLine(SM, RD->getLocation());
    llvm::json::Array Fields;
    for (const FieldDecl *F : RD->fields()) {
      Fields.push_back(llvm::json::Object{{"name", F->getNameAsString()},
                                         {"type", F->getType().getAsString()}});
    }
    std::string Name = RD->getNameAsString().empty() ? "<anonymous>" : RD->getNameAsString();
    Out.Structs.push_back(llvm::json::Object{
        {"stable_id", "TYPE::" + File + "::" + Name},
        {"key", File + ":" + std::to_string(Line) + ":" + Name},
        {"name", Name}, {"kind", RD->isUnion() ? "union" : "struct"},
        {"file", File}, {"line", static_cast<int64_t>(Line)},
        {"fields", std::move(Fields)}, {"source", sourceText(Ctx, RD)}});
    return true;
  }

  bool VisitEnumDecl(EnumDecl *ED) {
    if (!ED || !ED->isCompleteDefinition() || !shouldVisit(ED)) return true;
    std::string File = locFile(SM, ED->getLocation(), Root);
    unsigned Line = locLine(SM, ED->getLocation());
    std::string Name = ED->getNameAsString().empty() ? "<anonymous>" : ED->getNameAsString();
    llvm::json::Array Values;
    for (const EnumConstantDecl *E : ED->enumerators()) Values.push_back(E->getNameAsString());
    Out.Enums.push_back(llvm::json::Object{
        {"stable_id", "ENUM::" + File + "::" + Name},
        {"key", File + ":" + std::to_string(Line) + ":" + Name},
        {"name", Name}, {"file", File}, {"line", static_cast<int64_t>(Line)},
        {"values", std::move(Values)}, {"source", sourceText(Ctx, ED)}});
    return true;
  }

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (!BO || !BO->isAssignmentOp()) return true;
    class FnFinder : public RecursiveASTVisitor<FnFinder> {
     public:
      bool VisitDeclRefExpr(DeclRefExpr *D) {
        if (auto *F = dyn_cast<FunctionDecl>(D->getDecl())) Names.insert(F->getNameAsString());
        return true;
      }
      std::set<std::string> Names;
    } Finder;
    Finder.TraverseStmt(BO->getRHS());
    if (Finder.Names.empty()) return true;
    llvm::json::Array Targets;
    for (const auto &N : Finder.Names) Targets.push_back(N);
    Out.FunctionPointerBindings.push_back(llvm::json::Object{
        {"lhs", stripSpaces(sourceText(Ctx, BO->getLHS()))},
        {"targets", std::move(Targets)},
        {"file", locFile(SM, BO->getExprLoc(), Root)},
        {"line", static_cast<int64_t>(locLine(SM, BO->getExprLoc()))},
        {"kind", "assignment"}});
    return true;
  }

  bool VisitVarDecl(VarDecl *VD) {
    if (!VD || !VD->hasInit() || !shouldVisit(VD)) return true;
    class FnFinder : public RecursiveASTVisitor<FnFinder> {
     public:
      bool VisitDeclRefExpr(DeclRefExpr *D) {
        if (auto *F = dyn_cast<FunctionDecl>(D->getDecl())) Names.insert(F->getNameAsString());
        return true;
      }
      std::set<std::string> Names;
    } Finder;
    Finder.TraverseStmt(VD->getInit());
    if (!Finder.Names.empty()) {
      llvm::json::Array Targets;
      for (const auto &N : Finder.Names) Targets.push_back(N);
      Out.FunctionPointerBindings.push_back(llvm::json::Object{
          {"lhs", VD->getNameAsString()}, {"targets", std::move(Targets)},
          {"file", locFile(SM, VD->getLocation(), Root)},
          {"line", static_cast<int64_t>(locLine(SM, VD->getLocation()))},
          {"kind", "initializer"},
          {"initializer_text", stripSpaces(sourceText(Ctx, VD->getInit()))}});
      for (const auto &N : Finder.Names) {
        Out.CallbackRefs.push_back(llvm::json::Object{
            {"target_name", N},
            {"file", locFile(SM, VD->getLocation(), Root)},
            {"line", static_cast<int64_t>(locLine(SM, VD->getLocation()))},
            {"enclosing_declaration", VD->getNameAsString()},
            {"enclosing_kind", "VarDecl"},
            {"context", "var_init:" + VD->getNameAsString()}});
      }
    }

    // Preserve field-level provenance for C99 ops tables such as
    // `static const struct foo_ops ops = { .open = foo_open, ... }`.
    class DesignatedBindingFinder : public RecursiveASTVisitor<DesignatedBindingFinder> {
     public:
      DesignatedBindingFinder(ASTContext &Ctx, TUData &Out, std::string Root,
                              std::string VarName)
          : Ctx(Ctx), SM(Ctx.getSourceManager()), Out(Out), Root(std::move(Root)),
            VarName(std::move(VarName)) {}

      bool VisitDesignatedInitExpr(DesignatedInitExpr *DIE) {
        if (!DIE) return true;
        std::string Path = VarName;
        for (const auto &D : DIE->designators()) {
          if (D.isFieldDesignator()) {
            if (const FieldDecl *FD = D.getFieldDecl())
              Path += "." + FD->getNameAsString();
          } else if (D.isArrayDesignator()) {
            Path += "[]";
          } else if (D.isArrayRangeDesignator()) {
            Path += "[..]";
          }
        }
        class LocalFnFinder : public RecursiveASTVisitor<LocalFnFinder> {
         public:
          bool VisitDeclRefExpr(DeclRefExpr *DR) {
            if (auto *FD = dyn_cast<FunctionDecl>(DR->getDecl())) Names.insert(FD->getNameAsString());
            return true;
          }
          std::set<std::string> Names;
        } FF;
        FF.TraverseStmt(DIE->getInit());
        if (FF.Names.empty()) return true;
        llvm::json::Array Targets;
        for (const auto &N : FF.Names) Targets.push_back(N);
        Out.FunctionPointerBindings.push_back(llvm::json::Object{
            {"lhs", Path}, {"targets", std::move(Targets)},
            {"file", locFile(SM, DIE->getExprLoc(), Root)},
            {"line", static_cast<int64_t>(locLine(SM, DIE->getExprLoc()))},
            {"kind", "designated_initializer"},
            {"initializer_text", stripSpaces(sourceText(Ctx, DIE->getInit()))}});
        for (const auto &N : FF.Names) {
          Out.CallbackRefs.push_back(llvm::json::Object{
              {"target_name", N},
              {"file", locFile(SM, DIE->getExprLoc(), Root)},
              {"line", static_cast<int64_t>(locLine(SM, DIE->getExprLoc()))},
              {"enclosing_declaration", VarName},
              {"enclosing_kind", "DesignatedInitExpr"},
              {"context", "ops_binding:" + Path}});
        }
        return true;
      }

     private:
      ASTContext &Ctx;
      SourceManager &SM;
      TUData &Out;
      std::string Root;
      std::string VarName;
    } DBF(Ctx, Out, Root, VD->getNameAsString());
    DBF.TraverseStmt(VD->getInit());
    return true;
  }

 private:
  ASTContext &Ctx;
  SourceManager &SM;
  TUData &Out;
  std::string Root;
};

class Consumer : public ASTConsumer {
 public:
  Consumer(ASTContext &Ctx, TUData &Out, std::string Root)
      : Visitor(Ctx, Out, std::move(Root)) {}
  void HandleTranslationUnit(ASTContext &Ctx) override {
    Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
  }
 private:
  TUVisitor Visitor;
};

class Action : public ASTFrontendAction {
 public:
  explicit Action(SharedData &Shared) : Shared(Shared) {}

  bool BeginSourceFileAction(CompilerInstance &CI) override {
    TUData D;
    auto Main = CI.getSourceManager().getFileEntryRefForID(CI.getSourceManager().getMainFileID());
    D.TUFile = Main ? relPath(Main->getName(), Shared.Root) : getCurrentFile().str();
    Shared.TUs.push_back(std::move(D));
    Current = &Shared.TUs.back();
    CI.getPreprocessor().addPPCallbacks(std::make_unique<PPRecorder>(CI, *Current, Shared.Root));
    return true;
  }

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef) override {
    return std::make_unique<Consumer>(CI.getASTContext(), *Current, Shared.Root);
  }

 private:
  SharedData &Shared;
  TUData *Current = nullptr;
};

class Factory : public FrontendActionFactory {
 public:
  explicit Factory(SharedData &Shared) : Shared(Shared) {}
  std::unique_ptr<FrontendAction> create() override {
    return std::make_unique<Action>(Shared);
  }
 private:
  SharedData &Shared;
};


static bool startsWithAny(llvm::StringRef A,
                          std::initializer_list<llvm::StringRef> Prefixes) {
  for (auto P : Prefixes)
    if (A.starts_with(P)) return true;
  return false;
}

// Defensive normalization for GCC-generated Kbuild compilation databases.
// The Python builder normally writes a sanitized tooling DB first, but the
// extractor remains safe to invoke directly.  Preserve flags that affect the
// language, preprocessor, ABI widths and include graph; discard diagnostics,
// dependency output and GCC/codegen-only hardening switches that Clang's
// frontend may reject before AST construction.
static CommandLineArguments adjustKernelToolingArgs(
    const CommandLineArguments &Args, llvm::StringRef) {
  CommandLineArguments Out;
  if (Args.empty()) return Out;
  Out.push_back(Args.front());
  for (size_t I = 1; I < Args.size(); ++I) {
    llvm::StringRef A(Args[I]);
    if (A == "-o" || A == "-MF" || A == "-MT" || A == "-MQ" ||
        A == "-MJ" || A == "--serialize-diagnostics") {
      if (I + 1 < Args.size()) ++I;
      continue;
    }
    if (A == "-c" || A == "-S" || A == "-E" || A == "-MD" ||
        A == "-MMD" || A == "-MP" || A == "-MG" || A == "-M" ||
        A == "-MM" || A == "-pg" || A == "-mfentry" ||
        A == "-mrecord-mcount" || A == "-mindirect-branch-register" ||
        A == "-mindirect-branch-cs-prefix" || A == "-fconserve-stack" ||
        A == "-fno-allow-store-data-races" ||
        A == "-femit-struct-debug-baseonly" || A == "-fno-ipa-cp-clone" ||
        A == "-fno-ipa-sra" || A == "-fno-partial-inlining" ||
        A == "-fno-reorder-blocks" ||
        A == "-fno-inline-functions-called-once")
      continue;
    if (A.starts_with("-W")) continue;
    if (startsWithAny(A, {"-Wp,-M", "-fplugin=", "-fplugin-arg-",
                          "-mpreferred-stack-boundary=",
                          "-mindirect-branch=", "-mfunction-return=",
                          "-falign-jumps=", "-falign-loops=", "--param=",
                          "-fasan-shadow-offset=", "-fzero-call-used-regs=",
                          "-fmin-function-alignment=",
                          "-fzero-init-padding-bits=",
                          "-fpatchable-function-entry="}))
      continue;
    Out.push_back(Args[I]);
  }
  // Diagnostics should never turn source-analysis incompatibilities into
  // warning-policy failures.
  Out.push_back("-Wno-everything");
  return Out;
}

static llvm::json::Object tuToJson(TUData &T) {
  return llvm::json::Object{{"tu_file", T.TUFile},
                            {"functions", std::move(T.Functions)},
                            {"structs", std::move(T.Structs)},
                            {"enums", std::move(T.Enums)},
                            {"callback_refs", std::move(T.CallbackRefs)},
                            {"function_pointer_bindings", std::move(T.FunctionPointerBindings)},
                            {"pp_events", std::move(T.PPEvents)},
                            {"pp_summary", llvm::json::Object{
                                {"scope", PPScope.getValue()},
                                {"seen", static_cast<int64_t>(T.PPEventsSeen)},
                                {"retained", static_cast<int64_t>(T.PPEventsRetained)},
                                {"dropped_scope", static_cast<int64_t>(T.PPEventsDroppedScope)},
                                {"dropped_kind", static_cast<int64_t>(T.PPEventsDroppedKind)}}},
                            {"errors", std::move(T.Errors)}};
}

}  // namespace

int main(int argc, const char **argv) {
  llvm::cl::HideUnrelatedOptions(Category);
  llvm::cl::ParseCommandLineOptions(argc, argv, "NIC port LibTooling extractor\n");

  std::string Error;
  auto DB = JSONCompilationDatabase::loadFromFile(
      CompileCommands, Error, JSONCommandLineSyntax::AutoDetect);
  if (!DB) {
    llvm::errs() << "Failed to load compilation database: " << Error << "\n";
    return 2;
  }

  std::vector<std::string> Files;
  if (!OnlyFiles.empty()) {
    Files.assign(OnlyFiles.begin(), OnlyFiles.end());
  } else {
    for (const auto &F : DB->getAllFiles()) {
      if (pathUnder(F, SourceRoot)) Files.push_back(F);
    }
  }
  std::sort(Files.begin(), Files.end());
  Files.erase(std::unique(Files.begin(), Files.end()), Files.end());
  if (Files.empty()) {
    llvm::errs() << "No compilation-database source files under source root\n";
    return 3;
  }

  SharedData Shared;
  Shared.Root = normalizePath(SourceRoot);
  ClangTool Tool(*DB, Files);
  // Analysis only: suppress dependency/output-producing compile arguments.
  Tool.appendArgumentsAdjuster(adjustKernelToolingArgs);
  Tool.appendArgumentsAdjuster(getClangStripOutputAdjuster());
  Tool.appendArgumentsAdjuster(getClangSyntaxOnlyAdjuster());
  Factory F(Shared);
  int RC = Tool.run(&F);

  llvm::json::Array TUs;
  for (auto &T : Shared.TUs) TUs.push_back(tuToJson(T));
  llvm::json::Object RootObj{{"schema_version", "3.3"},
                             {"extractor", "nic_port_clang_extractor"},
                             {"source_root", Shared.Root},
                             {"translation_units", std::move(TUs)},
                             {"tool_return_code", static_cast<int64_t>(RC)}};

  std::error_code EC;
  llvm::raw_fd_ostream OS(Output, EC);
  if (EC) {
    llvm::errs() << "Cannot open output: " << EC.message() << "\n";
    return 4;
  }
  OS << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(RootObj)));
  return RC;
}
