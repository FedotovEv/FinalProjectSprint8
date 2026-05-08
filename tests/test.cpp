
#include <cstdlib>
#include <exception>
#include <gtest/gtest.h>
//
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/CommandLine.h"
#include "../include/RefactorTool.h"
//
#include <filesystem>
#include <stdexcept>
#include <fstream>

static std::string REFACTOR_TOOL_EXECUTABLE = "./build/refactor_tool";

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
//
using namespace std::literals;
//
namespace fs = std::filesystem;

class TempRefactorFileData
{
public:
    TempRefactorFileData(const std::string& refactor_data)
    {
        m_refactor_filepath = fs::temp_directory_path() / fs::path("file_to_refactor.cpp"s);
        // Создадим тестовый файл в рекомендованном временном каталоге.
        std::ofstream ofstr(m_refactor_filepath);
        {
            ofstr.write(refactor_data.data(), refactor_data.size());
            if (!ofstr)
            {
                fs::remove(m_refactor_filepath);
                m_refactor_filepath.clear();
                throw std::runtime_error("Ошибка при создании тестового файла");
            }
        }
        // Создаём также копию созданного выше файла, которая далее будет служить как эталон для сравнения
        // результата автоматического рефакторинга.
        m_refactor_ref_filepath = fs::temp_directory_path() / fs::path("file_to_refactor_ref.cpp"s);
        try
        {
            fs::remove(m_refactor_ref_filepath);
            fs::copy_file(m_refactor_filepath, m_refactor_ref_filepath);
        }
        catch (const fs::filesystem_error& fs_err)
        {
            fs::remove(m_refactor_filepath);
            fs::remove(m_refactor_ref_filepath);
            throw std::runtime_error("Ошибка при создании тестового файла-эталона - "s + fs_err.what());
        }
    }

    int ProcessTest()
    {    
        // Сформируем специальный набор аргументов командной строки для передачи clang-инструментарию.
        std::vector<std::string> args_vector = ::testing::internal::GetArgvs();
        int test_argc = 2;
        const char *test_argv[3];
        test_argv[0] = args_vector[0].c_str();
        std::string refactor_filepath_str = m_refactor_filepath.string();
        test_argv[1] = refactor_filepath_str.c_str();
        test_argv[2] = nullptr;

        auto ExpectedParser = CommonOptionsParser::create(test_argc, test_argv, ToolCategory);
        if (!ExpectedParser)
        {
            llvm::errs() << ExpectedParser.takeError();
            err_what = std::runtime_error("Ошибка при создании разборщика командной строки");
            return EXIT_FAILURE;
        }
        // 
        CommonOptionsParser& OptionsParser = ExpectedParser.get();
        // Создаем ClangTool
        ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
        // Конструируем фабрику рефакторизирующих процессоров и получаем созданный в ней объект-рефакторизатор.
        std::unique_ptr<FrontendActionFactory> refactor_action_factory = newFrontendActionFactory<CodeRefactorAction>();
        // Запускаем RefactorAction.
        return Tool.run(refactor_action_factory.get());
    }

    void PrintFile(bool is_ref, std::ostream& ostr)
    {
        std::ifstream ifstr;
        if (!is_ref)
            ifstr.open(m_refactor_filepath);
        else
            ifstr.open(m_refactor_ref_filepath);

        while (ifstr)
        {
            std::string fileline;
            std::getline(ifstr, fileline);
            ostr << fileline << std::endl;
        }
    }

    ~TempRefactorFileData()
    {
        if (!m_refactor_filepath.empty())
            fs::remove(m_refactor_filepath);
        if (!m_refactor_ref_filepath.empty())
            fs::remove(m_refactor_ref_filepath);
    }

private:
    // Маршруты местоположения обрабатываемого и эталонного (исходного) файлов.
    fs::path m_refactor_filepath;
    fs::path m_refactor_ref_filepath;
    //
    std::optional<std::runtime_error> err_what;
};

// Тест проверки нормального функционирования операции автоматической вставки спецификатора override.
TEST(NoOverrideRefactorSuite, NoOverrideRefactorTest)
{
    const std::string test_rabbit_body_1 = R"--(
#include <string>

class Base
{
public:
    virtual void func()
    {}    
    virtual void func(int a) = 0;    
    virtual ~Base()
    {}
};

class Derived : public Base
{
public:
    void func()             // Переопределен без override
    {}
    void func(int a)        // Переопределен без override
    {}
    ~Derived()              // Деструктор без override
    {}
};

class SubDerived : public Derived
{
public:
    void func()             // Переопределен без override
    {}
    void func(int a)        // Переопределен без override
    {}
};
    )--";

        const std::string test_rabbit_body_2 = R"--(
class BaseWithOverride
{
public:
    virtual void func()
    {}
};

class DerivedWithOverride : public BaseWithOverride
{
public:
    void func() override    // Уже с override, не меняется
    {}
};
    )--";

    // Испытание основного положительного сценария операции - выполнение вставки спецификатора override в нужных местах.
    try
    {
        TempRefactorFileData test_rabbit_1(test_rabbit_body_1);
        ASSERT_EQ(test_rabbit_1.ProcessTest(), 0);
        //
        test_rabbit_1.PrintFile(false, std::cerr);
        std::cerr << std::endl;
    }
    catch (const std::exception& excpt)
    {
        std::cerr << "Возникло исключение - " << excpt.what() << std::endl;
        ASSERT_TRUE(false);
    }

    // Проверка главного отрицательного сценария - отказ от вставки повторного override при его наличии в оригинальном исходнике.
    try
    {
        TempRefactorFileData test_rabbit_2(test_rabbit_body_2);
        ASSERT_EQ(test_rabbit_2.ProcessTest(), 0);
        //
        test_rabbit_2.PrintFile(false, std::cerr);
        std::cerr << std::endl;
    }
    catch (const std::exception& excpt)
    {
        std::cerr << "Возникло исключение - " << excpt.what() << std::endl;
        ASSERT_TRUE(false);
    }
}

// Тест проверки работы ссылочной модификации константной переменной диапазонного цикла.
TEST(RangeLoopRefConstVarRefactorSuite, RangeLoopRefConstVarRefactorTest)
{
    const std::string test_rabbit_body_1 = R"--(
#include <iostream>
#include <string>
#include <vector>

struct CustomType
{
    int id;
    std::string name;
};

void process()
{
    std::vector<CustomType> vec = {{1, "a"}, {2, "b"}};

    // const auto без &
    for (const auto xy : vec)
    {
        std::cout << xy.id;
    }

    // const явный тип без &
    for (const CustomType xy : vec)
    {
        std::cout << xy.id;
    }

    // const decltype без &
    for (const decltype(vec)::value_type   xy : vec)
    {
        std::cout << xy.id;
    }

    // Фундаментальный тип, не меняется
    std::vector<int> ints = {1, 2};
    for (const int xy : ints)
    {
        std::cout << xy;
    }

    // Уже с &
    for (const auto& xy : vec)
    {
        std::cout << xy.id;
    }
}
    )--";

    // Комплексная проверка всех случаев срабатывания и несрабатывания вставки ссылочного модификатора для рабочей
    // переменной диапазонного цикла.
    try
    {
        TempRefactorFileData test_rabbit_1(test_rabbit_body_1);
        ASSERT_EQ(test_rabbit_1.ProcessTest(), 0);
        //
        test_rabbit_1.PrintFile(false, std::cerr);
        std::cout << std::endl;
    }
    catch (const std::exception& excpt)
    {
        std::cerr << "Возникло исключение - " << excpt.what() << std::endl;
        ASSERT_TRUE(false);
    }
}

// Проверка выполнения "виртуализации" (вставки спецификатора virtual) всех невиртуальных деструкторов каких-либо базовых
// классов (имеющих наследников).
TEST(NonVirtualDtorRefactorSuite, NonVirtualDtorRefactorTest)
{
    const std::string test_rabbit_body_1 = R"--(    
class BaseNoDtor
{
public:
    int x;
    ~BaseNoDtor() = default;
};

class DerivedFromNoDtor : public BaseNoDtor
{};

class BaseNonVirtual   // Базовый класс с невиртуальным деструктором.
{
public:
    ~BaseNonVirtual()  // Убедимся что вставка virtual выполняется однократно.
    {}
};

class DerivedFromNonVirtual : public BaseNonVirtual
{};

class DerivedFromNonVirtual1 : public DerivedFromNonVirtual
{};

// Класс с уже виртуальным деструктором, не меняется.
class BaseVirtual
{
public:
    virtual ~BaseVirtual() {}
};

class DerivedFromVirtual : public BaseVirtual
{};

// Класс без наследников, также не изменяется.
class Standalone
{
public:
    ~Standalone()
    {}
};
   )--";

    try
    {
        TempRefactorFileData test_rabbit_1(test_rabbit_body_1);
        ASSERT_EQ(test_rabbit_1.ProcessTest(), 0);
        //
        test_rabbit_1.PrintFile(false, std::cerr);
        std::cout << std::endl;
    }
    catch (const std::exception& excpt)
    {
        std::cerr << "Возникло исключение - " << excpt.what() << std::endl;
        ASSERT_TRUE(false);
    }
}
