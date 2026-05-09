#include "RefactorTool.h"
#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Basic/SourceLocation.h>
#include <cstdint>

std::unique_ptr<llvm::raw_ostream> CodeRefactorAction::m_log_stream;
std::unique_ptr<TextDiagnosticPrinter> CodeRefactorAction::m_log_client;
bool CodeRefactorAction::m_is_log_notes = true;

// Метод run вызывается для каждого совпадения с матчем. 
// Мы проверяем тип совпадения по bind-именам и применяем рефакторинг.
void RefactorHandler::run(const MatchFinder::MatchResult& Result)
{
    auto& Diag = Result.Context->getDiagnostics();
    auto& SM = *Result.SourceManager; // Получаем SourceManager для проверки isInMainFile
    
    // Далее следует распределение операций, выполняемых над отобранными матчерами узлами AST-дерева, по их действительным функциям-исполнителям.
    // Тип конкретного сработавшего матчера устанавливается по связанной с текущим узлом текстовой метке, прикреплённой к нему матчером,
    // отобравшим этот узел.
    if (const auto* Dtor = Result.Nodes.getNodeAs<CXXDestructorDecl>("nonVirtualDestructorDecl"))
        handle_nv_dtor(Dtor, Diag, SM);

    if (const auto* Method = Result.Nodes.getNodeAs<CXXMethodDecl>("methodWithoutOverride");
        Method && Method->size_overridden_methods() > 0 && !Method->hasAttr<OverrideAttr>())
        handle_miss_override(Method, Diag, SM);

    if (const auto* LoopVar = Result.Nodes.getNodeAs<VarDecl>("NonReferenceConstRangeLoopVar"))
        handle_crange_for(LoopVar, Diag, SM);

    if (const auto* DerivedClass = Result.Nodes.getNodeAs<CXXRecordDecl>("DerivedClass"))
        handle_derived_class(DerivedClass, Diag, SM);        
}

// Обработчик накапливает в множестве virtualDtorPtrs все встреченные при анализе невиртуальные деструкторы.
void RefactorHandler::handle_nv_dtor(const CXXDestructorDecl* Dtor, DiagnosticsEngine& Diag, SourceManager& SM)
{
    SourceLocation dtor_begin_loc = Dtor->getLocation();
    // Отбрасываем объекты библиотечных заголовков, а также возможные захваты скрытых деструкторов различных производных классов.
    // Интересующий нас явно объявленный деструктор должен начинаться с '~'.
    if (SM.isInMainFile(dtor_begin_loc) && *SM.getCharacterData(dtor_begin_loc) == '~')
    {
        if (m_is_output_notes)
        {
            const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Note, "Объявлен невиртуальный деструктор");
            Diag.Report(dtor_begin_loc, DiagID);
        }
        virtualDtorPtrs.insert(Dtor);   // Откладываем полученный деструктор для последующей обработки.
    }
}

// Вставка спецификатора override после блока описания формальных параметров метода в том случае, если его там ещё нет.
void RefactorHandler::handle_miss_override(const CXXMethodDecl* Method, DiagnosticsEngine& Diag, SourceManager& SM)
{
    SourceLocation method_end_loc = Method->getNameInfo().getEndLoc();
    if (SM.isInMainFile(method_end_loc))
    {
        const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Установлен спецификатор override для переопределяющего метода");
        Diag.Report(Method->getLocation(), DiagID);

        const char* method_end_pos = SM.getCharacterData(method_end_loc);
        // Отыскиваем символ ')', закрывающий блок формальных параметров метода Method. Именно после него будет вставлен спецификатор "override".
        for (; *method_end_pos != ')'; ++method_end_pos);
        ++method_end_pos;   // Позиция предстоящей вставки спецификатора override сразу после закрывающей скобки блока формальных параметров.

        SourceLocation override_insert_loc =
            method_end_loc.getLocWithOffset(method_end_pos - SM.getCharacterData(method_end_loc));
        Rewrite.InsertTextAfter(override_insert_loc, " override"s);
    }
}

// Преобразование в ссылку константной рабочей переменной диапазонного цикла, если она не относится к легко копируемым фундаментальным типам.
void RefactorHandler::handle_crange_for(const VarDecl* LoopVar, DiagnosticsEngine& Diag, SourceManager& SM)
{
    SourceLocation loop_varname_end_loc = LoopVar->getEndLoc();
    if (SM.isInMainFile(loop_varname_end_loc) && !LoopVar->getType()->isReferenceType())
    {
        if (m_is_output_notes)
        {
            const unsigned DiagID =
                Diag.getCustomDiagID(DiagnosticsEngine::Note, "Найдена константная нессылочная рабочая переменная диапазонного цикла");
            Diag.Report(LoopVar->getLocation(), DiagID);
        }
        if (LoopVar->getType()->isBuiltinType())
        {
            if (m_is_output_notes)
            {
                const unsigned BuiltinTypeDiagID =
                    Diag.getCustomDiagID(DiagnosticsEngine::Note, "Отказ от действия - переменная фундаментального типа");
                Diag.Report(BuiltinTypeDiagID);
            }
            return;
        }
        // Найденная конструкция находится внутри основного анализируемого файла (не внутри вложенных заголовков), а переменная не относится к
        // фундаментальным типам C++ и не является ссылкой.
        // Отыскиваем теперь конец имени типа в полном терме объявления переменной цикла.
        const char* var_defterm_pos = SM.getCharacterData(loop_varname_end_loc);
        --var_defterm_pos;  // Указатель на последний символ терма.
        for (; *var_defterm_pos == ' '; --var_defterm_pos);  // Идя с конца тела терма, пропускаем сначала завершающие его пробелы.
        for (; *var_defterm_pos != ' '; --var_defterm_pos); // Далее выполняем пропуск самого имени переменной.
        for (; *var_defterm_pos == ' '; --var_defterm_pos); // Наконец, находим конечный символ искомого имени типа.
        ++var_defterm_pos;  // Расчёт окончательного положения точки вставки.
        //
        const unsigned DiagOpID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Переменная диапазонного цикла преобразована в ссылку");
        Diag.Report(LoopVar->getLocation(), DiagOpID);
        //
        SourceLocation ref_insert_loc =
            loop_varname_end_loc.getLocWithOffset(var_defterm_pos - SM.getCharacterData(loop_varname_end_loc));

        Rewrite.InsertTextAfter(ref_insert_loc, "&"s);
    }
}

// Обработчик-накопитель базовых классов текущей единицы трансляции, от которых наследуются какие-либо иные её классы.
void RefactorHandler::handle_derived_class(const CXXRecordDecl* ClassDecl, DiagnosticsEngine& Diag, SourceManager& SM)
{
    if (SM.isInMainFile(ClassDecl->getLocation()))
    {
        if (m_is_output_notes)
        {
            const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Note, "Найден наследующий класс");
            Diag.Report(ClassDecl->getLocation(), DiagID);
        }
        // Вставляем во множество-сборщик allBaseClassCollection все базовые классы текущего производного класса ClassDecl.
        // так как теперь ясно, что они достоверно имеют каких-либо наследников.
        for (const CXXBaseSpecifier& base_class_spec : ClassDecl->bases())
            allBaseClassCollection.insert(base_class_spec.getType()->getAsCXXRecordDecl());
    }
}

//todo: ниже необходимо реализовать матчеры для поиска узлов AST
auto NvDtorMatcher()
{
    // Матчер обнаружения только невиртуальных деструкторов и только в том случае, если класс, для которого такой деструктор определен,
    // имеет какие-либо производные от него классы.
    return cxxDestructorDecl
            (unless(isVirtual()))        // Условие удовлетворяется, если деструктор невиртуальный.
            .bind("nonVirtualDestructorDecl");
}

auto NoOverrideMatcher()
{
    // Матчер для отбора методов, которые фактически переопределяют какой-либо метод предка, но не имеют спецификатора override.
    return cxxMethodDecl
        (unless(isImplicit()),                 // Отсекает всякие неявные и автоматически порождаемые методы (в частности, неявно генерируемые конструкторы производных классов).
         isOverride(),                              // Отбирает только фактически переопределяющие методы.
         unless(hasAttr(attr::Override)))  // Но без указания явного override.
        .bind("methodWithoutOverride");
}

auto NoRefConstVarInRangeLoopMatcher()
{
    // Обнаруживаются определения переменных, являющихся рабочими для диапазонного цикла, объявленными константными, но не являющиеся ссылками.
    return varDecl
        (hasAncestor(cxxForRangeStmt()),    // Предок - диапазонный цикл.
         hasType(isConstQualified()),            // Переменная константного типа.
         unless(hasType(referenceType())))       // Но пока не ссылка.
        .bind("NonReferenceConstRangeLoopVar");
}

auto AllClassesMatcher()
{
    // Матчер перечисления всех классов, кроме тех, что не имеют никаких предков.
    return cxxRecordDecl(hasAnyBase(anything())).bind("DerivedClass");
}

// Конструктор принимает Rewriter для изменения кода и набор прочих параметров, управляющих некоторыми иными аспектами рабочего процесса.
ComplexConsumer::ComplexConsumer(Rewriter& Rewrite, const ConsumerParams& Parameters) :
    Handler(Rewrite), Params(Parameters)
{
    // Создаем MatchFinder и добавляем матчеры.
    Finder.addMatcher(NvDtorMatcher(), &Handler);
    Finder.addMatcher(NoOverrideMatcher(), &Handler);
    Finder.addMatcher(NoRefConstVarInRangeLoopMatcher(), &Handler);
    Finder.addMatcher(AllClassesMatcher(), &Handler);
}

// Метод HandleTranslationUnit вызывается для каждого файла.
void ComplexConsumer::HandleTranslationUnit(ASTContext& Context)
{
    auto& Diag = Context.getDiagnostics();
    if (Params.m_log_client)
        Diag.setClient(Params.m_log_client, false);

    Finder.matchAST(Context);
    // Теперь мы можем выяснить необходимость "виртуализации" деструктора для всех тех классов, у которых он сейчас
    // невиртуальный, но при этом они имеют наследников. Предварительно все подозреваемые деструкторы (точнее, указатели
    // на соответствующим узлы в AST) накоплены во множестве virtualDtorPtrs.
    for (const CXXDestructorDecl* try_dtor : Handler.virtualDtorPtrs)
    {
        if (Handler.allBaseClassCollection.find(try_dtor->getParent()) != Handler.allBaseClassCollection.end())
        {   // Класс, к которому принадлежит невиртуальный деструктор try_dtor, имеет наследников.
            SourceLocation virtual_insert_pos = try_dtor->getLocation();
            Handler.Rewrite.InsertTextBefore(virtual_insert_pos, "virtual "s);
            // Отчёт в протокол о выполненном действии.
            const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Выполнена виртуализация деструктора");
            Diag.Report(virtual_insert_pos, DiagID);
        }
    }
}

std::unique_ptr<ASTConsumer> CodeRefactorAction::CreateASTConsumer(CompilerInstance& CI, StringRef file)
{
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    // Создаем и настраиваем объект обработчика узлов AST.
    ComplexConsumer::ConsumerParams consumer_params;
    if (m_log_stream)
        consumer_params.m_log_client = m_log_client.get();
    consumer_params.m_is_log_notes = m_is_log_notes;
    return std::make_unique<ComplexConsumer>(RewriterForCodeRefactor, consumer_params);
}

bool CodeRefactorAction::BeginSourceFileAction(CompilerInstance& CI)
{
    // Инициализируем Rewriter для рефакторинга.
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    if (m_log_client)
        m_log_client->BeginSourceFile(CI.getLangOpts(), nullptr);
    return true;  // Возвращаем true, чтобы продолжить обработку файла.
}

void CodeRefactorAction::EndSourceFileAction()
{
    // Применяем изменения в файле.
    if (RewriterForCodeRefactor.overwriteChangedFiles())
        llvm::errs() << "Ошибка при внесении изменений в файл.\n";
    if (m_log_client)
        m_log_client->EndSourceFile();
}

void CodeRefactorAction::SetLogParams(std::unique_ptr<llvm::raw_ostream>&& p_log_stream, bool p_is_log_notes)
{
    m_log_client.reset();
    m_log_stream.reset();

    if (p_log_stream)
    {    
        m_log_stream = std::move(p_log_stream);
        m_log_client = std::make_unique<TextDiagnosticPrinter>(*m_log_stream.get(), new DiagnosticOptions);
    }
    m_is_log_notes = p_is_log_notes;
}
