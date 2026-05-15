#pragma once

#include <gtest/gtest.h>
#include "RefactorTool.h"

class MyGlobalEnvironment : public ::testing::Environment
{
public:
    ~MyGlobalEnvironment() override
    {}

    // Общие однократно выполняемые инициализирующие действия для всей тестирующей среды.
    void SetUp() override
    {
        log_stream = std::make_unique<llvm::raw_string_ostream>(message_log);        
        CodeRefactorAction::SetLogParams(std::move(log_stream), false);
    }

    // Возможные общие окончательные действия для корректного завершения тестов.
    void TearDown() override
    {}

    static const std::string& GetMessageLog()
    {
        return message_log;
    }

    static void ClearMessageLog()
    {
        message_log.clear();
    }

private:
    // Статические Поля для обслуживания системы логирования процедуры автоматического рефакторинга тестовых примеров.
    static std::string message_log;
    static std::unique_ptr<llvm::raw_string_ostream> log_stream;
};
