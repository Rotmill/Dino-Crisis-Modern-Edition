cmake_minimum_required(VERSION 3.20)
project(DinoCrisis1Recomp C CXX)

# MSVC defaults to C++14 (no std::filesystem); the framework's config_loader
# needs C++17+. Build everything as C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# psxrecomp framework is the sibling directory. Установщик может задать другой
# путь через -DPSXRECOMP_ROOT=..., поэтому значение по умолчанию подставляется
# только когда переменная не задана снаружи.
if(NOT PSXRECOMP_ROOT)
    get_filename_component(PSXRECOMP_ROOT "${CMAKE_SOURCE_DIR}/../psxrecomp" ABSOLUTE)
endif()
set(PSXRECOMP_ROOT "${PSXRECOMP_ROOT}" CACHE PATH "Path to psxrecomp framework" FORCE)

# 17.09.2026 вечер: мод собирается БЕЗ отладочного сервера. Генератор Visual Studio многоконфигурационный, CMAKE_BUILD_TYPE
# у него пуст, и runtime.cmake включал PSX_DEBUG_TOOLS по умолчанию: релиз слушал порт 127.0.0.1:4472 (чтение и запись
# памяти игры, запись файлов по команде) и вёл отладочные кольца на горячем пути. Перехваты игры (быстрое сохранение,
# BG_INSTANT) в сборке без инструментов работают — проверено сборкой дева с -DPSX_DEBUG_TOOLS=OFF.
set(PSX_DEBUG_TOOLS OFF CACHE BOOL "Build with TCP debug server + heartbeat + per-block recording" FORCE)
# 10.10.2026: под MinGW (Clang из llvm-mingw - бесплатный компилятор, который едет в установщике) библиотеки C++ и раскрутки
# вшиваются в exe: рядом с игрой не нужны libc++.dll / libunwind.dll
if(MINGW)
    set(PSX_STATIC_RUNTIME ON CACHE BOOL "Statically link SDL2 + libgcc/libstdc++ for a self-contained exe" FORCE)
endif()
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

# Единственная цель мода — сборка со СВОИМ ядром PS1 (runtime/src/bios_hle.c):
# образ BIOS от Sony ей не нужен.
#
# У проекта разработки есть и вторая цель, psx-runtime, — она поднимает
# рекомпилированный ретейл-BIOS и требует generated/SCPH1001_full.c. Здесь её
# нет намеренно: этого файла у пользователя не будет, и одно только объявление
# такой цели роняет CMake на этапе конфигурации («Cannot find source file»).
psxrecomp_add_runtime_target(DinoCrisisME
    HLE_BIOS
    GAME_GENERATED_FULL_C     "${CMAKE_SOURCE_DIR}/generated/SLUS_009.22_full.c"
    GAME_GENERATED_DISPATCH_C "${CMAKE_SOURCE_DIR}/generated/SLUS_009.22_dispatch.c"
    WINDOW_TITLE              "Dino Crisis"
    DEBUG_PORT                4472
    DEFAULT_GAME_CONFIG_PATH  "game.toml"
)

# Отладочный интерфейс (Dear ImGui: окно диагностики F2, подгонки F8, света F9)
# из релиза убран целиком. Перекрестие на него больше не завязано — оно
# рисуется средствами SDL в runtime/src/dc_reticle.c, там же и экранные
# сообщения (dc_osd_text), а шрифт растеризуется оригинальным stb_truetype, а
# не копией из состава ImGui. Убрать это определение — вернутся и окна, и
# зависимость от библиотеки.
target_compile_definitions(DinoCrisisME PRIVATE MCM_NO_IMGUI)

# Трассировка пути (прежний режим F5) в мод не входит: картинку рисует
# растеризатор Vulkan, и он же был режимом по умолчанию всегда. Переключатель
# становится пустым (pt_renderer.c), шейдеры трассировки в поставку не кладутся.
#
# ⚠ Сам pt_vulkan.cpp остаётся в сборке, и это не недоделка: в нём лежит ОБЩЕЕ
# ядро Vulkan — создание устройства, блокировка GPU, загрузка видеопамяти, — на
# котором работает сам растеризатор (7 функций в gpu_vk_raster.cpp плюс одна в
# gpu.c). Трассировочная часть просто становится недостижимой.
target_compile_definitions(DinoCrisisME PRIVATE MCM_NO_PT)

# Значок программы. Ресурс с идентификатором 1 — это и значок файла в
# проводнике, и значок окна: SDL при регистрации класса окна берёт именно
# LoadIcon(hInstance, MAKEINTRESOURCE(1)).
#
# Файл .rc пишется в каталог сборки с АБСОЛЮТНЫМ путём к .ico: компилятор
# ресурсов ищет вложенные файлы относительно своей рабочей папки, а она у
# генератора Visual Studio — каталог сборки, а не каталог проекта.
#
# 17.09.2026 (игрок: «dc_user.cfg, input.ini, SLUS_009.22 оставляй, всё остальное зашивай в exe»): в том же .rc —
# ресурс DCME_DATA, файл dcme_data.pak. Его собирает установщик до сборки программы (mod/tools/pack_data.py): стартовые
# настройки dc_env.cfg, подгонка предметов, декалей, Z-fight и комнат камеры у бёдер, game.toml с путём к образу,
# тексты лицензий, камерная коллизия и комнаты, выгруженные из образа игрока. Читает runtime/src/dc_pak.c. Без него
# игра осталась бы без настроек и комнат — поэтому его отсутствие останавливает сборку.
set(DCME_ICON "${CMAKE_SOURCE_DIR}/dc_icon.ico")
set(DCME_PAK  "${CMAKE_SOURCE_DIR}/dcme_data.pak")
if(NOT EXISTS "${DCME_PAK}")
    message(FATAL_ERROR "dcme_data.pak not found next to CMakeLists.txt: setup builds it before the program (mod/tools/pack_data.py)")
endif()
if(MSVC)
    set(DCME_RC "")
    if(EXISTS "${DCME_ICON}")
        file(TO_NATIVE_PATH "${DCME_ICON}" DCME_ICON_NATIVE)
        string(REPLACE "\\" "\\\\" DCME_ICON_RC "${DCME_ICON_NATIVE}")
        string(APPEND DCME_RC "1 ICON \"${DCME_ICON_RC}\"\n")
    endif()
    file(TO_NATIVE_PATH "${DCME_PAK}" DCME_PAK_NATIVE)
    string(REPLACE "\\" "\\\\" DCME_PAK_RC "${DCME_PAK_NATIVE}")
    string(APPEND DCME_RC "DCME_DATA RCDATA \"${DCME_PAK_RC}\"\n")
    file(WRITE "${CMAKE_BINARY_DIR}/dc_icon.rc" "${DCME_RC}")
    target_sources(DinoCrisisME PRIVATE "${CMAKE_BINARY_DIR}/dc_icon.rc")
else()
    # 10.10.2026: тот же ресурс под MinGW (Clang) - его собирает windres из llvm-mingw; пути прямыми слэшами
    enable_language(RC)
    # 11.10.2026: пути в .rc - от каталога сборки (компилятор ресурсов ищет их от своей рабочей папки - это он и есть), чтобы в
    # тексте .rc не было папки игрока: кириллицу в строках .rc компилятор ресурсов без кодовой страницы не принимает
    file(RELATIVE_PATH DCME_ICON_REL "${CMAKE_BINARY_DIR}" "${DCME_ICON}")
    file(RELATIVE_PATH DCME_PAK_REL  "${CMAKE_BINARY_DIR}" "${DCME_PAK}")
    set(DCME_RC "")
    if(EXISTS "${DCME_ICON}")
        string(APPEND DCME_RC "1 ICON \"${DCME_ICON_REL}\"\n")
    endif()
    string(APPEND DCME_RC "DCME_DATA RCDATA \"${DCME_PAK_REL}\"\n")
    # 11.10.2026 (друг игрока: игра в «E:\Игра\...» не собиралась, а собранная спрашивала образ, лежащий в такой папке):
    # манифест «кодовая страница программы - UTF-8» (Windows 10 1903+). Пути с любыми буквами доходят до fopen, std::filesystem
    # и до чтения образа такими, какие они в game.toml (UTF-8), а не в кодовой странице системы, где кириллицы может не быть.
    if(EXISTS "${CMAKE_SOURCE_DIR}/dc_utf8.manifest")
        file(RELATIVE_PATH DCME_MAN_REL "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}/dc_utf8.manifest")
        string(APPEND DCME_RC "1 24 \"${DCME_MAN_REL}\"\n")
    endif()
    file(WRITE "${CMAKE_BINARY_DIR}/dc_icon.rc" "${DCME_RC}")
    target_sources(DinoCrisisME PRIVATE "${CMAKE_BINARY_DIR}/dc_icon.rc")
endif()

# Релиз без отладки (17.09.2026, игрок: «это релиз мода, тут не должно быть ни дебага, ни отладочных файлов»; из клавиш
# вне меню — «оставь только квиксейвы»): DCME_RELEASE вырезает отладочные клавиши (остаются полный экран F11/Alt+Enter и
# быстрые сохранения F10/Del/PgUp/PgDn), отчёты о падениях и зависаниях. Остальное — правки tools/release_edits.py.
target_compile_definitions(DinoCrisisME PRIVATE DCME_RELEASE)

# MSVC: windows.h defines min/max macros that collide with std::min/std::max
# used in the framework runtime (main.cpp). Suppress them for this target.
if(MSVC)
    target_compile_definitions(DinoCrisisME PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    # Без окна консоли. Каркас включает оконную подсистему только для
    # конфигурации Release, а мод собирается в RelWithDebInfo (нужны .pdb для
    # разбора падений) — там чёрное окно с логом вылезало поверх игры.
    #
    # /ENTRY обязателен: SDL2main здесь не линкуется, WinMain в программе нет,
    # точка входа так и остаётся main(). Журнала у релиза нет (17.09.2026):
    # dc_boot.c уводит и stdout, и stderr в NUL.
    target_link_options(DinoCrisisME PRIVATE
        $<$<CONFIG:RelWithDebInfo>:/SUBSYSTEM:WINDOWS>
        $<$<CONFIG:RelWithDebInfo>:/ENTRY:mainCRTStartup>)
else()
    # 10.10.2026: MinGW (Clang). То же самое его средствами: оконная подсистема без консоли (-mwindows, точка входа
    # остаётся main), SDL_MAIN_HANDLED (каркас ставит его только для MSVC), FMT_CONSTEVAL= - {fmt} 9.1 каркаса с
    # consteval нового Clang в C++20 не собирается (config_loader.cpp).
    target_compile_definitions(DinoCrisisME PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN SDL_MAIN_HANDLED "FMT_CONSTEVAL=")
    # чужой toml11 каркаса (comments.hpp) зовёт std::terminate без #include <exception> - MSVC тянул его сам
    target_compile_options(DinoCrisisME PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include exception>")
    target_link_options(DinoCrisisME PRIVATE -mwindows)
endif()
