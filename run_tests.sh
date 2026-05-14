#!/bin/bash

echo Запуск проверки устранения утечек памяти после автоматического рефакторинга
git checkout development
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCT_Clang_INSTALL_DIR=/usr/lib/llvm-20/
cmake --build .
cp -f leak_example.cpp leak_example_org.cpp   # Копируем исходный пример с ошибкой, приводящей к утечке памяти.
./refactor_tool leak_example_org.cpp		# Производим его рефакторинг, который должен эту ошибку устранить.
g++ leak_example_org.cpp -fsanitize=address -o leak_example_org  # Компиляция исправленного файла в чистилище.
./leak_example_org				# Запуск инструментированного файла для контроля отсутствия утечки.
