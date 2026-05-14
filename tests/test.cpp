
#include <cstdlib>
#include <exception>
#include <gtest/gtest.h>
//
#include "test_helper.h"
#include "RefactorTool.h"
//
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <fstream>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
//
using namespace std::literals;
//
namespace fs = std::filesystem;

// Основной класс, в котором сосредоточены все базовые тестовые и проверочные операции, организующие запуск операции
// автоматического рефакторинга эталона и позволяющие убедиться в правильном её выполнении.
class TempRefactorFileData
{
public:
    static constexpr char FILE_REFACTOR_NAME[] = "file_to_refactor.cpp";
    static constexpr char REF_FILE_REFACTOR_NAME[] = "file_to_refactor_ref.cpp";
    static constexpr char REFACTOR_TOOL_EXECUTABLE[] = "./build/refactor_tool";

    struct RefactorLogPattern
    {
        int line_number;
        int symbol_number;
        DiagnosticsEngine::Level log_level;
    };

    TempRefactorFileData(const std::string& refactor_data)
    {
        m_refactor_filepath = fs::temp_directory_path() / fs::path(FILE_REFACTOR_NAME);
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
        m_refactor_ref_filepath = fs::temp_directory_path() / fs::path(REF_FILE_REFACTOR_NAME);
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
        MyGlobalEnvironment::ClearMessageLog();
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
        // Конструируем фабрику рефакторизирующих процессоров и Запускаем RefactorAction.
        return Tool.run(newFrontendActionFactory<CodeRefactorAction>().get());
    }

    void PrintFile(bool is_ref, std::ostream& ostr) const
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

    bool TestRefactorLog(std::initializer_list<RefactorLogPattern> test_list)
    {
        std::string check_log = MyGlobalEnvironment::GetMessageLog();
        std::initializer_list<RefactorLogPattern>::const_iterator scan_list_elem = test_list.begin();
        size_t scan_log_pos = 0;
        while (scan_log_pos < check_log.size() && scan_list_elem != test_list.end())
        {
            const RefactorLogPattern& ref_pattern = *scan_list_elem;
            std::string test_list_pattern = std::string(FILE_REFACTOR_NAME) + ':' + std::to_string(ref_pattern.line_number) + ':'
                                            + std::to_string(ref_pattern.symbol_number) + ": "s;
            switch (ref_pattern.log_level)
            {
                case DiagnosticsEngine::Level::Ignored:
                    test_list_pattern += "ignored"s;
                    break;
                case DiagnosticsEngine::Level::Note:
                    test_list_pattern += "note"s;
                    break;                    
                case DiagnosticsEngine::Level::Remark:
                    test_list_pattern += "remark"s;
                    break;
                case DiagnosticsEngine::Level::Warning:
                    test_list_pattern += "warning"s;
                    break;
                case DiagnosticsEngine::Level::Error:
                    test_list_pattern += "error"s;
                    break;
                case DiagnosticsEngine::Level::Fatal:
                    test_list_pattern += "fatal"s;
                    break;
                default:
                    break;
            }
            // Пробуем обнаружить образец test_list_pattern в теле лога check_log, начиная с текущей поисковой позиции scan_log_pos.
            size_t found_pattern_pos = check_log.find(test_list_pattern,  scan_log_pos);
            if (found_pattern_pos == std::string::npos)
                return false;   // Очередной требуемый элемент протокола не найден в его теле.
            
            scan_log_pos = found_pattern_pos + test_list_pattern.size();
            ++scan_list_elem;
        }
        return scan_list_elem == test_list.end();   // Возвращаем "ИСТИНУ", если все нужные позиционные термы обнаружены в тексте лога.
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
        // test_rabbit_1.PrintFile(false, std::cerr);
        // std::cerr << std::endl;
        // std::cerr << MyGlobalEnvironment::GetMessageLog() << std::endl;        
        //
        ASSERT_TRUE(test_rabbit_1.TestRefactorLog
            (
                {
                    {17, 10, DiagnosticsEngine::Level::Remark},
                    {19, 10, DiagnosticsEngine::Level::Remark},
                    {21, 5, DiagnosticsEngine::Level::Remark},
                    {28, 10, DiagnosticsEngine::Level::Remark},
                    {30, 10, DiagnosticsEngine::Level::Remark}
                }
            ));
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
        // test_rabbit_2.PrintFile(false, std::cerr);
        // std::cerr << std::endl;
        // std::cerr << MyGlobalEnvironment::GetMessageLog() << std::endl;
        // Так как никаких модифицирующих операций выполняться не должно, лог будет оставаться пустым.
        ASSERT_TRUE(MyGlobalEnvironment::GetMessageLog().empty());
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
        // test_rabbit_1.PrintFile(false, std::cerr);
        // std::cout << std::endl;
        // std::cerr << MyGlobalEnvironment::GetMessageLog() << std::endl;
        ASSERT_TRUE(test_rabbit_1.TestRefactorLog
            (
                {
                    {17, 21, DiagnosticsEngine::Level::Remark},
                    {23, 27, DiagnosticsEngine::Level::Remark},
                    {29, 44, DiagnosticsEngine::Level::Remark}
                }
            ));
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
        // test_rabbit_1.PrintFile(false, std::cerr);
        // std::cout << std::endl;
        // std::cerr << MyGlobalEnvironment::GetMessageLog() << std::endl;
        ASSERT_TRUE(test_rabbit_1.TestRefactorLog
            (
                {
                    {15, 5, DiagnosticsEngine::Level::Remark},
                    {6, 5, DiagnosticsEngine::Level::Remark}
                }
            ));
    }
    catch (const std::exception& excpt)
    {
        std::cerr << "Возникло исключение - " << excpt.what() << std::endl;
        ASSERT_TRUE(false);
    }
}

// Тест, демонстрирующий вставку модификатора override для функций, уже имеющих в своей сигнатуре суффиксы типа &, &&, const, final или noexcept.
TEST(ConstRefMethodsSuite, ConstRefMethodsOverrideTest)
{
    const std::string test_rabbit_body_1 = R"--(    
#include <string>

class Base
{
public:
    virtual void func_0()
    {}
    virtual void func_1() const
    {}
    virtual void func_1(int a) & = 0;
    virtual void func_1(double a) &&
    {}
    virtual void func_1(std::string a) const &
    {}
    virtual void func_2(int a) noexcept((bool)(2 + 3))
    {}
    virtual void func_2(float a) noexcept = 0;
    virtual void func_2(double a) &&
    {}
    virtual void func_2(std::string a) noexcept
    {}
    virtual ~Base()
    {}
};

class Derived : public Base
{
public:
    void func_0()                       // Переопределен без override
    {}
    void func_1() const                 // Переопределен без override
    {}
    void func_1(int a) &                // Переопределен без override
    {}
    void func_1(double a) &&            // Переопределен без override
    {}
    void func_1(std::string a) const &  // Переопределен без override
    {}
    void func_2(int a) noexcept((bool)(2 + 3))  // Переопределен без override
    {}
    void func_2(float a) noexcept               // Переопределен без override
    {}
    void func_2(double a) && final              // Переопределен без override
    {}
    void func_2(std::string a) noexcept final   // Переопределен без override
    {}    
    ~Derived()                         // Деструктор без override
    {}
}; 
    )--";

    try
    {
        TempRefactorFileData test_rabbit_1(test_rabbit_body_1);
        ASSERT_EQ(test_rabbit_1.ProcessTest(), 0);
        //
        // test_rabbit_1.PrintFile(false, std::cerr);
        // std::cout << std::endl;
        // std::cerr << MyGlobalEnvironment::GetMessageLog() << std::endl;
        //
        ASSERT_TRUE(test_rabbit_1.TestRefactorLog
            (
        {
                    {30, 10, DiagnosticsEngine::Level::Remark},
                    {32, 10, DiagnosticsEngine::Level::Remark},
                    {34, 10, DiagnosticsEngine::Level::Remark},
                    {36, 10, DiagnosticsEngine::Level::Remark},
                    {38, 10, DiagnosticsEngine::Level::Remark},
                    {40, 10, DiagnosticsEngine::Level::Remark},
                    {42, 10, DiagnosticsEngine::Level::Remark},
                    {44, 10, DiagnosticsEngine::Level::Remark},
                    {46, 10, DiagnosticsEngine::Level::Remark},
                    {48, 5, DiagnosticsEngine::Level::Remark}
                }
            ));
    }
    catch (const std::exception& excpt)
    {
        std::cerr << "Возникло исключение - " << excpt.what() << std::endl;
        ASSERT_TRUE(false);
    }    
}
