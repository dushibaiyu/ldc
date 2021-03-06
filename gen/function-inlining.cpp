//===-- function-inlining.cpp ---------------------------------------------===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//

#include "gen/function-inlining.h"

#include "declaration.h"
#include "globals.h"
#include "id.h"
#include "module.h"
#include "statement.h"
#include "template.h"
#include "gen/logger.h"
#include "gen/optimizer.h"
#include "gen/recursivevisitor.h"
#include "gen/uda.h"

namespace {

/// An ASTVisitor that checks whether the number of statements is larger than a
/// certain number.
struct MoreThanXStatements : public StoppableVisitor {
  /// Are there more or fewer statements than `threshold`?.
  unsigned threshold;
  /// The statement count.
  unsigned count;

  explicit MoreThanXStatements(unsigned X) : threshold(X), count(0) {}

  using StoppableVisitor::visit;

  void visit(Statement *stmt) override {
    count++;
    if (count > threshold)
      stop = true;
  }
  void visit(Expression *exp) override {}
  void visit(Declaration *decl) override {}
  void visit(Initializer *init) override {}
  void visit(Dsymbol *) override {}
};

// Use a heuristic to determine if it could make sense to inline this fdecl.
// Note: isInlineCandidate is called _before_ semantic3 analysis of fdecl.
bool isInlineCandidate(FuncDeclaration &fdecl) {
  // Giving maximum inlining potential to LLVM should be possible, but we
  // restrict it to save some compile time.
  // return true;

  // TODO: make the heuristic more sophisticated?
  // In the end, LLVM will make the decision whether to _actually_ inline.
  // The statement count threshold is completely arbitrary. Also, all
  // statements are weighed the same.

  unsigned statementThreshold = 10;
  MoreThanXStatements statementCounter(statementThreshold);
  RecursiveWalker walker(&statementCounter, false);
  fdecl.fbody->accept(&walker);

  IF_LOG Logger::println("Contains %u statements or more (threshold = %u).",
                         statementCounter.count, statementThreshold);
  return statementCounter.count <= statementThreshold;
}

} // end anonymous namespace

bool alreadyOrWillBeDefined(FuncDeclaration &fdecl) {
  for (FuncDeclaration *f = &fdecl; f;) {
    if (!f->isInstantiated() && f->inNonRoot()) {
      return false;
    }
    if (f->isNested()) {
      f = f->toParent2()->isFuncDeclaration();
    } else {
      break;
    }
  }
  return true;
}

bool defineAsExternallyAvailable(FuncDeclaration &fdecl) {
  IF_LOG Logger::println("Enter defineAsExternallyAvailable");
  LOG_SCOPE

#if LDC_LLVM_VER < 307
  // Pre-3.7, cross-module inlining is disabled completely.
  // See the commandline flag definition for more details.
  IF_LOG Logger::println("LLVM < 3.7: Cross-module inlining disabled.");
  return false;
#endif

  // FIXME: For now, disable all cross-module inlining (also of pragma(inline, true)
  // functions). This check should be removed when cross-module inlining has
  // become more stable.
  // There are related `FIXME`s in a few lit-based `codegen/inlining_*.d` tests.
  if (!willCrossModuleInline()) {
    IF_LOG Logger::println("Cross-module inlining fully disabled.");
    return false;
  }

  // Implementation note: try to do cheap checks first.

  if (fdecl.neverInline || fdecl.inlining == PINLINEnever) {
    IF_LOG Logger::println("pragma(inline, false) specified");
    return false;
  }

  // pragma(inline, true) functions will be inlined even at -O0
  if (fdecl.inlining == PINLINEalways) {
    IF_LOG Logger::println(
        "pragma(inline, true) specified, overrides cmdline flags");
  } else if (!willCrossModuleInline()) {
    IF_LOG Logger::println("Commandline flags indicate no inlining");
    return false;
  }

  if (fdecl.isUnitTestDeclaration()) {
    IF_LOG Logger::println("isUnitTestDeclaration() == true");
    return false;
  }
  if (fdecl.isFuncAliasDeclaration()) {
    IF_LOG Logger::println("isFuncAliasDeclaration() == true");
    return false;
  }
  if (!fdecl.fbody) {
    IF_LOG Logger::println("No function body available for inlining");
    return false;
  }

  // Because the frontend names `__invariant*` functions differently depending
  // on the compilation order, we cannot emit the `__invariant` wrapper that
  // calls the `__invariant*` functions.
  // This is a workaround, the frontend needs to be changed such that the
  // __invariant* names no longer depend on semantic analysis order.
  // See https://github.com/ldc-developers/ldc/issues/1678
  if (fdecl.isInvariantDeclaration()) {
    IF_LOG Logger::println("__invariant cannot be emitted.");
    return false;
  }

  // TODO: Fix inlining functions from object.d. Currently errors because of
  // TypeInfo type-mismatch issue (TypeInfo classes get special treatment by the
  // compiler). To start working on it: comment-out this check and druntime will
  // fail to compile.
  if (fdecl.getModule()->ident == Id::object) {
    IF_LOG Logger::println("Inlining of object.d functions is disabled");
    return false;
  }

  if (fdecl.semanticRun >= PASSsemantic3) {
    // If semantic analysis has come this far, the function will be defined
    // elsewhere and should not get the available_externally attribute from
    // here.
    // TODO: This check prevents inlining of nested functions.
    IF_LOG Logger::println("Semantic analysis already completed");
    return false;
  }

  if (alreadyOrWillBeDefined(fdecl)) {
    // This check is needed because of ICEs happening because of unclear issues
    // upon changing the codegen order without this check.
    IF_LOG Logger::println("Function will be defined later.");
    return false;
  }

  // Weak-linkage functions can not be inlined.
  if (hasWeakUDA(&fdecl)) {
    IF_LOG Logger::println("@weak functions cannot be inlined.");
    return false;
  }

  if (fdecl.inlining != PINLINEalways && !isInlineCandidate(fdecl))
    return false;

  IF_LOG Logger::println("Potential inlining candidate");

  {
    IF_LOG Logger::println("Do semantic analysis");
    LOG_SCOPE

    // The inlining is aggressive and may give semantic errors that are
    // forward referencing errors. Simply avoid those cases for inlining.
    unsigned errors = global.startGagging();
    global.gaggedForInlining = true;

    bool semantic_error = false;
    if (fdecl.functionSemantic3()) {
      Module::runDeferredSemantic3();
    } else {
      IF_LOG Logger::println("Failed functionSemantic3.");
      semantic_error = true;
    }

    global.gaggedForInlining = false;
    if (global.endGagging(errors) || semantic_error) {
      IF_LOG Logger::println("Errors occured during semantic analysis.");
      return false;
    }
    assert(fdecl.semanticRun >= PASSsemantic3done);
  }

  // FuncDeclaration::naked is set by the AsmParser during semantic3 analysis,
  // and so this check can only be done at this late point.
  if (fdecl.naked) {
    IF_LOG Logger::println("Naked asm functions cannot be inlined.");
    return false;
  }

  IF_LOG Logger::println("defineAsExternallyAvailable? Yes.");
  return true;
}
