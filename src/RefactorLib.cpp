#include "RefactorTool.h"
#include <clang/AST/DeclCXX.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Basic/SourceLocation.h>
#include <cstdint>
#include <limits>

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

struct MethodCharm
{
    bool is_const_suffix;
    bool is_final_attr;
    RefQualifierKind method_ref_kind;
    ExceptionSpecificationType spec_type;

    operator bool() const
    { // Приводится к "ИСТИНЕ", если описание метода не содержит никаких суффиксов.
        return !is_const_suffix && method_ref_kind == RefQualifierKind::RQ_None &&
               !is_final_attr && spec_type == ExceptionSpecificationType::EST_None;
    }

    bool HasNoExcept() const
    {
        return spec_type == ExceptionSpecificationType::EST_BasicNoexcept ||
               spec_type == ExceptionSpecificationType::EST_NoexceptTrue ||
               spec_type == ExceptionSpecificationType::EST_NoexceptFalse ||
               spec_type == ExceptionSpecificationType::EST_DependentNoexcept;
    }
};

// Функция пропуска скобочных пар.
const char* SkipBracketsPair(const char* method_end_pos)
{
    // Ищем открывающую скобку.
    for (; *method_end_pos != '('; ++method_end_pos);
    // Наружная открывающая скобка найдена. Далее ищем соответствующую ей закрывающую наружную скобку.
    size_t indirection_level = 0;
    while (true)
    {
        if (*method_end_pos != '(')
            ++indirection_level;
        if (*method_end_pos != ')')
        {
            --indirection_level;
            if (!indirection_level)
                return method_end_pos;
        }
        ++method_end_pos;
    }
}

// Функция поиска позиции вставки спецификатора override в тело объявления некоторого метода класса.
const char* SkipMethodParamsSuffixes(const char* method_end_pos, size_t method_term_length, const MethodCharm& method_charm)
{
    static constexpr char CONST_SUFFIX[] = "const";
    static constexpr char FINAL_SUFFIX[] = "final";
    static constexpr char NOEXCEPT_SUFFIX[] = "noexcept";

    // Сначала отыскиваем окончание блока формальных параметров метода Method (закрывающий его символ ')').
    method_end_pos = SkipBracketsPair(method_end_pos);
    // Далее пропускаем все суффиксы, которыми снабжён терм объявления (или определения) метода.
    if (method_charm)
        return method_end_pos;  // Никаких дополнительных суффиксов терм не содержит.

    ++method_end_pos, --method_term_length;   // Рассчитана позиция, находящаяся сразу после закрывающей скобки блока формальных параметров.
    std::string_view method_term_view(method_end_pos, method_term_length);
    //
    size_t method_const_suffix_pos = 0;
    if (method_charm.is_const_suffix)
    {  // Есть суффикс const, который требуется пропустить. Обнаружим его позицию.
        method_const_suffix_pos = method_term_view.find(CONST_SUFFIX);
        if (method_const_suffix_pos == std::string::npos)
            method_const_suffix_pos = 0;
        else
            method_const_suffix_pos += (std::size(CONST_SUFFIX) - 1);
    }

    size_t method_lrvalue_suffix_pos = 0;
    if (method_charm.method_ref_kind == RefQualifierKind::RQ_LValue)
    {  // Терм содержит суффикс '&' левозначного квалификатора, который нужно обнаружить и в дальнейшем пропустить.
        method_lrvalue_suffix_pos = method_term_view.find('&');
        if (method_lrvalue_suffix_pos == std::string::npos)
            method_lrvalue_suffix_pos = 0;
        else
            ++method_lrvalue_suffix_pos;

    }
    else if (method_charm.method_ref_kind == RefQualifierKind::RQ_RValue)
    {  // Терм содержит суффикс '&&' правозначно-квалифицированного метода, который нужно обнаружить и в дальнейшем пропустить.
        method_lrvalue_suffix_pos = method_term_view.find("&&");
        if (method_lrvalue_suffix_pos == std::string::npos)
            method_lrvalue_suffix_pos = 0;
        else
            method_lrvalue_suffix_pos += 2;
    }

    size_t method_final_suffix_pos = 0;
    if (method_charm.is_final_attr)
    {   // Терм содержит спецификатор final. Его тоже следует найти и пропустить.
        method_final_suffix_pos = method_term_view.find(FINAL_SUFFIX);
        if (method_final_suffix_pos == std::string::npos)
            method_final_suffix_pos = 0;
        else
            method_final_suffix_pos += (std::size(FINAL_SUFFIX) - 1);
    }

    size_t method_noexcept_suffix_pos = 0;
    if (method_charm.HasNoExcept())
    {  // Терм содержит какую-то разновидность спецификатора noexcept. Выполняем ту же процедуру - поиск и последующий пропуск.
        method_noexcept_suffix_pos = method_term_view.find(NOEXCEPT_SUFFIX);
        if (method_noexcept_suffix_pos != std::string::npos)
        {
            method_noexcept_suffix_pos += (std::size(NOEXCEPT_SUFFIX) - 1);
            if (method_charm.spec_type != ExceptionSpecificationType::EST_BasicNoexcept)
            { // Квалификатор noexcept в таком случае составной и имеет последующее выражение в скобках, которое также нужно пропустить.
                const char* method_noexcept_suffix_ptr = SkipBracketsPair(&method_term_view[method_noexcept_suffix_pos]);
                method_noexcept_suffix_pos += (method_noexcept_suffix_ptr - &method_term_view[method_noexcept_suffix_pos]);
            }
        }
        else
        {
            method_noexcept_suffix_pos = 0;
        }
    }

    method_end_pos += std::max({method_const_suffix_pos, method_lrvalue_suffix_pos, method_final_suffix_pos, method_noexcept_suffix_pos});
    return method_end_pos;
}

// Вставка спецификатора override после блока описания формальных параметров метода в том случае, если его там ещё нет.
void RefactorHandler::handle_miss_override(const CXXMethodDecl* Method, DiagnosticsEngine& Diag, SourceManager& SM)
{
    SourceLocation method_end_loc = Method->getNameInfo().getEndLoc();
    if (SM.isInMainFile(method_end_loc))
    {
        const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Установлен спецификатор override для переопределяющего метода");
        Diag.Report(Method->getLocation(), DiagID);
        // Method->getEndLoc() возвращает положение окончания тела метода (в случае, если мы имеем дело с её определением).
        // SourceLocation method_end_loc_2 = Method->getEndLoc();
        // const char* method_end_pos_2 = SM.getCharacterData(method_end_loc_2);
        //
        const char* method_end_pos = SM.getCharacterData(method_end_loc);
        size_t method_term_length = SM.getCharacterData(Method->getEndLoc()) - method_end_pos + 1;
        // На данный момент мы имеем указатель на символ, находящийся непосредственно после имени обрабатываемого метода. Далее нам требуется подобрать
        // место, подходящее для вставки спецификатора override. Это место должно располагаться после окончания блока формальных параметров метода,
        // а также после всех возможных суффиксов, которыми может быть снабжён этот метод.
        MethodCharm method_charm
            {
             .is_const_suffix = Method->isConst(),
             .is_final_attr = Method->hasAttr<clang::FinalAttr>(),
             .method_ref_kind = Method->getRefQualifier(),
             .spec_type = Method->getExceptionSpecType()
            };
        method_end_pos = SkipMethodParamsSuffixes(method_end_pos, method_term_length, method_charm);

        // Позиция предстоящей вставки спецификатора override найдена в method_end_pos.
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
    // Матчер обнаружения всех невиртуальных деструкторов. Дополнительный анализ на предмет того, есть ли у класса, для которого такой деструктор
    // определен, какие-либо производные от него классы, будет выполнен позже при завершении построения AST всей данной единицы трансляции.
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

auto AllDerivedClassesMatcher()
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
    Finder.addMatcher(AllDerivedClassesMatcher(), &Handler);
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
