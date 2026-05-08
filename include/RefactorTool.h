#pragma once
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/CommandLine.h"

#include <clang/AST/DeclCXX.h>
#include <cstdint>
#include <unordered_set>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace std::literals;

static llvm::cl::OptionCategory ToolCategory("refactor-tool options");

class RefactorHandler : public clang::ast_matchers::MatchFinder::MatchCallback
{
    friend class ComplexConsumer;

public:
    explicit RefactorHandler(clang::Rewriter& Rewrite) : Rewrite(Rewrite)
    {}
    // Метод run вызывается для каждого совпадения с матчем. 
    // Мы проверяем тип совпадения по bind-именам и применяем рефакторинг.
    virtual void run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;

private:
    // Обработчики содержимого узлов AST-дерева, удовлетворяющих каким-либо поддерживаемым вариантам автоматических
    // модификаций и обнаруженных соответствующими матчерами.
    // 1. Невиртуальные деструкторы
    void handle_nv_dtor(const clang::CXXDestructorDecl* Dtor,
                        clang::DiagnosticsEngine& Diag,
                        clang::SourceManager& SM);

    // 2. Методы без override
    void handle_miss_override(const clang::CXXMethodDecl* Method,
                              clang::DiagnosticsEngine& Diag,
                              clang::SourceManager& SM);

    // 3. range-for без &
    void handle_crange_for(const clang::VarDecl *LoopVar,
                           clang::DiagnosticsEngine &Diag,
                           clang::SourceManager &SM);

    // 4. Вспомогательный обработчик-коллектор всех производных классов анализируемой единицы трансляции.
    void handle_derived_class(const clang::CXXRecordDecl* ClassDecl,
                              clang::DiagnosticsEngine& Diag,
                              clang::SourceManager& SM);

private:
    clang::Rewriter& Rewrite;
    // Набор массивов для выполнения подстановки спецификатора virtual в деструкторы базовых классов.
    std::unordered_set<const CXXDestructorDecl*> virtualDtorPtrs;
    std::unordered_set<const CXXRecordDecl*> allBaseClassCollection;
    // Аналогично для позиций вставки override фактически переопределяющих методов классов.
    //std::unordered_set<uintptr_t> overrideInsertLocations;
};

class ComplexConsumer : public clang::ASTConsumer
{
public:
    // Конструктор принимает Rewriter для изменения кода.
    explicit ComplexConsumer(clang::Rewriter &Rewrite);
    // Метод HandleTranslationUnit вызывается для каждого файла.
    void HandleTranslationUnit(clang::ASTContext &Context) override;

private:
    RefactorHandler Handler;                                // Обработчик матчеров.
    clang::ast_matchers::MatchFinder Finder;                // MatchFinder для поиска узлов AST.
};

class CodeRefactorAction : public clang::ASTFrontendAction
{
public:
  // Returns our ASTConsumer per translation unit.
  virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI, clang::StringRef file) override;
  virtual bool BeginSourceFileAction( clang::CompilerInstance &CI) override;
  virtual void EndSourceFileAction() override;

private:
  clang::Rewriter RewriterForCodeRefactor;
};
