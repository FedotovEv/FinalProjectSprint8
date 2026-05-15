#include <gtest/gtest.h>
#include "RefactorTool.h"
#include "test_helper.h"

std::string MyGlobalEnvironment::message_log;
std::unique_ptr<llvm::raw_string_ostream> MyGlobalEnvironment::log_stream;

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    // Регистрируем класс общих инициализирующих и завершающих операций данной тестирующей системы.
    ::testing::AddGlobalTestEnvironment(new MyGlobalEnvironment);
    return RUN_ALL_TESTS();
}
