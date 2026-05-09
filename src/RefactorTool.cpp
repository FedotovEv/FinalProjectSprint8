#include "RefactorTool.h"
#include <cstdlib>

int main(int argc, const char **argv)
{
    // Парсер опций: Обрабатывает флаги командной строки, компиляционные базы данных.
    // Дополняем инструментальную категорию ToolCategory собственными нестандартными параметрами - именем выходного файла протокола и желаемым
    // уровнем логирования, позволяющим впоследствии отделить отчёты о произведённых действиях от прочих чисто информационных сообщений.
    llvm::cl::opt<std::string> LogFilename
        ("log-file", llvm::cl::desc("Переадресация протокола работы в файл"), llvm::cl::value_desc("Имя файла"),
         llvm::cl::cat(ToolCategory));
    llvm::cl::opt<bool> LogActionsOnly
        ("log-act-only", llvm::cl::desc("Выводить в протокол только действия"), llvm::cl::init(false),
         llvm::cl::cat(ToolCategory));

    auto ExpectedParser = CommonOptionsParser::create(argc, argv, ToolCategory);
    if (!ExpectedParser)
    {
        llvm::errs() << ExpectedParser.takeError();
        return EXIT_FAILURE;
    }
    CommonOptionsParser& OptionsParser = ExpectedParser.get();
    // Проверяем наличие специальных опций командной строки, управляющих ведением протокола.
    std::unique_ptr<raw_ostream> log_stream;
    if (!LogFilename.empty())
    {
        std::error_code EC;
        log_stream = std::make_unique<llvm::raw_fd_ostream>(LogFilename.getValue(), EC, llvm::sys::fs::OF_Text);    
    }
    // Создаем ClangTool
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    // Выполним  некоторые предварительные установки для всех экземпляров класса CodeRefactorAction, которые будут порождаться при
    // анализе отдельных единиц трансляции.
    // llvm::errs() << "LogActionsOnly - " << LogActionsOnly.getValue() << '\n';
    CodeRefactorAction::SetLogParams(std::move(log_stream), !LogActionsOnly.getValue());
    // Наконец, запускаем главный рабочий процесс, который будет использовать настроенный выше фабричный класс CodeRefactorAction.
    return Tool.run(newFrontendActionFactory<CodeRefactorAction>().get());
}
