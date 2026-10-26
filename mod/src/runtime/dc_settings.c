/* dc_settings — навигация, файл настроек и отрисовка меню. См. dc_settings.h. */
#include "dc_settings.h"
#include "dc_font.h"
#include "dc_binds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

/* 10.10.2026: строка назначения DC_OPT_BIND: lo - действие DCB_*, hi - устройство; тип игры, чью раскладку правят, -
 * строка «Для типа игры» вверху подстраницы (dcb_edit_mode: при входе - текущий тип, переключается там же) */
#define BIND_DEV(o)  (((int)((o)->hi + 0.5f)) & 1)
#define BIND_MODE(o) ((void)(o), dcb_edit_mode())

static const DcOpt  *s_opts;
static int           s_count;
static const char  **s_pages;
static int           s_page_count;

/* ---- ЯЗЫК МЕНЮ (2026-09-01) --------------------------------------------
 * Игрок: «почему меню настроек сейчас на русском, всё остальное же на
 * английском». Основной язык теперь английский — таблица опций в main.cpp
 * объявлена по-английски, а русский получается переводом здесь. Так добавить
 * третий язык — это дописать колонку, а не переписывать таблицу.
 * Со временем сюда же придёт язык озвучки; пока только текст. */
int g_dc_menu_lang = 0;   /* ui.lang: 0 English (по умолчанию) ... 8 Italiano (en ru de es fr pt ja zh it) */

static const char *const s_tr[][2] = {
    /* вкладки */
    { "SCREEN", "ЭКРАН" }, { "CAMERA", "КАМЕРА" },
    { "CONTROLS", "УПРАВЛЕНИЕ" }, { "INTERFACE", "ИНТЕРФЕЙС" },
    /* опции */
    { "Language", "Язык" },
    /* подсказка пропуска катсцены */
    { "Skip", "Пропустить" },
    { "Skipping...", "Пропускаем..." },
    { "Skipped", "Пропущено" },
    { "SPACE", "ПРОБЕЛ" },
    { "Cutscene Skip", "Пропуск катсцен" },
    { "Any button shows the prompt; hold Start or Space for 2 s to skip a scene or movie",
      "Любая кнопка показывает подсказку; удержание Start или Пробела 2 с пропускает сцену или ролик" },
    { "Menu and interface language", "Язык меню и интерфейса" },
    /* экран «Раскладка управления» (dc_splash.cpp) */
    { "Controls Layout", "Раскладка управления" },
    { "Gamepad and keyboard buttons on one screen", "Кнопки геймпада и клавиатуры на одном экране" },
    { "Display Mode", "Режим отображения" },
    { "Windowed, borderless or fullscreen", "Окно, без рамки или во весь экран" },
    { "Resolution", "Разрешение экрана" },
    { "Window changes at once, render size on next launch",
      "Окно меняется сразу, разрешение картинки - со следующего запуска" },
    { "V-Sync", "Вертикальная синхронизация" },
    { "Locks the frame to the monitor, removing tearing",
      "Убирает разрывы картинки, привязывая кадр к монитору" },
    { "FPS Limit", "Ограничение FPS" },
    { "Upper bound on the frame rate", "Верхний предел частоты кадров" },
    { "Brightness", "Яркость" },
    { "Makes the whole picture lighter or darker", "Светлее или темнее общая картинка" },
    { "Camera Sensitivity", "Чувствительность камеры" },
    { "Look speed during normal movement", "Скорость обзора при обычном движении" },
    { "Aim Sensitivity", "Чувствительность при прицеливании" },
    { "Separate speed while the weapon is raised", "Отдельная скорость, когда оружие поднято" },
    { "Invert Horizontal", "Инверсия по горизонтали" },
    { "Look turns the opposite way", "Обзор поворачивается в обратную сторону" },
    { "Invert Vertical", "Инверсия по вертикали" },
    { "Same for looking up and down", "То же для наклона вверх-вниз" },
    { "Camera Distance", "Дальность камеры" },
    { "How far the camera sits behind Regina", "Насколько далеко камера стоит за Региной" },
    { "Control Type", "Тип управления" },
    { "Modern walks where the camera looks; classic is the original tank control",
      "Современное - идём куда смотрит камера; классическое - как в оригинале" },
    { "Swap Mouse Buttons", "Инверсия кнопок мыши" },
    { "Left becomes aim, right becomes fire", "Левая станет прицелом, правая - выстрелом" },
    { "Lock Cursor", "Фиксация курсора" },
    { "Cursor stays inside the window during play", "Курсор не выходит за пределы окна во время игры" },
    { "Stick Dead Zone", "Мёртвая зона" },
    { "How far to push the stick before the view moves (%)",
      "Насколько отклонить стик, чтобы обзор начал двигаться (%)" },
    { "Crosshair", "Прицел" },
    { "Crosshair at the centre while the weapon is raised",
      "Перекрестие в центре экрана, пока оружие поднято" },
    { "Shape of the aim marker at the centre of the screen",
      "Вид отметки прицела в центре экрана" },
    { "Shape of the aim marker: each weapon has its own, or one fixed shape",
      "Вид отметки прицела: своя у каждого оружия или одна на всё" },
    { "Per Weapon", "По оружию" },
    { "Damage Numbers", "Цифры урона" },
    { "Debug: shows the damage of every hit in the corner",
      "Отладка: показывает урон каждого попадания в углу экрана" },
    { "Crosshair On Target", "Прицел на цели" },
    { "Turns red when the shot would hit a creature",
      "Краснеет, когда выстрел попадёт по противнику" },
    { "Focus Aim", "Сведение прицела" },
    { "The crosshair settles while standing still; a wide one hits softer, firing opens it again",
      "Стоя на месте прицел сводится; разведённый бьёт слабее, выстрел разводит его снова" },
    { "Cross", "Перекрестие" }, { "Dot", "Точка" }, { "Red Cross", "Красное перекрестие" },
    { "Crosshair + Damage", "Прицел и урон" },
    /* вкладка GAME — родные страницы OPTION игры как подпункты (2026-09-02) */
    { "WEAPON AIM", "ПРИЦЕЛ ОРУЖИЯ" },
    { "Handgun: Tuning", "Пистолет: доводка" }, { "Handgun: Forward Tilt", "Пистолет: наклон вперёд" }, { "Handgun: Side Turn", "Пистолет: поворот вбок" },
    { "Shotgun: Tuning", "Дробовик: доводка" }, { "Shotgun: Forward Tilt", "Дробовик: наклон вперёд" }, { "Shotgun: Side Turn", "Дробовик: поворот вбок" },
    { "Grenade Gun: Tuning", "Гранатомёт: доводка" }, { "Grenade Gun: Forward Tilt", "Гранатомёт: наклон вперёд" }, { "Grenade Gun: Side Turn", "Гранатомёт: поворот вбок" },
    { "Our aim-pose adjustment for this weapon: raise blend, hand-over smoothing, offsets",
      "Наша доводка позы прицела для этого оружия: морф вскидывания, сглаживание стыка, поправки" },
    { "Turns the arms about the shoulder line: + raises the weapon (degrees)",
      "Поворот рук вокруг линии плеч: + поднимает оружие (градусы)" },
    { "Turns the arms about the vertical: + to the right (degrees)",
      "Поворот рук вокруг вертикали: + вправо (градусы)" },
    { "GAME", "ИГРА" },
    { "Key Config", "Настройка кнопок" },
    { "Button type, auto targeting, vibration: the game's own screen",
      "Тип кнопок, автоприцел, вибрация - экран самой игры" },
    { "Sound", "Звук" },
    { "BGM and SE volume, stereo or mono: the game's own screen",
      "Громкость музыки и эффектов, стерео или моно - экран самой игры" },
    { "Monitor Tuning", "Настройка монитора" },
    { "The game's colour-bar brightness test", "Тест яркости по цветным полосам - экран самой игры" },
    { "Reset", "Сброс" },
    { "Return to the title screen (the game asks to confirm)",
      "Вернуться на титульный экран (игра спросит подтверждение)" },
    { "Available in the game world only", "Доступно только в игре - не в меню и не в роликах" },
    { "Opening the game's screen...", "Открываю экран игры..." },
    /* титульные надписи игры (перерисовываются нашим шрифтом в VRAM, gpu.c);
     * ширина ячеек 80 / 88 / 56 / 128 px — перевод должен влезать */
    { "NEW GAME", "НОВАЯ ИГРА" }, { "LOAD GAME", "ЗАГРУЗИТЬ" },
    { "OPTION", "ОПЦИИ" }, { "PUSH START BUTTON", "НАЖМИТЕ START" },
    /* значения */
    { "Windowed", "Окно" }, { "Borderless", "Без рамки" }, { "Fullscreen", "Полный экран" },
    { "Unlimited", "Без ограничения" },
    { "Modern", "Современная" }, { "Tank", "Танковая" },
    { "English", "English" }, { "Russian", "Русский" },
    { "On", "Вкл" }, { "Off", "Выкл" },
    /* служебные строки */
    { "Applies after a restart", "Применится после перезапуска" },
    { "Press ENTER to apply", "Нажмите ENTER, чтобы применить" },
    { "Could not write dc_user.cfg", "Не удалось записать dc_user.cfg" },
    { "Settings saved", "Настройки сохранены" },
    { "Q/TAB tabs   arrows select   ESC exit", "Q/TAB вкладки   стрелки выбор   ESC выход" },
    { "TAB/Q tabs   arrows/WASD select   ENTER change   ESC back   |   L1/R1   Cross   Circle",
      "TAB/Q вкладки   стрелки/WASD выбор   ENTER изменить   ESC назад   |   L1/R1   Крест   Круг" },
};

/* ---- ВОСЕМЬ ЯЗЫКОВ (10.09.2026) ---------------------------------------------
 * Игрок: «выбираем язык — все наши меню, подписи, опции в моде переключаются
 * на этот язык; по умолчанию английский». Набор и порядок — как у установщика
 * мода. Здесь всё, что игрок видит: вкладки, пункты и подсказки меню, значения,
 * сообщения, строка управления, подсказка пропуска сцены. Колонки — ui.lang.
 * Таблица s_tr выше осталась запасной для русского: в ней ещё строки скрытых
 * страниц (доводка прицела, родное OPTION), которых игрок сейчас не видит.
 * Японский и китайский рисуются системным шрифтом Windows (dc_font.c).
 * 20.10.2026: девятый язык - итальянский, колонка it в конце строки (ui.lang 8). */

static const char *const s_tr8[][DC_LANG_COUNT] = {
    /* en, ru, de, es, fr, pt-BR, ja, zh, it */
    { "GAME", "ИГРА", "SPIEL", "JUEGO", "JEU", "JOGO", "ゲーム", "游戏", "GIOCO" },
    { "SCREEN", "ЭКРАН", "BILDSCHIRM", "PANTALLA", "ÉCRAN", "TELA", "画面", "画面", "SCHERMO" },
    { "CAMERA", "КАМЕРА", "KAMERA", "CÁMARA", "CAMÉRA", "CÂMERA", "カメラ", "镜头", "TELECAMERA" },
    { "CONTROLS", "УПРАВЛЕНИЕ", "STEUERUNG", "CONTROLES", "COMMANDES", "CONTROLES", "操作", "操作", "COMANDI" },
    { "Language", "Язык", "Sprache", "Idioma", "Langue", "Idioma", "言語", "语言", "Lingua" },
    { "Menu and interface language", "Язык меню и интерфейса", "Sprache von Menü und Oberfläche",
      "Idioma del menú y la interfaz", "Langue des menus et de l'interface", "Idioma do menu e da interface",
      "メニューとインターフェースの言語", "菜单和界面语言", "Lingua dei menu e dell'interfaccia" },
    { "Display Mode", "Режим отображения", "Anzeigemodus", "Modo de pantalla", "Mode d'affichage",
      "Modo de exibição", "表示モード", "显示模式", "Modalità schermo" },
    { "Windowed, borderless or fullscreen", "Окно, без рамки или во весь экран", "Fenster, randlos oder Vollbild",
      "Ventana, sin bordes o pantalla completa", "Fenêtré, sans bordure ou plein écran",
      "Janela, sem bordas ou tela cheia", "ウィンドウ、ボーダーレス、フルスクリーン", "窗口、无边框或全屏",
      "Finestra, senza bordi o schermo intero" },
    { "Window Size", "Размер окна", "Fenstergröße", "Tamaño de la ventana", "Taille de la fenêtre",
      "Tamanho da janela", "ウィンドウサイズ", "窗口大小", "Dimensioni finestra" },
    { "Size of the game window in windowed mode", "Размер окна игры в оконном режиме",
      "Größe des Spielfensters im Fenstermodus", "Tamaño de la ventana del juego en modo ventana",
      "Taille de la fenêtre du jeu en mode fenêtré", "Tamanho da janela do jogo no modo janela",
      "ウィンドウモードでのゲームウィンドウの大きさ", "窗口模式下游戏窗口的大小", "Dimensioni della finestra di gioco in modalità finestra" },
    /* 10.10.2026: разрешение рендера (main.cpp scr.render) */
    { "Render Resolution", "Разрешение рендера", "Renderauflösung", "Resolución de renderizado", "Résolution de rendu",
      "Resolução de renderização", "レンダリング解像度", "渲染分辨率", "Risoluzione di rendering" },
    { "Lines in the picture. 240p as on the original console; more is sharper but needs a stronger video card",
      "Сколько строк в картинке. 240p - как на оригинальной консоли; больше - чётче, но нужна видеокарта мощнее",
      "Zeilen im Bild. 240p wie auf der Originalkonsole; mehr ist schärfer, braucht aber eine stärkere Grafikkarte",
      "Líneas de la imagen. 240p como en la consola original; más es más nítido, pero exige una tarjeta gráfica más potente",
      "Lignes de l'image. 240p comme sur la console d'origine ; plus, c'est plus net mais demande une carte graphique plus puissante",
      "Linhas da imagem. 240p como no console original; mais é mais nítido, mas exige uma placa de vídeo mais forte",
      "画面の走査線の数。240pはオリジナルのゲーム機と同じ。多いほど鮮明ですが、高性能なグラフィックカードが必要です",
      "画面的行数。240p 与原版主机相同；越高越清晰，但需要更强的显卡",
      "Linee dell'immagine. 240p come sulla console originale; di più è più nitido, ma serve una scheda video più potente" },
    { "240p (PS1)", "240p (PS1)", "240p (PS1)", "240p (PS1)", "240p (PS1)", "240p (PS1)", "240p (PS1)", "240p (PS1)",
      "240p (PS1)" },
    { "480p", "480p", "480p", "480p", "480p", "480p", "480p", "480p", "480p" },
    { "720p", "720p", "720p", "720p", "720p", "720p", "720p", "720p", "720p" },
    { "960p", "960p", "960p", "960p", "960p", "960p", "960p", "960p", "960p" },
    { "1200p", "1200p", "1200p", "1200p", "1200p", "1200p", "1200p", "1200p", "1200p" },
    { "1440p (2K)", "1440p (2K)", "1440p (2K)", "1440p (2K)", "1440p (2K)", "1440p (2K)", "1440p (2K)", "1440p (2K)",
      "1440p (2K)" },
    { "1680p", "1680p", "1680p", "1680p", "1680p", "1680p", "1680p", "1680p", "1680p" },
    { "1920p", "1920p", "1920p", "1920p", "1920p", "1920p", "1920p", "1920p", "1920p" },
    { "2160p (4K)", "2160p (4K)", "2160p (4K)", "2160p (4K)", "2160p (4K)", "2160p (4K)", "2160p (4K)", "2160p (4K)",
      "2160p (4K)" },
    /* 15.09 ночь: соотношение сторон (вкладка SCREEN); 10.10.2026 - форма картинки, Auto - по окну, ультравайд */
    { "Aspect Ratio", "Соотношение сторон", "Seitenverhältnis", "Relación de aspecto", "Format d'image",
      "Proporção de tela", "アスペクト比", "画面比例", "Proporzioni" },
    { "Shape of the picture. Auto: the shape of your screen or window. 4:3: the whole game as on the original screen",
      "Форма картинки. Авто - по форме экрана или окна. 4:3 - вся игра как на экране оригинала",
      "Form des Bildes. Auto: wie Bildschirm oder Fenster. 4:3: das ganze Spiel wie im Original",
      "Forma de la imagen. Automático: la de tu pantalla o ventana. 4:3: todo el juego como en el original",
      "Forme de l'image. Auto : celle de l'écran ou de la fenêtre. 4:3 : tout le jeu comme à l'origine",
      "Formato da imagem. Automático: o da tela ou da janela. 4:3: o jogo inteiro como no original",
      "画面の形。自動:モニターまたはウィンドウに合わせる。4:3:ゲーム全体をオリジナル通りに表示",
      "画面形状。自动：与屏幕或窗口一致。4:3：整个游戏与原版一致",
      "Forma dell'immagine. Automatico: quella del tuo schermo o della finestra. 4:3: tutto il gioco come sullo schermo originale" },
    { "Stretch Interface", "Растянуть интерфейс", "Oberfläche strecken", "Estirar interfaz", "Étirer l'interface",
      "Esticar interface", "インターフェースを引き伸ばす", "拉伸界面", "Allarga interfaccia" },
    { "Menus, text and videos fill the whole width of a wide picture. Off: they keep the shape of the original screen",
      "Меню, текст и ролики на всю ширину широкой картинки. Выкл - в пропорциях экрана оригинала",
      "Menüs, Texte und Videos füllen die ganze Breite des breiten Bildes. Aus: sie behalten die Form des Originals",
      "Menús, textos y vídeos ocupan todo el ancho de la imagen ancha. No: conservan la forma de la pantalla original",
      "Menus, textes et vidéos occupent toute la largeur de l'image large. Non : ils gardent la forme de l'écran d'origine",
      "Menus, textos e vídeos ocupam toda a largura da imagem larga. Não: mantêm o formato da tela original",
      "メニュー、文字、ムービーをワイド画面の横幅いっぱいに表示。オフ:オリジナル画面の比率のまま",
      "菜单、文字和视频铺满宽画面的整个宽度。关：保持原版屏幕的比例",
      "Menu, testi e video occupano tutta la larghezza di un'immagine larga. No: mantengono la forma dello schermo originale" },
    { "V-Sync", "Вертикальная синхронизация", "V-Sync", "Sincronización vertical", "Synchro verticale",
      "Sincronização vertical", "垂直同期", "垂直同步", "Sincronizzazione verticale" },
    { "Locks the frame to the monitor, removing tearing", "Убирает разрывы картинки, привязывая кадр к монитору",
      "Koppelt das Bild an den Monitor und verhindert Tearing",
      "Sincroniza la imagen con el monitor y elimina el tearing",
      "Cale l'image sur l'écran et supprime le déchirement",
      "Sincroniza a imagem com o monitor e elimina o tearing",
      "フレームをモニターに同期させ、ティアリングを防ぎます", "将画面与显示器同步，消除画面撕裂",
      "Sincronizza i fotogrammi con il monitor ed elimina il tearing" },
    /* 10.10.2026: частота кадров 30 / 60 / 120 (main.cpp scr.fps) */
    { "Frame Rate", "Частота кадров", "Bildrate", "Velocidad de fotogramas", "Fréquence d'images", "Taxa de quadros",
      "フレームレート", "帧率", "Frequenza dei fotogrammi" },
    /* 24.10.2026: частоты выше 120 - подсказка про монитор в общем виде */
    { "How many times a second the picture is updated. 30 as on the original console; above 60 needs a monitor with at least as many Hz",
      "Сколько раз в секунду обновляется картинка. 30 - как на оригинальной консоли; для частоты выше 60 нужен монитор не меньшей частоты",
      "Wie oft pro Sekunde das Bild erneuert wird. 30 wie auf der Originalkonsole; über 60 braucht einen Monitor mit mindestens so vielen Hz",
      "Cuántas veces por segundo se actualiza la imagen. 30 como en la consola original; más de 60 requiere un monitor de al menos esos Hz",
      "Nombre de mises à jour de l'image par seconde. 30 comme sur la console d'origine ; au-delà de 60, il faut un écran d'au moins autant de Hz",
      "Quantas vezes por segundo a imagem é atualizada. 30 como no console original; acima de 60 exige um monitor de pelo menos tantos Hz",
      "1秒間に画面を更新する回数。30はオリジナルのゲーム機と同じ。60を超える値には、それ以上のHzのモニターが必要です",
      "画面每秒更新的次数。30 与原版主机相同；高于 60 需要刷新率不低于该值的显示器",
      "Quante volte al secondo si aggiorna l'immagine. 30 come sulla console originale; sopra 60 serve un monitor con almeno altrettanti Hz" },
    { "30 FPS", "30 FPS", "30 FPS", "30 FPS", "30 FPS", "30 FPS", "30 FPS", "30 FPS", "30 FPS" },
    { "60 FPS", "60 FPS", "60 FPS", "60 FPS", "60 FPS", "60 FPS", "60 FPS", "60 FPS", "60 FPS" },
    { "120 FPS", "120 FPS", "120 FPS", "120 FPS", "120 FPS", "120 FPS", "120 FPS", "120 FPS", "120 FPS" },
    { "144 FPS", "144 FPS", "144 FPS", "144 FPS", "144 FPS", "144 FPS", "144 FPS", "144 FPS", "144 FPS" },
    { "165 FPS", "165 FPS", "165 FPS", "165 FPS", "165 FPS", "165 FPS", "165 FPS", "165 FPS", "165 FPS" },
    { "240 FPS", "240 FPS", "240 FPS", "240 FPS", "240 FPS", "240 FPS", "240 FPS", "240 FPS", "240 FPS" },
    /* 25.10.2026 вечер: частоты выше 240 (main.cpp FR_N) */
    { "300 FPS", "300 FPS", "300 FPS", "300 FPS", "300 FPS", "300 FPS", "300 FPS", "300 FPS", "300 FPS" },
    { "360 FPS", "360 FPS", "360 FPS", "360 FPS", "360 FPS", "360 FPS", "360 FPS", "360 FPS", "360 FPS" },
    { "480 FPS", "480 FPS", "480 FPS", "480 FPS", "480 FPS", "480 FPS", "480 FPS", "480 FPS", "480 FPS" },
    { "1000 FPS", "1000 FPS", "1000 FPS", "1000 FPS", "1000 FPS", "1000 FPS", "1000 FPS", "1000 FPS", "1000 FPS" },
    { "75 FPS", "75 FPS", "75 FPS", "75 FPS", "75 FPS", "75 FPS", "75 FPS", "75 FPS", "75 FPS" },
    { "100 FPS", "100 FPS", "100 FPS", "100 FPS", "100 FPS", "100 FPS", "100 FPS", "100 FPS", "100 FPS" },
    { "180 FPS", "180 FPS", "180 FPS", "180 FPS", "180 FPS", "180 FPS", "180 FPS", "180 FPS", "180 FPS" },
    /* 24.10.2026: режим частоты кадров (main.cpp scr.frmode) */
    { "Frame Rate Mode", "Режим частоты кадров", "Bildratenmodus", "Modo de fotogramas", "Mode de fréquence", "Modo de quadros",
      "フレームレートモード", "帧率模式", "Modalità frequenza" },
    { "Fixed: exactly as many pictures a second as chosen, also when nothing moves. Dynamic: extra pictures only while the camera moves",
      "Фиксированный: ровно столько картинок в секунду, сколько выбрано, даже когда ничего не движется. Динамический: дополнительные картинки - только когда движется камера",
      "Fest: genau so viele Bilder pro Sekunde wie gewählt, auch wenn sich nichts bewegt. Dynamisch: zusätzliche Bilder nur, während sich die Kamera bewegt",
      "Fijo: exactamente tantas imágenes por segundo como las elegidas, también cuando nada se mueve. Dinámico: imágenes extra solo mientras la cámara se mueve",
      "Fixe : exactement autant d'images par seconde que choisi, même quand rien ne bouge. Dynamique : images supplémentaires seulement quand la caméra bouge",
      "Fixo: exatamente tantas imagens por segundo quanto o escolhido, mesmo quando nada se move. Dinâmico: imagens extras só enquanto a câmera se move",
      "固定：何も動いていないときも、選んだ回数どおりに画面を更新します。可変：カメラが動いているときだけ追加の画面を描きます",
      "固定：即使画面静止，也按所选次数每秒更新画面。动态：只在镜头移动时绘制额外画面",
      "Fissa: esattamente tante immagini al secondo quante scelte, anche quando nulla si muove. Dinamica: immagini extra solo mentre la telecamera si muove" },
    { "Fixed", "Фиксированный", "Fest", "Fijo", "Fixe", "Fixo", "固定", "固定", "Fissa" },
    { "Dynamic", "Динамический", "Dynamisch", "Dinámico", "Dynamique", "Dinâmico", "可変", "动态", "Dinamica" },
    /* 24.10.2026: счётчик кадров (main.cpp scr.fpsctr) */
    { "FPS Counter", "Счётчик кадров", "FPS-Anzeige", "Contador de FPS", "Compteur de FPS", "Contador de FPS",
      "FPSカウンター", "帧率显示", "Contatore FPS" },
    { "Shows how many pictures a second reach the screen, in its top left corner",
      "Показывает в левом верхнем углу экрана, сколько картинок в секунду на него выводится",
      "Zeigt oben links an, wie viele Bilder pro Sekunde auf dem Bildschirm ankommen",
      "Muestra en la esquina superior izquierda cuántas imágenes por segundo llegan a la pantalla",
      "Affiche en haut à gauche combien d'images par seconde arrivent à l'écran",
      "Mostra no canto superior esquerdo quantas imagens por segundo chegam à tela",
      "1秒間に画面に届く画像の数を左上に表示します",
      "在屏幕左上角显示每秒送到屏幕的画面数",
      "Mostra in alto a sinistra quante immagini al secondo arrivano sullo schermo" },
    { "Brightness", "Яркость", "Helligkeit", "Brillo", "Luminosité", "Brilho", "明るさ", "亮度", "Luminosità" },
    { "Makes the whole picture lighter or darker", "Светлее или темнее общая картинка",
      "Macht das ganze Bild heller oder dunkler", "Aclara u oscurece toda la imagen",
      "Éclaircit ou assombrit toute l'image", "Deixa a imagem inteira mais clara ou mais escura",
      "画面全体を明るく、または暗くします", "使整个画面更亮或更暗", "Schiarisce o scurisce tutta l'immagine" },
    { "Mouse Sensitivity", "Чувствительность мыши", "Mausempfindlichkeit", "Sensibilidad del ratón",
      "Sensibilité de la souris", "Sensibilidade do mouse", "マウス感度", "鼠标灵敏度", "Sensibilità del mouse" },
    { "How fast the mouse turns the camera", "Скорость поворота камеры мышью",
      "Wie schnell die Maus die Kamera dreht", "Velocidad con la que el ratón gira la cámara",
      "Vitesse à laquelle la souris tourne la caméra", "Velocidade com que o mouse gira a câmera",
      "マウスでカメラを回す速さ", "鼠标转动镜头的速度", "Velocità con cui il mouse ruota la telecamera" },
    { "Mouse Aim Sensitivity", "Чувствительность мыши в прицеле", "Mausempfindlichkeit beim Zielen",
      "Sensibilidad del ratón al apuntar", "Sensibilité de la souris en visée", "Sensibilidade do mouse ao mirar",
      "エイム時のマウス感度", "瞄准时鼠标灵敏度", "Sensibilità del mouse in mira" },
    { "Mouse speed while the weapon is raised", "Скорость мыши, пока оружие поднято",
      "Mausgeschwindigkeit bei gehobener Waffe", "Velocidad del ratón con el arma en alto",
      "Vitesse de la souris arme levée", "Velocidade do mouse com a arma erguida",
      "武器を構えている間のマウスの速さ", "举起武器时的鼠标速度", "Velocità del mouse con l'arma alzata" },
    { "Stick Sensitivity", "Чувствительность стика", "Stick-Empfindlichkeit", "Sensibilidad del stick",
      "Sensibilité du stick", "Sensibilidade do analógico", "スティック感度", "摇杆灵敏度", "Sensibilità dello stick" },
    { "How fast the right stick turns the camera", "Скорость поворота камеры правым стиком",
      "Wie schnell der rechte Stick die Kamera dreht", "Velocidad con la que el stick derecho gira la cámara",
      "Vitesse à laquelle le stick droit tourne la caméra", "Velocidade com que o analógico direito gira a câmera",
      "右スティックでカメラを回す速さ", "右摇杆转动镜头的速度", "Velocità con cui lo stick destro ruota la telecamera" },
    { "Stick Aim Sensitivity", "Чувствительность стика в прицеле", "Stick-Empfindlichkeit beim Zielen",
      "Sensibilidad del stick al apuntar", "Sensibilité du stick en visée", "Sensibilidade do analógico ao mirar",
      "エイム時のスティック感度", "瞄准时摇杆灵敏度", "Sensibilità dello stick in mira" },
    { "Right stick speed while the weapon is raised", "Скорость правого стика, пока оружие поднято",
      "Geschwindigkeit des rechten Sticks bei gehobener Waffe", "Velocidad del stick derecho con el arma en alto",
      "Vitesse du stick droit arme levée", "Velocidade do analógico direito com a arma erguida",
      "武器を構えている間の右スティックの速さ", "举起武器时的右摇杆速度", "Velocità dello stick destro con l'arma alzata" },
    { "Invert Horizontal", "Инверсия по горизонтали", "Horizontal invertieren", "Invertir eje horizontal",
      "Inverser l'axe horizontal", "Inverter eixo horizontal", "左右反転", "水平反转", "Inverti asse orizzontale" },
    { "The mouse and the right stick turn the camera left and right the other way",
      "Мышь и правый стик поворачивают камеру влево-вправо в обратную сторону",
      "Maus und rechter Stick drehen die Kamera nach links und rechts andersherum",
      "El ratón y el stick derecho giran la cámara a izquierda y derecha al revés",
      "La souris et le stick droit tournent la caméra à gauche et à droite dans l'autre sens",
      "O mouse e o analógico direito giram a câmera para os lados ao contrário",
      "マウスと右スティックでのカメラの左右の向きを反転します", "鼠标和右摇杆左右转动镜头的方向反转",
      "Il mouse e lo stick destro ruotano la telecamera a sinistra e a destra al contrario" },
    { "Invert Vertical", "Инверсия по вертикали", "Vertikal invertieren", "Invertir eje vertical",
      "Inverser l'axe vertical", "Inverter eixo vertical", "上下反転", "垂直反转", "Inverti asse verticale" },
    { "The mouse and the right stick tilt the camera up and down the other way",
      "Мышь и правый стик наклоняют камеру вверх-вниз в обратную сторону",
      "Maus und rechter Stick neigen die Kamera nach oben und unten andersherum",
      "El ratón y el stick derecho inclinan la cámara arriba y abajo al revés",
      "La souris et le stick droit inclinent la caméra en haut et en bas dans l'autre sens",
      "O mouse e o analógico direito inclinam a câmera para cima e para baixo ao contrário",
      "マウスと右スティックでのカメラの上下の向きを反転します", "鼠标和右摇杆上下转动镜头的方向反转",
      "Il mouse e lo stick destro inclinano la telecamera in alto e in basso al contrario" },
    { "Camera Distance", "Дальность камеры", "Kameraabstand", "Distancia de la cámara", "Distance de la caméra",
      "Distância da câmera", "カメラ距離", "镜头距离", "Distanza della telecamera" },
    { "How far the camera sits behind Regina", "Насколько далеко камера стоит за Региной",
      "Wie weit die Kamera hinter Regina ist", "A qué distancia queda la cámara detrás de Regina",
      "Distance entre la caméra et Regina", "A que distância a câmera fica atrás de Regina",
      "カメラがレジーナの後ろにどれだけ離れるか", "镜头在瑞吉娜身后多远", "A che distanza sta la telecamera dietro Regina" },
    /* 10.10.2026: смещение камеры вбок и по высоте (main.cpp cam.side, cam.up) */
    { "Camera Side Offset", "Смещение камеры вбок", "Kameraversatz seitlich", "Desplazamiento lateral de la cámara",
      "Décalage latéral de la caméra", "Deslocamento lateral da câmera", "カメラの左右オフセット", "镜头左右偏移",
      "Spostamento laterale telecamera" },
    { "Moves the camera to the left (minus) or right of Regina; at 0% she is in the middle",
      "Сдвигает камеру влево (минус) или вправо от Регины; при 0% она посередине",
      "Verschiebt die Kamera nach links (minus) oder rechts von Regina; bei 0% ist sie in der Mitte",
      "Mueve la cámara a la izquierda (menos) o a la derecha de Regina; con 0% queda en el centro",
      "Décale la caméra à gauche (moins) ou à droite de Regina ; à 0% elle est au centre",
      "Move a câmera para a esquerda (menos) ou para a direita de Regina; em 0% ela fica no centro",
      "カメラをレジーナの左（マイナス）または右へずらします。0% で中央になります",
      "将镜头移到瑞吉娜左侧（负值）或右侧；0% 时她在画面中央",
      "Sposta la telecamera a sinistra (meno) o a destra di Regina; con 0% lei è al centro" },
    { "Camera Height Offset", "Смещение камеры по высоте", "Kameraversatz in der Höhe", "Desplazamiento vertical de la cámara",
      "Décalage vertical de la caméra", "Deslocamento vertical da câmera", "カメラの上下オフセット", "镜头上下偏移",
      "Spostamento verticale telecamera" },
    { "Moves the camera up (plus) or down (minus); 0% is the usual height",
      "Поднимает (плюс) или опускает (минус) камеру; 0% - обычная высота",
      "Hebt (plus) oder senkt (minus) die Kamera; 0% ist die übliche Höhe",
      "Sube (más) o baja (menos) la cámara; 0% es la altura habitual",
      "Monte (plus) ou descend (moins) la caméra ; 0% est la hauteur habituelle",
      "Sobe (mais) ou desce (menos) a câmera; 0% é a altura normal",
      "カメラを上（プラス）または下（マイナス）へ動かします。0% が通常の高さです",
      "升高（正值）或降低（负值）镜头；0% 为默认高度", "Alza (più) o abbassa (meno) la telecamera; 0% è l'altezza normale" },
    /* 19.10.2026: отъезд камеры в прицеле и при беге (main.cpp cam.aimdist, cam.rundist) */
    { "Aim Camera Distance", "Дальность камеры в прицеле", "Kameraabstand beim Zielen", "Distancia de la cámara al apuntar",
      "Distance de la caméra en visée", "Distância da câmera ao mirar", "照準時のカメラ距離", "瞄准时的镜头距离",
      "Distanza telecamera in mira" },
    { "How far the camera sits behind Regina while she aims; 100% is the usual, at 0% it is at her head",
      "Насколько далеко камера стоит за Региной, когда она целится; 100% - как обычно, при 0% камера у её головы",
      "Wie weit die Kamera beim Zielen hinter Regina ist; 100% ist üblich, bei 0% ist sie an ihrem Kopf",
      "A qué distancia queda la cámara detrás de Regina al apuntar; 100% es lo habitual, con 0% queda junto a su cabeza",
      "Distance entre la caméra et Regina pendant la visée ; 100% est la valeur habituelle, à 0% elle est à sa tête",
      "A que distância a câmera fica atrás de Regina ao mirar; 100% é o normal, em 0% ela fica junto à cabeça dela",
      "照準中にカメラがレジーナの後ろにどれだけ離れるか。100% が通常、0% で頭の位置になります",
      "瞄准时镜头在瑞吉娜身后多远；100% 为默认，0% 时镜头在她头部",
      "A che distanza sta la telecamera dietro Regina quando mira; 100% è il valore normale, con 0% è vicino alla sua testa" },
    { "Run Camera Pull-Back", "Отъезд камеры при беге", "Kamera-Rückzug beim Rennen", "Alejamiento de la cámara al correr",
      "Recul de la caméra en course", "Afastamento da câmera ao correr", "走行時のカメラの引き", "奔跑时镜头后拉",
      "Arretramento telecamera in corsa" },
    { "How much further back the camera moves while Regina runs; 100% is the usual, at 0% it stays as when she walks",
      "Насколько дальше отъезжает камера, когда Регина бежит; 100% - как обычно, при 0% она остаётся как при ходьбе",
      "Wie weit die Kamera zurückweicht, wenn Regina rennt; 100% ist üblich, bei 0% bleibt sie wie beim Gehen",
      "Cuánto se aleja la cámara cuando Regina corre; 100% es lo habitual, con 0% se queda como al caminar",
      "De combien la caméra recule quand Regina court ; 100% est la valeur habituelle, à 0% elle reste comme à la marche",
      "Quanto a câmera se afasta quando Regina corre; 100% é o normal, em 0% ela fica como ao andar",
      "レジーナが走るときにカメラがどれだけ下がるか。100% が通常、0% で歩くときと同じ位置になります",
      "瑞吉娜奔跑时镜头后拉多少；100% 为默认，0% 时与行走时相同",
      "Quanto si allontana la telecamera quando Regina corre; 100% è il valore normale, con 0% resta come quando cammina" },
    /* 19.10.2026 вечер: CAMERA -> Hide Regina */
    { "Hide Regina", "Скрыть Регину", "Regina ausblenden", "Ocultar a Regina",
      "Masquer Regina", "Ocultar Regina", "レジーナを非表示", "隐藏瑞吉娜", "Nascondi Regina" },
    { "Regina, the weapon in her hand and her shadow are not drawn while you play; in cutscenes she is shown",
      "Во время игры не рисуются Регина, оружие в её руке и её тень; в катсценах она видна",
      "Regina, die Waffe in ihrer Hand und ihr Schatten werden beim Spielen nicht gezeichnet; in Zwischensequenzen ist sie zu sehen",
      "Regina, el arma en su mano y su sombra no se dibujan mientras juegas; en las escenas se muestra",
      "Regina, l'arme dans sa main et son ombre ne sont pas affichées pendant le jeu ; dans les cinématiques elle est visible",
      "Regina, a arma na mão dela e a sombra dela não são desenhadas durante o jogo; nas cenas ela aparece",
      "プレイ中はレジーナ、手に持つ武器、影を表示しません。カットシーンでは表示されます",
      "游戏中不绘制瑞吉娜、她手中的武器和她的影子；过场动画中她会显示",
      "Regina, l'arma che impugna e la sua ombra non vengono disegnate mentre giochi; nelle scene di intermezzo è visibile" },
    { "Controls Layout", "Раскладка управления", "Tastenbelegung", "Esquema de controles", "Schéma des commandes",
      "Esquema de controles", "操作方法", "按键布局", "Schema dei comandi" },
    { "Gamepad and keyboard buttons on one screen", "Кнопки геймпада и клавиатуры на одном экране",
      "Gamepad- und Tastaturbelegung auf einen Blick", "Botones del mando y del teclado en una pantalla",
      "Boutons de la manette et du clavier sur un seul écran", "Botões do controle e do teclado em uma tela",
      "ゲームパッドとキーボードの操作を一画面で", "手柄和键盘按键一览", "Tasti del controller e della tastiera in un'unica schermata" },
    /* 15.09 вечер: «Control Type» стал «Game Type» на вкладке GAME */
    { "Game Type", "Тип игры", "Spielart", "Tipo de juego", "Type de jeu", "Tipo de jogo", "ゲームタイプ", "游戏类型",
      "Tipo di gioco" },
    { "Modern: the camera follows over Regina's shoulder and she aims with the crosshair. Original: the fixed camera angles and the aiming of the original game",
      "Современный - камера из-за плеча, Регина целится по перекрестию. Оригинальный - родные ракурсы и прицеливание оригинальной игры",
      "Modern: Die Kamera folgt über Reginas Schulter, sie zielt mit dem Fadenkreuz. Original: die festen Kamerawinkel und das Zielen des Originalspiels",
      "Moderno: la cámara va sobre el hombro de Regina y ella apunta con la retícula. Original: los ángulos fijos y la puntería del juego original",
      "Moderne : la caméra suit Regina par-dessus l'épaule et elle vise avec le réticule. Original : les angles fixes et la visée du jeu d'origine",
      "Moderno: a câmera segue sobre o ombro de Regina e ela mira com a retícula. Original: os ângulos fixos e a mira do jogo original",
      "モダン:レジーナの肩越しにカメラが追い、照準で狙います。オリジナル:オリジナル版の固定カメラと狙い方",
      "现代：镜头越过瑞吉娜的肩膀跟随，她用准星瞄准。原版：原版游戏的固定镜头和瞄准方式",
      "Moderno: la telecamera segue Regina da sopra la spalla e lei mira con il mirino. Originale: le inquadrature fisse e la mira del gioco originale" },
    { "Camera",
      "По камере",
      "Kamera",
      "Cámara",
      "Caméra",
      "Câmera",
      "カメラ基準",
      "镜头方向", "Telecamera" },
    /* 21.10 вечер: значение строки «Movement Type» («Тип управления») - род мужской, как у «Современный» */
    { "Movement Type", "Тип управления", "Bewegungsart", "Tipo de movimiento", "Type de déplacement", "Tipo de movimento",
      "移動タイプ", "移动方式", "Tipo di movimento" },
    { "Tank",
      "Танковый",
      "Panzersteuerung",
      "Tanque",
      "Tank",
      "Tanque",
      "ラジコン操作",
      "坦克式", "Carro armato" },
    { "RE4",
      "RE4",
      "RE4",
      "RE4",
      "RE4",
      "RE4",
      "RE4",
      "RE4", "RE4" },
    { "Directions follow the camera: Regina goes where you push on the screen and turns quickly",
      "Направления - от камеры: Регина идёт туда, куда вы жмёте на экране, и быстро поворачивается",
      "Richtungen folgen der Kamera: Regina geht dorthin, wohin Sie auf dem Bildschirm drücken, und dreht sich schnell",
      "Las direcciones siguen a la cámara: Regina va hacia donde empujas en la pantalla y gira rápido",
      "Les directions suivent la caméra : Regina va là où vous poussez à l'écran et se tourne vite",
      "As direções seguem a câmera: Regina vai para onde você aponta na tela e vira rápido",
      "方向はカメラ基準:画面上で押した方向へレジーナが進み、素早く向きを変えます",
      "方向以镜头为准：按向屏幕上的哪个方向，瑞吉娜就走向哪里，并迅速转身",
      "Le direzioni seguono la telecamera: Regina va dove spingi sullo schermo e si gira rapidamente" },
    /* 20.10.2026: без фразы про камеру за спиной - в Modern + Tank она больше сама не заходит (main.cpp dc_tank_cam_follow) */
    { "As in the original game: up and down walk Regina forward and back, left and right turn her",
      "Как в оригинальной игре: вверх и вниз - Регина идёт вперёд и назад, влево и вправо - поворачивается",
      "Wie im Originalspiel: Hoch und runter lassen Regina vor- und zurückgehen, links und rechts drehen sie",
      "Como en el juego original: arriba y abajo hacen que Regina avance y retroceda, izquierda y derecha la giran",
      "Comme dans le jeu d'origine : haut et bas font avancer et reculer Regina, gauche et droite la font pivoter",
      "Como no jogo original: cima e baixo fazem Regina andar para frente e para trás, esquerda e direita a giram",
      "オリジナル版と同じ:上下で前進・後退、左右で向きを変えます",
      "与原版相同：上下键让瑞吉娜前进和后退，左右键让她转身",
      "Come nel gioco originale: su e giù fanno avanzare e indietreggiare Regina, sinistra e destra la fanno girare" },
    { "As in the original Resident Evil 4: tank controls with the camera always behind her, she aims standing still and the movement buttons move the aim, back with run turns her around",
      "Как в оригинальной Resident Evil 4: танковое управление и камера всегда за спиной, Регина целится стоя, а кнопки движения ведут прицел; назад с бегом - разворот",
      "Wie im originalen Resident Evil 4: Panzersteuerung mit der Kamera stets hinter ihr, sie zielt im Stehen und die Bewegungstasten führen das Zielen, zurück mit Laufen dreht sie um",
      "Como en el Resident Evil 4 original: control tipo tanque con la cámara siempre detrás, apunta quieta y los botones de movimiento mueven la mira; atrás con correr la hace girar",
      "Comme dans le Resident Evil 4 d'origine : commandes « tank », caméra toujours dans son dos, elle vise à l'arrêt et les touches de déplacement déplacent la visée ; arrière avec course la fait se retourner",
      "Como no Resident Evil 4 original: controle de tanque com a câmera sempre atrás dela, ela mira parada e os botões de movimento movem a mira; para trás com correr a faz dar meia-volta",
      "オリジナル版バイオハザード4と同じ:ラジコン操作でカメラは常に背後、構えると立ち止まり、移動ボタンで狙いを動かします。後ろ+ダッシュで振り向き",
      "与原版《生化危机4》相同：坦克式操作，镜头始终在她身后，举枪时站定，用移动键移动准星；后退加奔跑即可转身",
      "Come nel Resident Evil 4 originale: controlli a carro armato con la telecamera sempre dietro di lei, mira da ferma e i tasti di movimento spostano la mira; indietro più corsa la fa voltare" },
    { "Run with Left Stick", "Бег левым стиком", "Laufen mit linkem Stick", "Correr con el stick izquierdo",
      "Course au stick gauche", "Correr com o analógico esquerdo", "左スティックでダッシュ", "左摇杆奔跑",
      "Corsa con lo stick sinistro" },
    { "Clicking the left stick (L3): Toggle - Regina runs until she stops or raises the weapon; Hold - only while it is held down",
      "Нажатие левого стика (L3): Переключение - Регина бежит, пока не остановится или не поднимет оружие; Удержание - только пока стик нажат",
      "Klick auf den linken Stick (L3): Umschalten - Regina läuft, bis sie stehen bleibt oder die Waffe hebt; Halten - nur solange er gedrückt ist",
      "Pulsar el stick izquierdo (L3): Alternar - Regina corre hasta que se detiene o alza el arma; Mantener - solo mientras lo mantienes pulsado",
      "Clic du stick gauche (L3) : Bascule - Regina court jusqu'à s'arrêter ou lever son arme ; Maintien - seulement tant qu'il est enfoncé",
      "Clique do analógico esquerdo (L3): Alternar - Regina corre até parar ou erguer a arma; Segurar - só enquanto estiver pressionado",
      "左スティック押し込み(L3):切り替え - 止まるか武器を構えるまで走り続けます。長押し - 押している間だけ走ります",
      "按下左摇杆(L3)：切换 - 瑞吉娜持续奔跑，直到停下或举起武器；按住 - 仅在按住时奔跑",
      "Premendo lo stick sinistro (L3): Alterna - Regina corre finché non si ferma o alza l'arma; Tieni premuto - solo finché lo tieni premuto" },
    { "Mouse Look", "Обзор мышью", "Kamera per Maus", "Cámara con el ratón", "Caméra à la souris",
      "Câmera com o mouse", "マウスで視点操作", "鼠标视角", "Visuale col mouse" },
    { "On: the mouse turns the camera and the cursor is hidden while you play. Off: the cursor stays free - handy when playing with a gamepad",
      "Вкл - мышь поворачивает камеру, курсор во время игры скрыт. Выкл - курсор свободен: удобно, если играете на геймпаде",
      "An: Die Maus dreht die Kamera, der Mauszeiger ist beim Spielen ausgeblendet. Aus: Der Mauszeiger bleibt frei - praktisch beim Spielen mit Gamepad",
      "Sí: el ratón gira la cámara y el cursor se oculta durante la partida. No: el cursor queda libre, útil si juegas con mando",
      "Oui : la souris tourne la caméra et le curseur est masqué pendant la partie. Non : le curseur reste libre, pratique pour jouer à la manette",
      "Sim: o mouse gira a câmera e o cursor fica oculto durante o jogo. Não: o cursor fica livre, útil ao jogar com controle",
      "オン:マウスでカメラを回し、プレイ中はカーソルを隠します。オフ:カーソルは自由なまま。ゲームパッドで遊ぶときに便利です",
      "开：用鼠标转动镜头，游戏时隐藏光标。关：光标保持自由，适合用手柄游玩",
      "Sì: il mouse ruota la telecamera e il cursore è nascosto mentre giochi. No: il cursore resta libero, comodo se giochi con un controller" },
    { "Stick Dead Zone", "Мёртвая зона стика", "Stick-Totzone", "Zona muerta del stick", "Zone morte du stick",
      "Zona morta do analógico", "スティックのデッドゾーン", "摇杆死区", "Zona morta dello stick" },
    { "How far to tilt the right stick before the camera starts to move",
      "Насколько отклонить правый стик, прежде чем камера начнёт поворачиваться",
      "Wie weit der rechte Stick geneigt werden muss, bevor sich die Kamera dreht",
      "Cuánto inclinar el stick derecho antes de que la cámara empiece a girar",
      "Inclinaison du stick droit nécessaire avant que la caméra ne commence à tourner",
      "Quanto inclinar o analógico direito antes de a câmera começar a girar",
      "カメラが回り始めるまでに右スティックを倒す量", "镜头开始转动前右摇杆需推动的幅度",
      "Quanto inclinare lo stick destro prima che la telecamera inizi a muoversi" },
    /* 15.09 вечер: «Cutscene Skip» убран из меню (пропуск всегда включён); «Door Transitions»
     * стал «Door Transitions Skip» (Вкл = пропуск) */
    { "Door Transitions Skip", "Пропуск сцен дверей", "Türsequenzen überspringen", "Omitir transiciones de puertas",
      "Passer les transitions des portes", "Pular transições de portas", "ドア演出スキップ", "跳过开门过场",
      "Salta transizioni delle porte" },
    { "On: the next room loads right away. Off: the door-opening scenes play as in the original",
      "Вкл - следующая комната загружается сразу. Выкл - сцены открытия дверей идут, как в оригинале",
      "An: Der nächste Raum lädt sofort. Aus: Die Türöffnungsszenen laufen wie im Original",
      "Sí: la siguiente sala carga al instante. No: las escenas de puertas se reproducen como en el original",
      "Oui : la pièce suivante se charge aussitôt. Non : les scènes d'ouverture de porte se jouent comme dans l'original",
      "Sim: a próxima sala carrega na hora. Não: as cenas de abrir portas são exibidas como no original",
      "オン:次の部屋をすぐ読み込みます。オフ:オリジナルと同じくドアを開ける演出を再生します",
      "开：立即载入下一个房间。关：像原版一样播放开门过场",
      "Sì: la stanza successiva si carica subito. No: le scene delle porte vengono riprodotte come nell'originale" },
    { "Crosshair", "Прицел", "Fadenkreuz", "Retícula", "Réticule", "Mira", "照準", "准星", "Mirino" },
    /* 23.09: вид предметов (вкладка GAME) */
    { "Item View", "Вид предметов", "Gegenstände", "Vista de objetos", "Affichage des objets", "Exibição de itens",
      "アイテム表示", "道具显示", "Vista degli oggetti" },
    { "Modern: pickups lie still on the floor. Original: they spin and sit where the original game puts them",
      "Современный - предметы лежат неподвижно на полу. Оригинальный - крутятся и стоят там, где их ставит оригинальная игра",
      "Modern: Gegenstände liegen ruhig auf dem Boden. Original: Sie drehen sich und stehen dort, wo das Originalspiel sie platziert",
      "Moderno: los objetos quedan quietos en el suelo. Original: giran y se colocan donde los pone el juego original",
      "Moderne : les objets restent immobiles au sol. Original : ils tournent et se placent comme dans le jeu d'origine",
      "Moderno: os itens ficam parados no chão. Original: giram e ficam onde o jogo original os coloca",
      "モダン:アイテムは床に静止して置かれます。オリジナル:オリジナル版と同じく回転し、元の位置に置かれます",
      "现代：道具静止地放在地面上。原版：像原版游戏一样旋转并放在原来的位置",
      "Moderno: gli oggetti restano fermi sul pavimento. Originale: ruotano e stanno dove li mette il gioco originale" },
    /* 12.10: подсветка предметов (вкладка GAME) */
    { "Item Highlight", "Подсветка предметов", "Gegenstände hervorheben", "Resaltar objetos", "Surbrillance des objets",
      "Destacar itens", "アイテムの光", "道具高亮", "Evidenzia oggetti" },
    { "Every few seconds a light sweeps over the items you can pick up, so they are easier to find. Off: items look as in the original game",
      "Раз в несколько секунд по предметам, которые можно подобрать, пробегает блик - их легче заметить. Выкл - предметы как в оригинальной игре",
      "Alle paar Sekunden gleitet ein Lichtschimmer über Gegenstände, die man aufheben kann - so findet man sie leichter. Aus: Gegenstände wie im Originalspiel",
      "Cada pocos segundos un destello recorre los objetos que se pueden recoger, para encontrarlos con facilidad. No: objetos como en el juego original",
      "Toutes les quelques secondes, un reflet balaie les objets à ramasser pour qu'on les repère plus facilement. Non : objets comme dans le jeu d'origine",
      "A cada poucos segundos um brilho passa pelos itens que podem ser pegos, para encontrá-los com facilidade. Não: itens como no jogo original",
      "数秒ごとに拾えるアイテムの上を光が走り、見つけやすくなります。オフ:オリジナル版と同じ見た目",
      "每隔几秒，可拾取的道具上会掠过一道光，更容易发现。关：道具与原版游戏相同",
      "Ogni pochi secondi un bagliore passa sugli oggetti raccoglibili, così si trovano più facilmente. No: oggetti come nel gioco originale" },
    /* 13.10.2026 ночь: GAME -> Aim Walk (ходьба в прицеле) - пояснения режимов 1..5 (ch_aim_walk_hint, main.cpp) */
    { "Aim Walk",
      "Ходьба в прицеле",
      "Gehen beim Zielen",
      "Caminar apuntando",
      "Marche en visant",
      "Andar mirando",
      "構えたまま移動",
      "瞄准时移动", "Camminata in mira" },
    { "Regina turns to face where she walks, the weapon raised",
      "Регина поворачивается туда, куда идёт, с поднятым оружием",
      "Regina dreht sich in Laufrichtung, die Waffe bleibt erhoben",
      "Regina se gira hacia donde camina, con el arma en alto",
      "Regina se tourne vers là où elle marche, arme levée",
      "Regina se vira para onde anda, com a arma erguida",
      "レジーナは武器を構えたまま、歩く方向へ向きを変えます",
      "瑞吉娜举着武器，转身朝行走方向前进", "Regina si gira verso dove cammina, con l'arma alzata" },
    { "Strafe: she keeps aiming at the crosshair and steps in any direction",
      "Стрейф: она целится в перекрестие и шагает в любую сторону",
      "Seitwärtsgehen: Sie zielt weiter aufs Fadenkreuz und geht in jede Richtung",
      "Desplazamiento lateral: sigue apuntando a la mira y se mueve en cualquier dirección",
      "Pas latéraux : elle vise toujours le réticule et se déplace dans toutes les directions",
      "Movimento lateral: ela continua mirando na mira e anda em qualquer direção",
      "ストレイフ：照準を狙ったまま、どの方向にも歩きます",
      "横移：始终瞄准准星，可向任意方向移动", "Spostamento laterale: continua a mirare al mirino e si muove in qualsiasi direzione" },
    { "Strafe, and on a side step the body turns a little toward it",
      "Стрейф, а при шаге вбок корпус немного поворачивается к шагу",
      "Seitwärtsgehen, bei einem Schritt zur Seite dreht sich der Körper etwas mit",
      "Desplazamiento lateral; al dar un paso al lado, el cuerpo gira un poco hacia él",
      "Pas latéraux ; sur un pas de côté, le corps se tourne un peu vers lui",
      "Movimento lateral; ao dar um passo para o lado, o corpo gira um pouco para ele",
      "ストレイフ。横に歩くと体が少しその方向へ向きます",
      "横移；向侧面迈步时身体会稍微转向该侧", "Spostamento laterale; con un passo di lato il corpo si gira un po' verso di esso" },
    { "Strafe with the body turned to the step",
      "Стрейф: корпус поворачивается к шагу",
      "Seitwärtsgehen mit dem Körper zur Schrittrichtung",
      "Desplazamiento lateral con el cuerpo hacia el paso",
      "Pas latéraux, le corps tourné vers le pas",
      "Movimento lateral com o corpo virado para o passo",
      "ストレイフ。体が歩く方向へ向きます",
      "横移：身体转向迈步方向", "Spostamento laterale con il corpo girato verso il passo" },
    { "As 4, and while walking the camera is the usual one, not the aiming one",
      "Как 4, а на ходу камера обычная, как без прицела",
      "Wie 4, beim Gehen ist die Kamera die normale, nicht die Zielkamera",
      "Como 4, y al caminar la cámara es la normal, no la de apuntar",
      "Comme 4, et en marchant la caméra est la normale, pas celle de visée",
      "Como 4, e ao andar a câmera é a normal, não a de mira",
      "4と同じですが、歩行中は構えていない時と同じ通常のカメラです",
      "同4，但行走时使用普通镜头，而非瞄准镜头", "Come 4, e camminando la telecamera è quella normale, non quella di mira" },
    { "Crosshair While Walking",
      "Перекрестие на ходу",
      "Fadenkreuz beim Gehen",
      "Retícula al caminar",
      "Réticule en marchant",
      "Mira ao andar",
      "移動中の照準",
      "移动时的准星", "Mirino in movimento" },
    { "On: the crosshair stays on the screen while Regina walks with the weapon raised. Off: it is hidden while she walks and comes back when she stops",
      "Вкл - перекрестие видно и когда Регина идёт с поднятым оружием. Выкл - на ходу оно скрыто и возвращается, как только она остановится",
      "An: Das Fadenkreuz bleibt sichtbar, während Regina mit erhobener Waffe geht. Aus: Beim Gehen ist es ausgeblendet und kehrt zurück, sobald sie stehen bleibt",
      "Sí: la retícula sigue en pantalla mientras Regina camina con el arma en alto. No: se oculta al caminar y vuelve en cuanto se detiene",
      "Oui : le réticule reste à l'écran pendant que Regina marche arme levée. Non : il disparaît pendant la marche et revient dès qu'elle s'arrête",
      "Sim: a mira continua na tela enquanto Regina anda com a arma erguida. Não: ela some ao andar e volta assim que Regina para",
      "オン:武器を構えて歩いている間も照準を表示します。オフ:歩いている間は隠れ、止まると戻ります",
      "开：瑞吉娜举枪行走时准星仍显示在屏幕上。关：行走时隐藏，停下后重新出现",
      "Sì: il mirino resta sullo schermo mentre Regina cammina con l'arma alzata. No: si nasconde mentre cammina e torna quando si ferma" },
    /* 27.09: анимация лиц (вкладка GAME) */
    { "Face Animation", "Анимация лиц", "Gesichtsanimation", "Animación facial", "Animation des visages", "Animação facial",
      "顔のアニメーション", "面部动画", "Animazione facciale" },
    { "On: characters blink, look around, move their lips to the voice and show emotions in dialogue. Off: faces stay as in the original game",
      "Вкл - персонажи моргают, смотрят по сторонам, двигают губами под голос и показывают эмоции в диалогах. Выкл - лица как в оригинале",
      "An: Figuren blinzeln, schauen umher, bewegen die Lippen zur Stimme und zeigen Gefühle in Dialogen. Aus: Gesichter wie im Original",
      "Sí: los personajes parpadean, miran alrededor, mueven los labios con la voz y muestran emociones en los diálogos. No: caras como en el original",
      "Oui : les personnages clignent des yeux, regardent autour d'eux, bougent les lèvres avec la voix et montrent leurs émotions. Non : visages comme dans l'original",
      "Sim: os personagens piscam, olham ao redor, mexem os lábios com a voz e mostram emoções nos diálogos. Não: rostos como no original",
      "オン:会話中にキャラクターがまばたきし、視線を動かし、声に合わせて口を動かし、感情を表します。オフ:オリジナルと同じ顔",
      "开：对话时角色会眨眼、环顾四周、随语音动嘴并表现情绪。关：面部与原版相同",
      "Sì: i personaggi sbattono le palpebre, si guardano intorno, muovono le labbra con la voce e mostrano emozioni nei dialoghi. No: volti come nel gioco originale" },
    { "Shape of the aim marker: each weapon has its own, or one fixed shape",
      "Вид отметки прицела: своя у каждого оружия или одна на всё",
      "Form der Zielmarkierung: je Waffe eigene oder eine feste Form",
      "Forma de la marca de apuntado: una por arma o una fija",
      "Forme du repère de visée : propre à chaque arme ou unique",
      "Formato da marca de mira: um por arma ou um fixo",
      "照準マークの形:武器ごと、または固定", "瞄准标记的形状：每种武器各一种或统一形状",
      "Forma del segno di mira: ogni arma ha la sua, oppure una forma fissa" },
    /* 25.09 ночь: вкладка GRAPHICS — тени, материалы, сглаживание, затенение */
    { "GRAPHICS", "ГРАФИКА", "GRAFIK", "GRÁFICOS", "GRAPHISMES", "GRÁFICOS", "グラフィック", "图形", "GRAFICA" },
    { "Shadows", "Тени", "Schatten", "Sombras", "Ombres", "Sombras", "影", "阴影", "Ombre" },
    { "Soft shadows from the room's lamps under Regina and the creatures. Off: the round shadow spots of the original game",
      "Мягкие тени от ламп комнаты под Региной и существами. Выкл - круглые пятна-тени оригинальной игры",
      "Weiche Schatten der Raumlampen unter Regina und den Kreaturen. Aus: die runden Schattenflecken des Originalspiels",
      "Sombras suaves de las lámparas de la sala bajo Regina y las criaturas. No: las manchas de sombra redondas del juego original",
      "Ombres douces des lampes de la pièce sous Regina et les créatures. Non : les taches d'ombre rondes du jeu d'origine",
      "Sombras suaves das lâmpadas da sala sob Regina e as criaturas. Não: as manchas de sombra redondas do jogo original",
      "部屋のライトによるレジーナやクリーチャーの柔らかい影。オフ:オリジナル版の丸い影",
      "房间灯光在瑞吉娜和生物身下投出的柔和阴影。关：原版游戏的圆形阴影",
      "Ombre morbide delle lampade della stanza sotto Regina e le creature. No: le macchie d'ombra rotonde del gioco originale" },
    { "Materials", "Материалы", "Materialien", "Materiales", "Matériaux", "Materiais", "マテリアル", "材质", "Materiali" },
    { "Reflections on floors, highlights on metal and tile, glowing screens and lamps with their light. Off: surfaces look as in the original game",
      "Отражения на полу, блики на металле и кафеле, светящиеся экраны и лампы с их светом. Выкл - поверхности как в оригинальной игре",
      "Spiegelungen auf Böden, Glanzlichter auf Metall und Fliesen, leuchtende Bildschirme und Lampen mit ihrem Licht. Aus: Oberflächen wie im Originalspiel",
      "Reflejos en el suelo, brillos en metal y azulejos, pantallas y lámparas que brillan con su luz. No: superficies como en el juego original",
      "Reflets sur le sol, éclats sur le métal et le carrelage, écrans et lampes lumineux avec leur lumière. Non : surfaces comme dans le jeu d'origine",
      "Reflexos no chão, brilhos em metal e azulejos, telas e lâmpadas que brilham com sua luz. Não: superfícies como no jogo original",
      "床の反射、金属やタイルのハイライト、光る画面やライトとその光。オフ:オリジナル版と同じ見た目",
      "地面反射、金属和瓷砖上的高光、发光的屏幕和灯及其光照。关：表面与原版游戏相同",
      "Riflessi sui pavimenti, lucentezza su metallo e piastrelle, schermi e lampade luminosi con la loro luce. No: superfici come nel gioco originale" },
    { "Anti-Aliasing", "Сглаживание", "Kantenglättung", "Antialiasing", "Anticrénelage", "Anti-aliasing", "アンチエイリアス", "抗锯齿",
      "Anti-aliasing" },
    /* 25.10.2026: + FXAA / SMAA / CMAA2 в меню сглаживания */
    { "Smooths jagged edges. MSAA is the cleanest and takes the most video card time; FXAA, SMAA and CMAA2 are lighter filters of the finished picture",
      "Сглаживает ступенчатые края. MSAA - самое чистое и больше всего нагружает видеокарту; FXAA, SMAA и CMAA2 - лёгкие фильтры готовой картинки",
      "Glättet gezackte Kanten. MSAA ist am saubersten und kostet die meiste Grafikkarten-Leistung; FXAA, SMAA und CMAA2 sind leichtere Filter des fertigen Bildes",
      "Suaviza los bordes dentados. MSAA es el más limpio y el que más exige a la tarjeta gráfica; FXAA, SMAA y CMAA2 son filtros más ligeros sobre la imagen final",
      "Adoucit les bords crénelés. Le MSAA est le plus net et sollicite le plus la carte graphique ; FXAA, SMAA et CMAA2 sont des filtres plus légers appliqués à l'image finale",
      "Suaviza as bordas serrilhadas. O MSAA é o mais limpo e o que mais exige da placa de vídeo; FXAA, SMAA e CMAA2 são filtros mais leves sobre a imagem final",
      "ギザギザの縁をなめらかにします。MSAAは最もきれいですがビデオカードの負荷が最も大きく、FXAA・SMAA・CMAA2は完成した画像にかける軽いフィルターです",
      "平滑锯齿边缘。MSAA 效果最干净，显卡负担也最大；FXAA、SMAA 和 CMAA2 是作用于最终画面的较轻滤镜",
      "Smussa i bordi seghettati. L'MSAA è il più pulito e impegna di più la scheda video; FXAA, SMAA e CMAA2 sono filtri più leggeri sull'immagine finale" },
    /* значения сглаживания - названия технологий, во всех языках одинаковые */
    { "MSAA 2x", "MSAA 2x", "MSAA 2x", "MSAA 2x", "MSAA 2x", "MSAA 2x", "MSAA 2x", "MSAA 2x", "MSAA 2x" },
    { "MSAA 4x", "MSAA 4x", "MSAA 4x", "MSAA 4x", "MSAA 4x", "MSAA 4x", "MSAA 4x", "MSAA 4x", "MSAA 4x" },
    { "MSAA 8x", "MSAA 8x", "MSAA 8x", "MSAA 8x", "MSAA 8x", "MSAA 8x", "MSAA 8x", "MSAA 8x", "MSAA 8x" },
    { "FXAA", "FXAA", "FXAA", "FXAA", "FXAA", "FXAA", "FXAA", "FXAA", "FXAA" },
    { "SMAA", "SMAA", "SMAA", "SMAA", "SMAA", "SMAA", "SMAA", "SMAA", "SMAA" },
    { "CMAA2", "CMAA2", "CMAA2", "CMAA2", "CMAA2", "CMAA2", "CMAA2", "CMAA2", "CMAA2" },
    { "Ambient Occlusion", "Затенение", "Umgebungsverdeckung", "Oclusión ambiental", "Occlusion ambiante", "Oclusão de ambiente",
      "アンビエントオクルージョン", "环境光遮蔽", "Occlusione ambientale" },
    { "Soft shading in corners and where objects meet the floor (SSAO): low, medium or high strength",
      "Мягкое затенение в углах и там, где предметы касаются пола (SSAO): слабое, среднее или сильное",
      "Weiche Schattierung in Ecken und dort, wo Gegenstände den Boden berühren (SSAO): niedrig, mittel oder hoch",
      "Sombreado suave en las esquinas y donde los objetos tocan el suelo (SSAO): bajo, medio o alto",
      "Ombrage doux dans les coins et là où les objets touchent le sol (SSAO) : faible, moyen ou élevé",
      "Sombreamento suave nos cantos e onde os objetos tocam o chão (SSAO): baixo, médio ou alto",
      "隅や物が床に接する部分の柔らかい陰影(SSAO):弱・中・強",
      "角落和物体接触地面处的柔和阴影（SSAO）：弱、中或强",
      "Ombreggiatura morbida negli angoli e dove gli oggetti toccano il pavimento (SSAO): intensità bassa, media o alta" },
    /* 27.09: дымка у ламп (вкладка GRAPHICS) */
    { "Lamp Haze", "Дымка у ламп", "Lampendunst", "Neblina de luces", "Brume lumineuse", "Névoa das luzes",
      "ランプのもや", "灯光雾气", "Foschia delle luci" },
    { "A light haze in the air around lamps and glowing panels. Off: clear air, as in the original game",
      "Лёгкая дымка в воздухе у ламп и светящихся панелей. Выкл - чистый воздух, как в оригинале",
      "Leichter Dunst in der Luft um Lampen und leuchtende Paneele. Aus: klare Luft wie im Originalspiel",
      "Una ligera neblina en el aire alrededor de las lámparas y los paneles luminosos. No: aire limpio, como en el juego original",
      "Une légère brume dans l'air autour des lampes et des panneaux lumineux. Non : air clair, comme dans le jeu original",
      "Uma leve névoa no ar ao redor das lâmpadas e dos painéis luminosos. Não: ar limpo, como no jogo original",
      "ランプや光るパネルの周りの空気にかすかなもやがかかります。オフ:オリジナルと同じ澄んだ空気",
      "灯光和发光面板周围的空气中有淡淡的雾气。关：空气清澈，与原版游戏相同",
      "Una leggera foschia nell'aria intorno alle lampade e ai pannelli luminosi. No: aria limpida, come nel gioco originale" },
    /* 27.09 (2): подраздел вкладки GRAPHICS — экспериментальные опции (тени, дымка) */
    { "Experimental", "Экспериментальные", "Experimentell", "Experimental", "Expérimental", "Experimental",
      "実験的機能", "实验性功能", "Sperimentale" },
    /* степени затенения (род — по «затенение»); «2x/4x/8x» у сглаживания не переводятся */
    { "Low", "Слабое", "Niedrig", "Bajo", "Faible", "Baixo", "弱", "弱", "Basso" },
    { "Medium", "Среднее", "Mittel", "Medio", "Moyen", "Médio", "中", "中", "Medio" },
    { "High", "Сильное", "Hoch", "Alto", "Élevé", "Alto", "強", "强", "Alto" },
    /* 15.09 вечер: «Crosshair On Target» и «Aim Focus» убраны из меню — всегда включены */
    /* значения */
    { "Windowed", "Окно", "Fenster", "Ventana", "Fenêtré", "Janela", "ウィンドウ", "窗口", "Finestra" },
    { "Borderless", "Без рамки", "Randlos", "Sin bordes", "Sans bordure", "Sem bordas", "ボーダーレス", "无边框",
      "Senza bordi" },
    { "Fullscreen", "Полный экран", "Vollbild", "Pantalla completa", "Plein écran", "Tela cheia", "フルスクリーン", "全屏",
      "Schermo intero" },
    /* соотношение сторон (15.09 ночь): «4:3», «16:9» и прочие формы не переводятся; 10.10 - «16:9, интерфейс 4:3» убран
     * (строка Stretch Interface) */
    /* тип игры (15.09 вечер): род — по «тип игры» */
    { "Modern", "Современный", "Modern", "Moderno", "Moderne", "Moderno", "モダン", "现代", "Moderno" },
    { "Original", "Оригинальный", "Original", "Original", "Original", "Original", "オリジナル", "原版", "Originale" },
    { "Toggle", "Переключение", "Umschalten", "Alternar", "Bascule", "Alternar", "切り替え", "切换", "Alterna" },
    { "Hold", "Удержание", "Halten", "Mantener", "Maintien", "Segurar", "長押し", "按住", "Tieni premuto" },
    { "On", "Вкл", "An", "Sí", "Oui", "Sim", "オン", "开", "Sì" },
    { "Off", "Выкл", "Aus", "No", "Non", "Não", "オフ", "关", "No" },
    { "Per Weapon", "По оружию", "Je Waffe", "Por arma", "Par arme", "Por arma", "武器ごと", "按武器", "Per arma" },
    { "Dot", "Точка", "Punkt", "Punto", "Point", "Ponto", "ドット", "圆点", "Punto" },
    { "Cross", "Перекрестие", "Kreuz", "Cruz", "Croix", "Cruz", "十字", "十字", "Croce" },
    /* сообщения внизу меню */
    { "Applies after a restart", "Применится после перезапуска", "Gilt nach einem Neustart", "Se aplica tras reiniciar",
      "S'applique après un redémarrage", "Aplica-se após reiniciar", "再起動後に反映されます", "重启后生效",
      "Si applica dopo il riavvio" },
    { "Press ENTER to apply", "Нажмите ENTER, чтобы применить", "ENTER zum Übernehmen drücken", "Pulsa ENTER para aplicar",
      "Appuyez sur ENTRÉE pour appliquer", "Pressione ENTER para aplicar", "ENTER で適用", "按 ENTER 应用",
      "Premi INVIO per applicare" },
    { "Could not write dc_user.cfg", "Не удалось записать dc_user.cfg", "dc_user.cfg konnte nicht geschrieben werden",
      "No se pudo escribir dc_user.cfg", "Impossible d'écrire dc_user.cfg", "Não foi possível gravar dc_user.cfg",
      "dc_user.cfg を書き込めませんでした", "无法写入 dc_user.cfg", "Impossibile scrivere dc_user.cfg" },
    { "Settings saved", "Настройки сохранены", "Einstellungen gespeichert", "Ajustes guardados",
      "Paramètres enregistrés", "Configurações salvas", "設定を保存しました", "设置已保存", "Impostazioni salvate" },
    { "Available in the game world only", "Доступно только в игре - не в меню и не в роликах",
      "Nur in der Spielwelt verfügbar", "Solo disponible en el mundo del juego", "Disponible uniquement en jeu",
      "Disponível apenas no mundo do jogo", "ゲーム中のみ使用できます", "仅在游戏世界中可用", "Disponibile solo nel mondo di gioco" },
    { "Opening the game's screen...", "Открываю экран игры...", "Spielbildschirm wird geöffnet...",
      "Abriendo la pantalla del juego...", "Ouverture de l'écran du jeu...", "Abrindo a tela do jogo...",
      "ゲーム画面を開いています...", "正在打开游戏画面...", "Apertura della schermata del gioco..." },
    { "TAB/Q tabs   arrows/WASD select   ENTER change   ESC back   |   L1/R1   Cross   Circle",
      "TAB/Q вкладки   стрелки/WASD выбор   ENTER изменить   ESC назад   |   L1/R1   Крест   Круг",
      "TAB/Q Reiter   Pfeile/WASD Auswahl   ENTER ändern   ESC zurück   |   L1/R1   Kreuz   Kreis",
      "TAB/Q pestañas   flechas/WASD elegir   ENTER cambiar   ESC atrás   |   L1/R1   Cruz   Círculo",
      "TAB/Q onglets   flèches/WASD choisir   ENTRÉE modifier   ÉCHAP retour   |   L1/R1   Croix   Rond",
      "TAB/Q abas   setas/WASD escolher   ENTER mudar   ESC voltar   |   L1/R1   X   Círculo",
      "TAB/Q タブ   矢印/WASD 選択   ENTER 変更   ESC 戻る   |   L1/R1   ×   ○",
      "TAB/Q 标签   方向键/WASD 选择   ENTER 更改   ESC 返回   |   L1/R1   ×   ○",
      "TAB/Q schede   frecce/WASD scegli   INVIO cambia   ESC indietro   |   L1/R1   Croce   Cerchio" },
    /* подсказка пропуска сцены */
    { "Skip", "Пропустить", "Überspringen", "Omitir", "Passer", "Pular", "スキップ", "跳过", "Salta" },
    { "Skipping...", "Пропускаем...", "Wird übersprungen...", "Omitiendo...", "Passage...", "Pulando...",
      "スキップ中...", "正在跳过...", "Salto in corso..." },
    { "Skipped", "Пропущено", "Übersprungen", "Omitido", "Passé", "Pulado", "スキップしました", "已跳过", "Saltato" },
    { "SPACE", "ПРОБЕЛ", "LEERTASTE", "ESPACIO", "ESPACE", "ESPAÇO", "スペース", "空格", "SPAZIO" },
    /* ⭐ 04.10 (вечер, 6): назначения кнопок — шаблон раскладки, подстраницы клавиатуры и геймпада */
    { "Button Layout", "Шаблон раскладки", "Steuerungsvorlage", "Esquema de botones", "Modèle de commandes",
      "Modelo de controles", "ボタン配置", "按键方案", "Schema dei tasti" },
    { "For the current game type (GAME tab). Original: the buttons of the 1999 game and its PC version. Modern: like the Resident Evil remakes. Custom: your own - change any button below",
      "Для текущего типа игры (вкладка ИГРА). Оригинальный - кнопки игры 1999 года и её PC-версии. Современный - как в ремейках Resident Evil. Свой - ваш: любую кнопку можно поменять ниже",
      "Für die aktuelle Spielart (Reiter SPIEL). Original: die Tasten des Spiels von 1999 und seiner PC-Version. Modern: wie in den Resident-Evil-Remakes. Benutzerdefiniert: eigene Belegung - jede Taste unten änderbar",
      "Para el tipo de juego actual (pestaña JUEGO). Original: los botones del juego de 1999 y su versión de PC. Moderno: como en los remakes de Resident Evil. Personalizado: el tuyo, cambia cualquier botón abajo",
      "Pour le type de jeu actuel (onglet JEU). Original : les touches du jeu de 1999 et de sa version PC. Moderne : comme les remakes de Resident Evil. Personnalisé : le vôtre, chaque touche se modifie ci-dessous",
      "Para o tipo de jogo atual (aba JOGO). Original: os botões do jogo de 1999 e de sua versão para PC. Moderno: como nos remakes de Resident Evil. Personalizado: o seu, qualquer botão pode ser trocado abaixo",
      "現在のゲームタイプ用(ゲームタブ)。オリジナル:1999年版とそのPC版のボタン。モダン:バイオハザードのリメイク風。カスタム:下で自由に変更できます",
      "用于当前游戏类型（游戏标签）。原版：1999 年原版及其 PC 版的按键。现代：类似生化危机重制版。自定义：可在下方任意更改按键",
      "Per il tipo di gioco attuale (scheda GIOCO). Originale: i tasti del gioco del 1999 e della sua versione PC. Moderno: come nei remake di Resident Evil. Personalizzato: il tuo, cambia qualsiasi tasto qui sotto" },
    { "Custom", "Свой", "Benutzerdefiniert", "Personalizado", "Personnalisé", "Personalizado", "カスタム", "自定义",
      "Personalizzato" },
    { "Keyboard & Mouse", "Клавиатура и мышь", "Tastatur & Maus", "Teclado y ratón", "Clavier et souris",
      "Teclado e mouse", "キーボード & マウス", "键盘与鼠标", "Tastiera e mouse" },
    { "Keys and mouse buttons for every action of the current layout",
      "Клавиши и кнопки мыши для каждого действия текущей раскладки",
      "Tasten und Maustasten für jede Aktion der aktuellen Belegung",
      "Teclas y botones del ratón para cada acción del esquema actual",
      "Touches et boutons de la souris pour chaque action de la disposition actuelle",
      "Teclas e botões do mouse para cada ação do modelo atual",
      "現在の配置での各操作のキーとマウスボタン", "当前方案中每个动作的按键和鼠标按钮", "Tasti e pulsanti del mouse per ogni azione dello schema attuale" },
    { "Gamepad", "Геймпад", "Gamepad", "Mando", "Manette", "Controle", "ゲームパッド", "手柄", "Controller" },
    { "Gamepad buttons for every action of the current layout. Sticks: left - movement, right - camera",
      "Кнопки геймпада для каждого действия текущей раскладки. Стики: левый - движение, правый - камера",
      "Gamepad-Tasten für jede Aktion der aktuellen Belegung. Sticks: links Bewegung, rechts Kamera",
      "Botones del mando para cada acción del esquema actual. Sticks: izquierdo, movimiento; derecho, cámara",
      "Boutons de la manette pour chaque action de la disposition actuelle. Sticks : gauche, déplacement ; droit, caméra",
      "Botões do controle para cada ação do modelo atual. Analógicos: esquerdo, movimento; direito, câmera",
      "現在の配置での各操作のゲームパッドボタン。スティック:左は移動、右はカメラ", "当前方案中每个动作的手柄按键。摇杆：左为移动，右为镜头",
      "Pulsanti del controller per ogni azione dello schema attuale. Stick: sinistro - movimento, destro - telecamera" },
    { "KEYBOARD & MOUSE", "КЛАВИАТУРА И МЫШЬ", "TASTATUR & MAUS", "TECLADO Y RATÓN", "CLAVIER ET SOURIS",
      "TECLADO E MOUSE", "キーボード & マウス", "键盘与鼠标", "TASTIERA E MOUSE" },
    { "GAMEPAD", "ГЕЙМПАД", "GAMEPAD", "MANDO", "MANETTE", "CONTROLE", "ゲームパッド", "手柄", "CONTROLLER" },
    /* ⭐ 10.10.2026: раскладки обоих типов игры по устройствам (строка «For Game Type» вверху подстраниц) */
    { "Keys and mouse buttons of each game type, Modern and Original: its layout and every action",
      "Клавиши и кнопки мыши для каждого типа игры, современного и оригинального: его раскладка и каждое действие",
      "Tasten und Maustasten jeder Spielart, Modern und Original: ihre Belegung und jede Aktion",
      "Teclas y botones del ratón de cada tipo de juego, moderno y original: su esquema y cada acción",
      "Touches et boutons de la souris de chaque type de jeu, moderne et original : sa disposition et chaque action",
      "Teclas e botões do mouse de cada tipo de jogo, moderno e original: seu modelo e cada ação",
      "モダンとオリジナル、各ゲームタイプのキーとマウスボタン:配置と各操作",
      "现代与原版两种游戏类型各自的按键和鼠标按钮：方案与每个动作",
      "Tasti e pulsanti del mouse di ogni tipo di gioco, Moderno e Originale: il suo schema e ogni azione" },
    { "Gamepad buttons of each game type, Modern and Original: its layout and every action",
      "Кнопки геймпада для каждого типа игры, современного и оригинального: его раскладка и каждое действие",
      "Gamepad-Tasten jeder Spielart, Modern und Original: ihre Belegung und jede Aktion",
      "Botones del mando de cada tipo de juego, moderno y original: su esquema y cada acción",
      "Boutons de la manette de chaque type de jeu, moderne et original : sa disposition et chaque action",
      "Botões do controle de cada tipo de jogo, moderno e original: seu modelo e cada ação",
      "モダンとオリジナル、各ゲームタイプのゲームパッドボタン:配置と各操作",
      "现代与原版两种游戏类型各自的手柄按键：方案与每个动作",
      "Pulsanti del controller di ogni tipo di gioco, Moderno e Originale: il suo schema e ogni azione" },
    { "For Game Type", "Для типа игры", "Für Spielart", "Para el tipo de juego", "Pour le type de jeu", "Para o tipo de jogo",
      "対象ゲームタイプ", "适用游戏类型", "Per il tipo di gioco" },
    { "Whose buttons to show and change: the Modern or the Original game type - each has its own layout. The game type itself is chosen on the GAME tab",
      "Чьи кнопки показать и менять: современного или оригинального типа игры - у каждого своя раскладка. Сам тип игры выбирается на вкладке ИГРА",
      "Wessen Tasten angezeigt und geändert werden: moderne oder originale Spielart - jede hat ihre eigene Belegung. Die Spielart selbst wird im Reiter SPIEL gewählt",
      "De qué tipo de juego mostrar y cambiar los botones: moderno u original; cada uno tiene su propio esquema. El tipo de juego se elige en la pestaña JUEGO",
      "Les touches de quel type de jeu afficher et modifier : moderne ou original - chacun a sa propre disposition. Le type de jeu se choisit dans l'onglet JEU",
      "De qual tipo de jogo mostrar e trocar os botões: moderno ou original - cada um tem seu próprio modelo. O tipo de jogo é escolhido na aba JOGO",
      "表示・変更するボタンのゲームタイプ:モダンかオリジナル(それぞれ別の配置)。ゲームタイプ自体はゲームタブで選びます",
      "显示并修改哪种游戏类型的按键：现代或原版，各有自己的方案。游戏类型本身在“游戏”标签中选择",
      "Di quale tipo di gioco mostrare e cambiare i tasti: Moderno od Originale, ognuno ha il suo schema. Il tipo di gioco si sceglie nella scheda GIOCO" },
    { "Classic keys: WASD or the arrows walk, Shift runs, Caps Lock runs without holding, Q or X turns around, the right mouse button aims and the left one shoots",
      "Классические клавиши: WASD или стрелки - ходьба, Shift - бег, Caps Lock - бег без удержания, Q или X - разворот, правая кнопка мыши - прицел, левая - выстрел",
      "Klassische Tasten: WASD oder die Pfeiltasten zum Gehen, Umschalt zum Laufen, Feststelltaste zum Laufen ohne Halten, Q oder X zum Umdrehen, rechte Maustaste zielt, linke schießt",
      "Teclas clásicas: WASD o las flechas para caminar, Mayús para correr, Bloq Mayús para correr sin mantener, Q o X para darse la vuelta; el botón derecho del ratón apunta y el izquierdo dispara",
      "Touches classiques : WASD ou les flèches pour marcher, Maj pour courir, Verr. Maj pour courir sans maintenir, Q ou X pour faire demi-tour ; le clic droit vise et le clic gauche tire",
      "Teclas clássicas: WASD ou as setas para andar, Shift para correr, Caps Lock para correr sem segurar, Q ou X para dar meia-volta; o botão direito do mouse mira e o esquerdo atira",
      "クラシック:WASDか矢印キーで移動、Shiftでダッシュ、Caps Lockで押さずにダッシュ、QかXで振り向き、右クリックで構え、左クリックで撃ちます",
      "经典按键：WASD 或方向键移动，Shift 奔跑，Caps Lock 无需按住即可奔跑，Q 或 X 转身，鼠标右键瞄准，左键射击",
      "Tasti classici: WASD o le frecce per camminare, Shift per correre, Bloc Maiusc per correre senza tenere premuto, Q o X per voltarsi, il tasto destro del mouse mira e il sinistro spara" },
    { "As in today's remakes and remasters: WASD walk, Shift or Space runs, X turns around, the right mouse button aims and the left one shoots",
      "Как в нынешних ремейках и ремастерах: WASD - ходьба, Shift или пробел - бег, X - разворот, правая кнопка мыши - прицел, левая - выстрел",
      "Wie in heutigen Remakes und Remastern: WASD zum Gehen, Umschalt oder Leertaste zum Laufen, X zum Umdrehen, rechte Maustaste zielt, linke schießt",
      "Como en los remakes y remasters actuales: WASD para caminar, Mayús o Espacio para correr, X para darse la vuelta; el botón derecho del ratón apunta y el izquierdo dispara",
      "Comme les remakes et remasters actuels : WASD pour marcher, Maj ou Espace pour courir, X pour faire demi-tour ; le clic droit vise et le clic gauche tire",
      "Como nos remakes e remasters atuais: WASD para andar, Shift ou Espaço para correr, X para dar meia-volta; o botão direito do mouse mira e o esquerdo atira",
      "現在のリメイク・リマスター風:WASDで移動、ShiftかSpaceでダッシュ、Xで振り向き、右クリックで構え、左クリックで撃ちます",
      "类似如今的重制版和高清版：WASD 移动，Shift 或空格奔跑，X 转身，鼠标右键瞄准，左键射击",
      "Come nei remake e remaster di oggi: WASD per camminare, Shift o Spazio per correre, X per voltarsi, il tasto destro del mouse mira e il sinistro spara" },
    { "Your own buttons: change any of them below",
      "Свои клавиши: любую можно поменять ниже",
      "Eigene Belegung: jede Taste unten änderbar",
      "Tus propias teclas: cambia cualquiera abajo",
      "Vos propres touches : chacune se modifie ci-dessous",
      "Suas próprias teclas: troque qualquer uma abaixo",
      "自分の配置:下で自由に変更できます",
      "自定义按键：可在下方任意更改", "I tuoi tasti: cambia quelli che vuoi qui sotto" },
    { "Original: the buttons of the 1999 game, one to one. Modern: as in today's remakes and remasters. Custom: your own - change any button below",
      "Оригинальный - кнопки игры 1999 года один в один. Современный - как в нынешних ремейках и ремастерах. Свой - ваш: любую кнопку можно поменять ниже",
      "Original: die Tasten des Spiels von 1999, eins zu eins. Modern: wie in heutigen Remakes und Remastern. Benutzerdefiniert: eigene Belegung - jede Taste unten änderbar",
      "Original: los botones del juego de 1999, tal cual. Moderno: como en los remakes y remasters actuales. Personalizado: el tuyo, cambia cualquier botón abajo",
      "Original : les boutons du jeu de 1999, à l'identique. Moderne : comme les remakes et remasters actuels. Personnalisé : le vôtre, chaque touche se modifie ci-dessous",
      "Original: os botões do jogo de 1999, iguais. Moderno: como nos remakes e remasters atuais. Personalizado: o seu, qualquer botão pode ser trocado abaixo",
      "オリジナル:1999年版のボタンそのまま。モダン:現在のリメイク・リマスター風。カスタム:下で自由に変更できます",
      "原版：与 1999 年原版完全相同的按键。现代：类似如今的重制版和高清版。自定义：可在下方任意更改按键",
      "Originale: i tasti del gioco del 1999, identici. Moderno: come nei remake e remaster di oggi. Personalizzato: il tuo, cambia qualsiasi tasto qui sotto" },
    { "Back to the default layout for the game type chosen above",
      "Вернуть раскладку по умолчанию для выбранного выше типа игры",
      "Zurück zur Standardbelegung der oben gewählten Spielart",
      "Volver al esquema por defecto del tipo de juego elegido arriba",
      "Revenir à la disposition par défaut du type de jeu choisi ci-dessus",
      "Voltar ao modelo padrão do tipo de jogo escolhido acima",
      "上で選んだゲームタイプの初期配置に戻します",
      "恢复上方所选游戏类型的默认方案", "Torna allo schema predefinito per il tipo di gioco scelto sopra" },
    { "Toggle Run Button", "Кнопка бега переключением", "Taste »Laufen umschalten«", "Botón de correr (alternar)",
      "Touche course (bascule)", "Botão de correr (alternar)", "ダッシュ切替ボタン", "切换奔跑按键", "Tasto corsa (alterna)" },
    { "How the Run (Toggle) button works - the left stick click (L3) in the Modern layout. Toggle: Regina runs until she stops or raises the weapon. Hold: only while it is held down",
      "Как работает кнопка «Бег переключением» - в современной раскладке это нажатие левого стика (L3). Переключение - Регина бежит, пока не остановится или не поднимет оружие. Удержание - только пока кнопка нажата",
      "So wirkt die Taste »Laufen umschalten« - im modernen Layout der Klick auf den linken Stick (L3). Umschalten: Regina läuft, bis sie stehen bleibt oder die Waffe hebt. Halten: nur solange sie gedrückt ist",
      "Cómo funciona el botón Correr (alternar): en el esquema moderno es pulsar el stick izquierdo (L3). Alternar: Regina corre hasta que se detiene o alza el arma. Mantener: solo mientras lo mantienes pulsado",
      "Fonctionnement de la touche Course (bascule) - le clic du stick gauche (L3) dans la disposition moderne. Bascule : Regina court jusqu'à s'arrêter ou lever son arme. Maintien : seulement tant qu'elle est enfoncée",
      "Como funciona o botão Correr (alternar) - no modelo moderno é o clique do analógico esquerdo (L3). Alternar: Regina corre até parar ou erguer a arma. Segurar: só enquanto estiver pressionado",
      "「ダッシュ切替」ボタンの動作(モダン配置では左スティック押し込み L3)。切り替え:止まるか武器を構えるまで走り続けます。長押し:押している間だけ走ります",
      "“切换奔跑”按键的方式（现代方案中为按下左摇杆 L3）。切换：瑞吉娜持续奔跑，直到停下或举起武器。按住：仅在按住时奔跑",
      "Come funziona il tasto Corsa (alterna), cioè la pressione dello stick sinistro (L3) nello schema Moderno. Alterna: Regina corre finché non si ferma o alza l'arma. Tieni premuto: solo finché lo tieni premuto" },
    /* действия (строки подстраниц) и их группы */
    { "Movement", "Движение", "Bewegung", "Movimiento", "Déplacement", "Movimento", "移動", "移动", "Movimento" },
    { "Combat", "Бой", "Kampf", "Combate", "Combat", "Combate", "戦闘", "战斗", "Combattimento" },
    { "Actions & Menus", "Действия и меню", "Aktionen & Menüs", "Acciones y menús", "Actions et menus", "Ações e menus",
      "アクションとメニュー", "行动与菜单", "Azioni e menu" },
    { "Move Forward", "Вперёд", "Vorwärts", "Avanzar", "Avancer", "Avançar", "前進", "前进", "Avanti" },
    { "Move Backward", "Назад", "Rückwärts", "Retroceder", "Reculer", "Recuar", "後退", "后退", "Indietro" },
    { "Move Left", "Влево", "Links", "Izquierda", "Gauche", "Esquerda", "左", "向左", "Sinistra" },
    { "Move Right", "Вправо", "Rechts", "Derecha", "Droite", "Direita", "右", "向右", "Destra" },
    { "Run (Hold)", "Бег, пока держишь", "Laufen (halten)", "Correr (mantener)", "Courir (maintenir)", "Correr (segurar)",
      "押している間ダッシュ", "按住奔跑", "Corsa (tieni premuto)" },
    { "Run (Toggle)", "Бег переключением", "Laufen (umschalten)", "Correr (alternar)", "Courir (bascule)", "Correr (alternar)",
      "ダッシュ切替", "切换奔跑", "Corsa (alterna)" },
    { "Quick Turn", "Разворот на 180°", "180°-Drehung", "Giro de 180°", "Demi-tour", "Giro de 180°", "180°ターン", "180°转身",
      "Giro di 180°" },
    { "Aim", "Прицел", "Zielen", "Apuntar", "Viser", "Mirar", "構える", "瞄准", "Mira" },
    { "Fire", "Выстрел", "Schießen", "Disparar", "Tirer", "Atirar", "射撃", "射击", "Spara" },
    { "Switch Target", "Смена цели", "Ziel wechseln", "Cambiar objetivo", "Changer de cible", "Trocar alvo", "ターゲット切替", "切换目标",
      "Cambia bersaglio" },
    { "Action / Confirm", "Действие / подтвердить", "Aktion / Bestätigen", "Acción / Confirmar", "Action / Valider",
      "Ação / Confirmar", "アクション / 決定", "行动 / 确认", "Azione / Conferma" },
    { "Inventory", "Инвентарь", "Inventar", "Inventario", "Inventaire", "Inventário", "インベントリ", "物品栏", "Inventario" },
    { "Cancel", "Отмена", "Abbrechen", "Cancelar", "Annuler", "Cancelar", "キャンセル", "取消", "Annulla" },
    { "Pause", "Пауза", "Pause", "Pausa", "Pause", "Pausa", "ポーズ", "暂停", "Pausa" },
    { "OPTION Menu", "Меню OPTION", "OPTION-Menü", "Menú OPTION", "Menu OPTION", "Menu OPTION", "OPTIONメニュー", "OPTION 菜单",
      "Menu OPTION" },
    /* 25.10.2026 ночь: смена типа игры (была клавишей O мимо раскладки) - действие раскладки */
    { "Switch Game Type", "Сменить тип игры", "Spielart wechseln", "Cambiar tipo de juego", "Changer de type de jeu",
      "Trocar tipo de jogo", "ゲームタイプ切替", "切换游戏类型", "Cambia tipo di gioco" },
    /* 25.10.2026 вечер: быстрые сохранения - действия раскладки (подписи клавиш - Delete, Page Up, F1... - не переводятся) */
    { "Quicksaves", "Быстрые сохранения", "Schnellspeicherstände", "Guardados rápidos", "Sauvegardes rapides",
      "Salvamentos rápidos", "クイックセーブ・ロード", "快速存读档", "Salvataggi rapidi" },
    { "Quick Save", "Быстрое сохранение", "Schnellspeichern", "Guardado rápido", "Sauvegarde rapide", "Salvamento rápido",
      "クイックセーブ", "快速存档", "Salvataggio rapido" },
    { "Quick Load", "Быстрая загрузка", "Schnellladen", "Carga rápida", "Chargement rapide", "Carregamento rápido",
      "クイックロード", "快速读档", "Caricamento rapido" },
    { "Next Slot", "Следующий слот", "Nächster Slot", "Ranura siguiente", "Emplacement suivant", "Próximo slot",
      "次のスロット", "下一个存档位", "Slot successivo" },
    { "Previous Slot", "Предыдущий слот", "Vorheriger Slot", "Ranura anterior", "Emplacement précédent", "Slot anterior",
      "前のスロット", "上一个存档位", "Slot precedente" },
    { "Restore Defaults", "Вернуть по умолчанию", "Standard wiederherstellen", "Restaurar valores", "Rétablir par défaut",
      "Restaurar padrão", "初期設定に戻す", "恢复默认", "Ripristina predefiniti" },
    /* 11.10.2026: сброс вкладки настроек («у каждого раздела кнопку - сбросить до дефолта») */
    { "Every setting on this tab goes back to its default value",
      "Все настройки этой вкладки вернутся к значениям по умолчанию",
      "Alle Einstellungen dieser Registerkarte werden auf ihre Standardwerte zurückgesetzt",
      "Todos los ajustes de esta pestaña vuelven a su valor por defecto",
      "Tous les réglages de cet onglet reviennent à leur valeur par défaut",
      "Todas as configurações desta aba voltam ao valor padrão",
      "このタブの設定をすべて初期値に戻します",
      "将此选项卡中的所有设置恢复为默认值", "Tutte le impostazioni di questa scheda tornano al valore predefinito" },
    { "Every setting on this tab except Language goes back to its default value",
      "Все настройки этой вкладки, кроме языка, вернутся к значениям по умолчанию",
      "Alle Einstellungen dieser Registerkarte außer der Sprache werden auf ihre Standardwerte zurückgesetzt",
      "Todos los ajustes de esta pestaña, salvo el idioma, vuelven a su valor por defecto",
      "Tous les réglages de cet onglet, sauf la langue, reviennent à leur valeur par défaut",
      "Todas as configurações desta aba, exceto o idioma, voltam ao valor padrão",
      "言語以外のこのタブの設定をすべて初期値に戻します",
      "将此选项卡中除语言外的所有设置恢复为默认值",
      "Tutte le impostazioni di questa scheda, tranne la lingua, tornano al valore predefinito" },
    { "Restored the default settings", "Настройки по умолчанию восстановлены", "Standardeinstellungen wiederhergestellt",
      "Ajustes por defecto restaurados", "Réglages par défaut rétablis", "Configurações padrão restauradas",
      "初期設定に戻しました", "已恢复默认设置", "Impostazioni predefinite ripristinate" },
    { "Back to the default buttons of this game type: Modern in the Modern game type, Original in the Original one",
      "Вернуть кнопки по умолчанию для этого типа игры: современные - в современном, оригинальные - в оригинальном",
      "Zurück zur Standardbelegung dieser Spielart: Modern im modernen, Original im originalen Spiel",
      "Volver a los botones por defecto de este tipo de juego: moderno en el moderno, original en el original",
      "Revenir aux touches par défaut de ce type de jeu : moderne en moderne, original en original",
      "Voltar aos botões padrão deste tipo de jogo: moderno no moderno, original no original",
      "このゲームタイプの初期ボタンに戻します(モダンはモダン、オリジナルはオリジナル)",
      "恢复此游戏类型的默认按键：现代类型用现代，原版类型用原版",
      "Torna ai tasti predefiniti di questo tipo di gioco: Moderno nel tipo Moderno, Originale in quello Originale" },
    { "Enter or click: press a new key or mouse button for this action. Shift+Enter: add one more. Delete: clear. Esc always works as Back",
      "Enter или щелчок - нажмите новую клавишу или кнопку мыши для этого действия. Shift+Enter - добавить ещё одну. Delete - очистить. Esc всегда работает как «назад»",
      "Enter oder Klick: neue Taste oder Maustaste für diese Aktion drücken. Umschalt+Enter: eine weitere hinzufügen. Entf: leeren. Esc wirkt immer als Zurück",
      "Enter o clic: pulsa una nueva tecla o botón del ratón para esta acción. Mayús+Enter: añadir otra. Supr: borrar. Esc siempre funciona como Atrás",
      "Entrée ou clic : appuyez sur une nouvelle touche ou un bouton de souris pour cette action. Maj+Entrée : en ajouter une. Suppr : effacer. Échap sert toujours de Retour",
      "Enter ou clique: pressione uma nova tecla ou botão do mouse para esta ação. Shift+Enter: adicionar outra. Delete: limpar. Esc sempre funciona como Voltar",
      "Enter またはクリック:この操作に割り当てる新しいキーかマウスボタンを押します。Shift+Enter:追加。Delete:消去。Esc は常に「戻る」です",
      "Enter 或单击：为此动作按下新的按键或鼠标按钮。Shift+Enter：再添加一个。Delete：清除。Esc 始终用作返回",
      "Invio o clic: premi un nuovo tasto o pulsante del mouse per questa azione. Shift+Invio: aggiungine un altro. Canc: cancella. Esc funziona sempre come Indietro" },
    { "Press a new button for this action, add one more or clear it - see the buttons below. The left stick always moves Regina, the right one turns the camera",
      "Нажмите новую кнопку для этого действия, добавьте ещё одну или очистите - кнопки внизу. Левый стик всегда двигает Регину, правый поворачивает камеру",
      "Neue Taste für diese Aktion drücken, eine weitere hinzufügen oder leeren - siehe Tasten unten. Der linke Stick bewegt Regina immer, der rechte dreht die Kamera",
      "Pulsa un botón nuevo para esta acción, añade otro o bórralo: mira los botones abajo. El stick izquierdo siempre mueve a Regina y el derecho gira la cámara",
      "Appuyez sur un nouveau bouton pour cette action, ajoutez-en un ou effacez - voir les boutons en bas. Le stick gauche déplace toujours Regina, le droit tourne la caméra",
      "Pressione um novo botão para esta ação, adicione outro ou limpe - veja os botões abaixo. O analógico esquerdo sempre move Regina, o direito gira a câmera",
      "この操作に新しいボタンを割り当て、追加、または消去します(下のボタン参照)。左スティックは常に移動、右スティックはカメラです",
      "为此动作按下新按钮、再添加一个或清除（见下方按钮）。左摇杆始终用于移动，右摇杆转动镜头",
      "Premi un nuovo pulsante per questa azione, aggiungine un altro o cancellalo: vedi i pulsanti qui sotto. Lo stick sinistro muove sempre Regina, il destro ruota la telecamera" },
    /* значения: подписи клавиш (английские, кроме мыши и пробела), «нажмите…», «не назначено» */
    { "Space", "Пробел", "Leertaste", "Espacio", "Espace", "Espaço", "スペース", "空格", "Spazio" },
    { "LMB", "ЛКМ", "LMT", "Clic izdo.", "Clic gauche", "Botão esq.", "左クリック", "鼠标左键", "Clic sx" },
    { "RMB", "ПКМ", "RMT", "Clic dcho.", "Clic droit", "Botão dir.", "右クリック", "鼠标右键", "Clic dx" },
    { "MMB", "СКМ", "MMT", "Clic central", "Clic milieu", "Botão do meio", "中クリック", "鼠标中键", "Clic centrale" },
    { "Mouse 4", "Мышь 4", "Maus 4", "Ratón 4", "Souris 4", "Mouse 4", "マウス4", "鼠标4", "Mouse 4" },
    { "Mouse 5", "Мышь 5", "Maus 5", "Ratón 5", "Souris 5", "Mouse 5", "マウス5", "鼠标5", "Mouse 5" },
    /* 10.10: современный рендер не поднялся (main.cpp dc_renderer_fail_notice; два %s - причина и видеокарта) */
    { "The modern graphics could not start on this computer: %s.\n\nThe game will run with the original PlayStation graphics, without the 3D rooms. Updating the graphics card driver usually fixes this.\n\nGraphics card: %s",
      "Современная графика не запустилась на этом компьютере: %s.\n\nИгра пойдёт с оригинальной графикой PlayStation, без трёхмерных комнат. Обычно помогает обновить драйвер видеокарты.\n\nВидеокарта: %s",
      "Die moderne Grafik konnte auf diesem Computer nicht starten: %s.\n\nDas Spiel läuft mit der originalen PlayStation-Grafik, ohne die 3D-Räume. Meist hilft ein aktueller Grafiktreiber.\n\nGrafikkarte: %s",
      "Los gráficos modernos no pudieron iniciarse en este equipo: %s.\n\nEl juego funcionará con los gráficos originales de PlayStation, sin las salas en 3D. Actualizar el controlador de la tarjeta gráfica suele solucionarlo.\n\nTarjeta gráfica: %s",
      "Les graphismes modernes n'ont pas pu démarrer sur cet ordinateur : %s.\n\nLe jeu tournera avec les graphismes PlayStation d'origine, sans les salles en 3D. Mettre à jour le pilote de la carte graphique règle généralement le problème.\n\nCarte graphique : %s",
      "Os gráficos modernos não puderam iniciar neste computador: %s.\n\nO jogo vai rodar com os gráficos originais do PlayStation, sem as salas em 3D. Atualizar o driver da placa de vídeo costuma resolver.\n\nPlaca de vídeo: %s",
      "このPCでは最新のグラフィックを起動できませんでした: %s。\n\nゲームは3Dの部屋なしで、オリジナルのPlayStationのグラフィックで動作します。グラフィックドライバーを更新すると直ることが多いです。\n\nグラフィックカード: %s",
      "无法在这台电脑上启动现代画面：%s。\n\n游戏将以原版 PlayStation 画面运行，没有 3D 房间。更新显卡驱动通常可以解决。\n\n显卡：%s",
      "La grafica moderna non è riuscita ad avviarsi su questo computer: %s.\n\nIl gioco funzionerà con la grafica originale della PlayStation, senza le stanze in 3D. Di solito si risolve aggiornando il driver della scheda video.\n\nScheda video: %s" },
    /* 18.10.2026: видеокарта потеряна посреди игры (ptvk_lost, main.cpp dc_gpu_lost_notice) */
    { "The graphics card driver stopped responding and was reset.\n\nThe modern graphics stay off until the game is restarted. Make a quicksave, restart the game and load it.",
      "Драйвер видеокарты перестал отвечать и был перезапущен.\n\nСовременная графика выключена до перезапуска игры. Сделайте быстрое сохранение, перезапустите игру и загрузите его.",
      "Der Grafiktreiber reagierte nicht mehr und wurde neu gestartet.\n\nDie moderne Grafik bleibt bis zum Neustart des Spiels aus. Erstelle einen Schnellspeicherstand, starte das Spiel neu und lade ihn.",
      "El controlador de la tarjeta gráfica dejó de responder y se reinició.\n\nLos gráficos modernos seguirán desactivados hasta reiniciar el juego. Haz un guardado rápido, reinicia el juego y cárgalo.",
      "Le pilote de la carte graphique a cessé de répondre et a été redémarré.\n\nLes graphismes modernes restent désactivés jusqu'au redémarrage du jeu. Faites une sauvegarde rapide, redémarrez le jeu et chargez-la.",
      "O driver da placa de vídeo parou de responder e foi reiniciado.\n\nOs gráficos modernos ficam desligados até o jogo ser reiniciado. Faça um salvamento rápido, reinicie o jogo e carregue-o.",
      "グラフィックドライバーが応答しなくなり、再起動されました。\n\nゲームを再起動するまで最新のグラフィックはオフのままです。クイックセーブしてからゲームを再起動し、ロードしてください。",
      "显卡驱动停止响应并已重新启动。\n\n在重新启动游戏之前，现代画面将保持关闭。请快速存档，然后重新启动游戏并读取存档。",
      "Il driver della scheda video ha smesso di rispondere ed è stato reimpostato.\n\nLa grafica moderna resterà disattivata finché non riavvii il gioco. Fai un salvataggio rapido, riavvia il gioco e caricalo." },
    /* 10.10: колесо мыши - вход назначений */
    { "Wheel Up", "Колесо вверх", "Mausrad hoch", "Rueda arriba", "Molette haut", "Roda p/ cima", "ホイール上", "滚轮向上",
      "Rotella su" },
    { "Wheel Down", "Колесо вниз", "Mausrad runter", "Rueda abajo", "Molette bas", "Roda p/ baixo", "ホイール下", "滚轮向下",
      "Rotella giù" },
    { "Touchpad", "Тачпад", "Touchpad", "Panel táctil", "Pavé tactile", "Touchpad", "タッチパッド", "触摸板", "Touchpad" },
    { "Not assigned", "Не назначено", "Nicht belegt", "Sin asignar", "Non attribué", "Não atribuído", "未割り当て", "未分配",
      "Non assegnato" },
    { "Press a key", "Нажмите клавишу", "Taste drücken", "Pulsa una tecla", "Appuyez sur une touche", "Pressione uma tecla",
      "キーを押す", "按下按键", "Premi un tasto" },
    { "Press a button", "Нажмите кнопку", "Taste drücken", "Pulsa un botón", "Appuyez sur un bouton", "Pressione um botão",
      "ボタンを押す", "按下按钮", "Premi un pulsante" },
    /* сообщения */
    { "Press a key or a mouse button - Esc to cancel", "Нажмите клавишу или кнопку мыши - Esc отмена",
      "Taste oder Maustaste drücken - Esc bricht ab", "Pulsa una tecla o un botón del ratón - Esc cancela",
      "Appuyez sur une touche ou un bouton de souris - Échap annule", "Pressione uma tecla ou botão do mouse - Esc cancela",
      "キーかマウスボタンを押してください - Esc で取消", "请按下按键或鼠标按钮 - Esc 取消",
      "Premi un tasto o un pulsante del mouse - Esc per annullare" },
    { "Press a gamepad button - wait 5 s to cancel", "Нажмите кнопку геймпада - отмена через 5 с",
      "Gamepad-Taste drücken - Abbruch nach 5 s", "Pulsa un botón del mando - se cancela en 5 s",
      "Appuyez sur un bouton de la manette - annulation après 5 s", "Pressione um botão do controle - cancela em 5 s",
      "ゲームパッドのボタンを押してください - 5秒で取消", "请按下手柄按钮 - 5 秒后取消",
      "Premi un pulsante del controller - attendi 5 s per annullare" },
    { "Unchanged", "Без изменений", "Unverändert", "Sin cambios", "Inchangé", "Sem alterações", "変更なし", "未更改",
      "Invariato" },
    { "This key cannot be assigned", "Эту клавишу назначить нельзя", "Diese Taste kann nicht belegt werden",
      "Esta tecla no se puede asignar", "Cette touche ne peut pas être attribuée", "Esta tecla não pode ser atribuída",
      "このキーは割り当てられません", "无法分配此按键", "Questo tasto non si può assegnare" },
    { "Assigned", "Назначено", "Belegt", "Asignado", "Attribué", "Atribuído", "割り当てました", "已分配", "Assegnato" },
    { "Assigned - taken from another action", "Назначено - снято с другого действия", "Belegt - von einer anderen Aktion entfernt",
      "Asignado - quitado de otra acción", "Attribué - retiré d'une autre action", "Atribuído - removido de outra ação",
      "割り当てました(他の操作から外しました)", "已分配 - 已从其他动作移除", "Assegnato - tolto da un'altra azione" },
    { "Cleared", "Очищено", "Geleert", "Borrado", "Effacé", "Limpo", "消去しました", "已清除", "Cancellato" },
    { "Restored the default buttons", "Кнопки по умолчанию восстановлены", "Standardbelegung wiederhergestellt",
      "Botones por defecto restaurados", "Touches par défaut rétablies", "Botões padrão restaurados", "初期ボタンに戻しました",
      "已恢复默认按键", "Tasti predefiniti ripristinati" },
    /* 04.10 (6): выбор геймпада («геймпады у людей разные… выбор устройства, если их подключено 2») */
    { "Gamepad Device", "Устройство геймпада", "Gamepad-Gerät", "Mando en uso", "Manette utilisée", "Controle em uso",
      "使用するゲームパッド", "使用的手柄", "Controller in uso" },
    { "Which gamepad to use when several are connected. Auto: the first one connected. The choice is remembered and comes back when the gamepad is plugged in again",
      "Какой геймпад использовать, если подключено несколько. Авто - первый подключённый. Выбор запоминается и вернётся, когда геймпад подключат снова",
      "Welches Gamepad genutzt wird, wenn mehrere angeschlossen sind. Auto: das zuerst angeschlossene. Die Wahl bleibt gespeichert und gilt wieder, sobald das Gamepad erneut angeschlossen wird",
      "Qué mando usar si hay varios conectados. Automático: el primero conectado. La elección se recuerda y vuelve cuando el mando se conecta de nuevo",
      "Quelle manette utiliser quand plusieurs sont branchées. Auto : la première branchée. Le choix est mémorisé et revient quand la manette est rebranchée",
      "Qual controle usar quando há vários conectados. Automático: o primeiro conectado. A escolha é lembrada e volta quando o controle é conectado de novo",
      "複数接続されているときに使うゲームパッド。自動:最初に接続されたもの。選択は記憶され、再接続時にも有効です",
      "连接多个手柄时使用哪一个。自动：最先连接的手柄。选择会被记住，再次连接该手柄时恢复",
      "Quale controller usare se ne sono collegati diversi. Automatico: il primo collegato. La scelta viene ricordata e torna quando il controller viene ricollegato" },
    { "Auto", "Авто", "Auto", "Automático", "Auto", "Automático", "自動", "自动", "Automatico" },
    { "Virtual Gamepad", "Виртуальный геймпад", "Virtuelles Gamepad", "Mando virtual", "Manette virtuelle",
      "Controle virtual", "仮想ゲームパッド", "虚拟手柄", "Controller virtuale" },
    /* названия моделей — как есть */
    { "Xbox 360 Controller", "Xbox 360 Controller", "Xbox 360 Controller", "Xbox 360 Controller", "Xbox 360 Controller",
      "Xbox 360 Controller", "Xbox 360 Controller", "Xbox 360 Controller", "Xbox 360 Controller" },
    { "Xbox Controller", "Xbox Controller", "Xbox Controller", "Xbox Controller", "Xbox Controller", "Xbox Controller",
      "Xbox Controller", "Xbox Controller", "Xbox Controller" },
    { "PS3 Controller", "PS3 Controller", "PS3 Controller", "PS3 Controller", "PS3 Controller", "PS3 Controller",
      "PS3 Controller", "PS3 Controller", "PS3 Controller" },
    { "PS4 Controller", "PS4 Controller", "PS4 Controller", "PS4 Controller", "PS4 Controller", "PS4 Controller",
      "PS4 Controller", "PS4 Controller", "PS4 Controller" },
    { "PS5 Controller", "PS5 Controller", "PS5 Controller", "PS5 Controller", "PS5 Controller", "PS5 Controller",
      "PS5 Controller", "PS5 Controller", "PS5 Controller" },
    { "Switch Pro Controller", "Switch Pro Controller", "Switch Pro Controller", "Switch Pro Controller",
      "Switch Pro Controller", "Switch Pro Controller", "Switch Pro Controller", "Switch Pro Controller",
      "Switch Pro Controller" },
    { "Joy-Con", "Joy-Con", "Joy-Con", "Joy-Con", "Joy-Con", "Joy-Con", "Joy-Con", "Joy-Con", "Joy-Con" },
    { "Luna Controller", "Luna Controller", "Luna Controller", "Luna Controller", "Luna Controller", "Luna Controller",
      "Luna Controller", "Luna Controller", "Luna Controller" },
    { "Stadia Controller", "Stadia Controller", "Stadia Controller", "Stadia Controller", "Stadia Controller",
      "Stadia Controller", "Stadia Controller", "Stadia Controller", "Stadia Controller" },
    { "Shield Controller", "Shield Controller", "Shield Controller", "Shield Controller", "Shield Controller",
      "Shield Controller", "Shield Controller", "Shield Controller", "Shield Controller" },
};

/* Перевод строки меню. На английском (язык 0) возвращает её же — поиск не
 * нужен, поэтому по умолчанию это стоит ровно ничего. */
const char *dc_tr(const char *en) {
    const int L = g_dc_menu_lang;
    if (L <= 0 || L >= DC_LANG_COUNT || !en) return en;
    for (size_t i = 0; i < sizeof s_tr8 / sizeof s_tr8[0]; i++)
        if (strcmp(s_tr8[i][0], en) == 0) return s_tr8[i][L] ? s_tr8[i][L] : en;
    if (L == 1)                          /* скрытые страницы — только русский */
        for (size_t i = 0; i < sizeof s_tr / sizeof s_tr[0]; i++)
            if (strcmp(s_tr[i][0], en) == 0) return s_tr[i][1];
    return en;   /* нет перевода — показываем как есть, а не пустоту */
}

static int s_open;
static int s_page;      /* активная вкладка */
static int s_sel;       /* выбранная строка внутри вкладки */
static int s_scroll;    /* прокрутка списка */

/* Геометрия последнего нарисованного кадра — по ней работает мышь.
 * Заполняется в dc_menu_draw, читается в dc_menu_mouse. */
typedef struct { int x, y, w, h, idx; } DcHit;
static DcHit s_row_hit[64];   int s_row_hits;
static DcHit s_tab_hit[12];   int s_tab_hits;
static int   s_val_x, s_val_x2;   /* зона значения: левая стрелка и правая */

/* Сообщение внизу («Настройки сохранены», «Применится после перезапуска»). */
static char  s_note[128];
static int   s_note_frames;

/* ---- вспомогательное ------------------------------------------------------ */

/* Убрать последний знак строки целиком, не разрывая UTF-8. */
static void utf8_pop(char *s) {
    size_t n = strlen(s);
    while (n > 0) { n--; if (((unsigned char)s[n] & 0xC0) != 0x80) break; }
    s[n] = 0;
}

static int opt_on_page(int i) { return s_opts[i].page == s_page; }
static int opt_selectable(int i) { return s_opts[i].type != DC_OPT_HEADER; }

/* ---- подстраницы и назначения кнопок (04.10 вечер, 6) ------------------------------------------ */
static const char **s_sub_titles;
static int          s_sub_first = -1, s_sub_count;
static int          s_sub_parent = -1, s_sub_parent_sel = -1;   /* откуда открыли подстраницу */
static int          s_cap_opt = -1, s_cap_add;                   /* строка назначения ждёт новую клавишу/кнопку */
static int        (*s_cfg_rd)(const char *, const char *);
static void       (*s_cfg_wr)(FILE *);
static int is_sub(int page) { return s_sub_count > 0 && page >= s_sub_first && page < s_sub_first + s_sub_count; }
void dc_menu_set_subpages(const char **titles, int first_page, int count) {
    s_sub_titles = titles; s_sub_first = first_page; s_sub_count = count;
}
void dc_menu_set_cfg_ext(int (*rd)(const char *, const char *), void (*wr)(FILE *)) { s_cfg_rd = rd; s_cfg_wr = wr; }
static int        (*s_dev_count)(void);
static const char *(*s_dev_label)(int);
void dc_menu_set_dev_hooks(int (*count)(void), const char *(*label)(int)) { s_dev_count = count; s_dev_label = label; }
int  dc_menu_page_parent(void) { return is_sub(s_page) ? s_sub_parent : s_page; }
const char *dc_menu_sub_title(void) { return is_sub(s_page) ? s_sub_titles[s_page - s_sub_first] : NULL; }
int  dc_menu_capturing(void) { return (s_open && s_cap_opt >= 0) ? 1 + (int)(s_opts[s_cap_opt].hi + 0.5f) : 0; }
int  dc_menu_capture_add(void) { return s_cap_add; }
static void sub_leave(void) {
    if (s_cap_opt >= 0) { s_cap_opt = -1; s_note_frames = 0; }   /* ждали клавишу — сообщение «нажмите…» убрать */
    if (!is_sub(s_page)) return;
    s_page = (s_sub_parent >= 0) ? s_sub_parent : 0;
    s_sel = s_sub_parent_sel;
    s_scroll = 0;
}

static int page_first(void) {
    for (int i = 0; i < s_count; i++)
        if (opt_on_page(i) && opt_selectable(i)) return i;
    return -1;
}

static int step_sel(int dir) {
    int i = s_sel + dir;
    while (i >= 0 && i < s_count) {
        if (opt_on_page(i) && opt_selectable(i)) return i;
        i += dir;
    }
    return s_sel;
}

/* Помним и английский ключ сообщения: меню в стиле экранов запуска
 * (dc_splash.cpp) ищет по нему картинку строки в атласе. */
static const char *s_note_key;
static void note_en(const char *en) {
    s_note_key = en;
    snprintf(s_note, sizeof s_note, "%s", dc_tr(en));
    s_note_frames = 180;
}
void dc_menu_note(const char *text_en) { note_en(text_en); }

/* Число опции: процентами, если у строки задан pct (15.09.2026, игрок: «не понятны
 * некоторые» — «1.00», «18.00», «1700» ничего не говорили), иначе целым или с
 * двумя знаками. Знак «%» есть в цифрах атласа меню. */
static void num_text(const DcOpt *o, float v, char *out, size_t n) {
    if (o->pct > 0.0f) {
        const float p = v * o->pct;
        snprintf(out, n, "%d%%", (int)(p + (p < 0 ? -0.5f : 0.5f)));
    } else if (o->type == DC_OPT_INT) {
        snprintf(out, n, "%d", (int)(v + (v < 0 ? -0.5f : 0.5f)));
    } else {
        snprintf(out, n, "%.2f", v);
    }
}

/* Текст значения опции: "Вкл", "1440x1080", "1.25" и т.п. */
static void value_text(const DcOpt *o, char *out, size_t n) {
    float v = o->get ? o->get() : 0.0f;
    switch (o->type) {
    case DC_OPT_BOOL:
        snprintf(out, n, "%s", dc_tr(v >= 0.5f ? "On" : "Off"));
        break;
    case DC_OPT_ENUM: {
        int idx = (int)(v + 0.5f), max = 0;
        while (o->choices && o->choices[max]) max++;
        if (idx < 0) idx = 0;
        if (max && idx >= max) idx = max - 1;
        snprintf(out, n, "%s", (o->choices && max) ? dc_tr(o->choices[idx]) : "-");
        break; }
    case DC_OPT_INT:
    case DC_OPT_FLOAT:
        num_text(o, v, out, n);
        break;
    case DC_OPT_ACTION:
        snprintf(out, n, "%s", ">");
        break;
    case DC_OPT_BIND: {                               /* 04.10 (6): входы через « / » */
        int c[DCB_MAX];
        const int k = dcb_get(BIND_MODE(o), BIND_DEV(o), (int)(o->lo + 0.5f), c);
        out[0] = 0;
        for (int i = 0; i < k; i++) {
            const char *l = dcb_code_label(c[i]);
            size_t u = strlen(out);
            snprintf(out + u, n - u, "%s%s", i ? " / " : "", l ? dc_tr(l) : "?");
        }
        if (!k) snprintf(out, n, "%s", "-");
        break; }
    case DC_OPT_DEVICE: {                             /* 04.10 (6): «2. PS5 Controller» / «Auto» */
        const int slot = (int)(v + 0.5f);
        const char *l = s_dev_label ? s_dev_label(slot) : "Auto";
        if (slot > 0) snprintf(out, n, "%d. %s", slot, dc_tr(l));
        else snprintf(out, n, "%s", dc_tr(l));
        break; }
    default:
        out[0] = 0;
    }
}

/* 13.10.2026: пояснение строки - у ENUM со списком hints своё у каждого значения (режимы ходьбы в прицеле) */
static const char *opt_hint(const DcOpt *o) {
    if (o->type == DC_OPT_ENUM && o->hints && o->get) {
        int cnt = 0;
        while (o->hints[cnt]) cnt++;
        const int idx = (int)(o->get() + 0.5f);
        if (idx >= 0 && idx < cnt) return o->hints[idx];
    }
    return o->hint;
}

static void opt_adjust(const DcOpt *o, int dir) {
    if (!o->get || !o->set) return;
    float v = o->get();
    switch (o->type) {
    case DC_OPT_BOOL:
        v = (v >= 0.5f) ? 0.0f : 1.0f;
        break;
    case DC_OPT_ENUM: {
        int max = 0;
        while (o->choices && o->choices[max]) max++;
        int idx = (int)(v + 0.5f) + dir;
        if (max) { if (idx < 0) idx = max - 1; if (idx >= max) idx = 0; }
        v = (float)idx;
        break; }
    case DC_OPT_INT:
    case DC_OPT_FLOAT:
        v += o->step * (float)dir;
        if (v < o->lo) v = o->lo;
        if (v > o->hi) v = o->hi;
        break;
    case DC_OPT_DEVICE: {                             /* 04.10 (6): Auto и подключённые устройства по кругу */
        const int cnt = s_dev_count ? s_dev_count() : 0;
        int idx = (int)(v + 0.5f) + dir;
        if (idx < 0) idx = cnt; if (idx > cnt) idx = 0;
        v = (float)idx;
        break; }
    default:
        return;
    }
    o->set(v);
    if (o->apply == DC_APPLY_RESTART)      note_en("Applies after a restart");
    else if (o->apply == DC_APPLY_CONFIRM) note_en("Press ENTER to apply");
}

/* ---- файл настроек -------------------------------------------------------- */

static const char *cfg_path(void) { return "dc_user.cfg"; }

/* 18.10.2026 (разбор кода): файл пишется рядом (dc_user.cfg.tmp) и встаёт на место одной операцией (savestate.c) -
 * прежде "w" поверх старого стирал все настройки ещё до записи, и сбой или выход в этот миг оставлял пустой файл. */
extern int dc_file_finish(FILE *f);
extern int dc_replace_file(const char *tmp, const char *dst);
static int menu_write(void) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg_path());
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "version=1\n");
    for (int i = 0; i < s_count; i++) {
        const DcOpt *o = &s_opts[i];
        if (!o->id || o->type == DC_OPT_HEADER || o->type == DC_OPT_ACTION || o->type == DC_OPT_BIND || !o->get) continue;
        fprintf(f, "%s=%.4f\n", o->id, o->get());
    }
    if (s_cfg_wr) s_cfg_wr(f);                        /* 04.10 (6): своя раскладка кнопок */
    if (!dc_file_finish(f) || !dc_replace_file(tmp, cfg_path())) { remove(tmp); return 0; }
    return 1;
}

void dc_menu_save(void) {
    if (!menu_write()) { note_en("Could not write dc_user.cfg"); return; }
    note_en("Settings saved");
}

/* То же без строки «Настройки сохранены»: её показывает только меню, и при
 * сохранении с экрана выбора языка она всплыла бы при следующем открытии. */
void dc_menu_save_quiet(void) {
    if (!menu_write()) fprintf(stderr, "dc_settings: failed to write dc_user.cfg\n");
}

int g_dc_menu_loading = 0;
void dc_menu_load(void) {
    FILE *f = fopen(cfg_path(), "r");
    if (!f) return;
    char line[256];
    int applied = 0;
    g_dc_menu_loading = 1;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        float v = (float)atof(eq + 1);
        int found = 0;
        for (int i = 0; i < s_count; i++) {
            const DcOpt *o = &s_opts[i];
            if (!o->id || !o->set) continue;
            if (strcmp(line, o->id) != 0) continue;
            /* Значения из файла — источник истины: они выигрывают у env,
             * иначе повторится грабля dc_debug.cfg («настройки из bat не
             * работают»). */
            o->set(v);
            applied++;
            found = 1;
            break;
        }
        if (!found && s_cfg_rd) {                     /* 04.10 (6): строки своей раскладки кнопок */
            char *val = eq + 1;
            size_t n = strlen(val);
            while (n && (val[n - 1] == '\n' || val[n - 1] == '\r' || val[n - 1] == ' ')) val[--n] = 0;
            if (s_cfg_rd(line, val)) applied++;
        }
    }
    g_dc_menu_loading = 0;
    fclose(f);
    if (applied)
        fprintf(stderr, "dc_settings: dc_user.cfg - %d values applied\n", applied);
}

/* ---- состояние ------------------------------------------------------------ */

void dc_menu_init(const DcOpt *opts, int count, const char **pages, int page_count) {
    s_opts = opts; s_count = count;
    s_pages = pages; s_page_count = page_count;
    s_page = 0;
    s_sel = page_first();
}

int dc_menu_is_open(void) { return s_open; }

void dc_menu_close(void) {
    if (!s_open) return;
    s_open = 0;
    sub_leave();                                      /* 04.10 (6): открыть снова — на вкладке, а не в подстранице */
    dc_menu_save();
}

void dc_menu_toggle(void) {
    if (s_open) { dc_menu_close(); return; }
    s_open = 1;
    if (s_sel < 0 || s_sel >= s_count || !opt_on_page(s_sel)) s_sel = page_first();
    s_scroll = 0;
}

void dc_menu_open_sub(int page) {
    if (!s_open || !is_sub(page) || page == s_page) return;
    if (!is_sub(s_page)) { s_sub_parent = s_page; s_sub_parent_sel = s_sel; }
    s_page = page;
    s_sel = page_first();
    s_scroll = 0;
    s_cap_opt = -1;
}

/* строка назначения: начать ожидание входа (add — добавить к прежним) */
static void bind_start(int opt, int add) {
    if (opt < 0 || opt >= s_count || s_opts[opt].type != DC_OPT_BIND) return;
    s_cap_opt = opt; s_cap_add = add;
    /* строки сообщений — каждая своим вызовом: menu_strings.py берёт их отсюда в атлас */
    if (BIND_DEV(&s_opts[opt]) == DCB_PAD) note_en("Press a gamepad button - wait 5 s to cancel");
    else note_en("Press a key or a mouse button - Esc to cancel");
    s_note_frames = 100000;                           /* висит, пока ждём */
}

void dc_menu_capture_input(int code) {
    if (s_cap_opt < 0) return;
    const DcOpt *o = &s_opts[s_cap_opt];
    const int act = (int)(o->lo + 0.5f), dev = BIND_DEV(o), add = s_cap_add;
    s_cap_opt = -1;
    if (!code) { note_en("Unchanged"); return; }
    if (!dcb_code_ok(dev, code)) { note_en("This key cannot be assigned"); return; }
    const int moved = dcb_assign(BIND_MODE(o), dev, act, code, add);
    if (moved >= 0) note_en("Assigned - taken from another action");
    else note_en("Assigned");
}

int dc_menu_input(int action) {
    if (!s_open) return 0;
    if (s_cap_opt >= 0) return 1;                     /* 04.10 (6): ждём клавишу — навигация стоит (вход ловит main.cpp) */
    switch (action) {
    case DC_MENU_UP:    s_sel = step_sel(-1); break;
    case DC_MENU_DOWN:  s_sel = step_sel(+1); break;
    case DC_MENU_LEFT:  if (s_sel >= 0) opt_adjust(&s_opts[s_sel], -1); break;
    case DC_MENU_RIGHT: if (s_sel >= 0) opt_adjust(&s_opts[s_sel], +1); break;
    case DC_MENU_OK:
        if (s_sel >= 0 && s_opts[s_sel].type == DC_OPT_ACTION && s_opts[s_sel].set)
            s_opts[s_sel].set(1.0f);
        else if (s_sel >= 0 && s_opts[s_sel].type == DC_OPT_BIND)
            bind_start(s_sel, 0);                     /* 04.10 (6): новая клавиша заменит прежние */
        else if (s_sel >= 0 && s_opts[s_sel].type != DC_OPT_HEADER)
            /* Enter / Cross на ЛЮБОЙ строке что-то делает (2026-09-05, игрок:
             * «чтобы и с геймпада и с клавиатуры можно было … применять»):
             * переключатель — щёлкает, перечисление/число — следующий шаг,
             * как в меню ремейков. Раньше на ENUM/INT/FLOAT кнопка молчала. */
            opt_adjust(&s_opts[s_sel], +1);
        break;
    case DC_MENU_BIND_ADD:
        if (s_sel >= 0 && s_opts[s_sel].type == DC_OPT_BIND) bind_start(s_sel, 1);
        break;
    case DC_MENU_BIND_CLEAR:
        if (s_sel >= 0 && s_opts[s_sel].type == DC_OPT_BIND) {
            dcb_clear(BIND_MODE(&s_opts[s_sel]), BIND_DEV(&s_opts[s_sel]), (int)(s_opts[s_sel].lo + 0.5f));
            note_en("Cleared");
        }
        break;
    case DC_MENU_BACK:
        if (is_sub(s_page)) { sub_leave(); break; }   /* 04.10 (6): с подстраницы — на её вкладку */
        dc_menu_close(); break;
    case DC_MENU_PAGE_PREV:
    case DC_MENU_PAGE_NEXT: {
        if (is_sub(s_page)) break;                    /* на подстранице вкладки не листаются */
        int d = (action == DC_MENU_PAGE_NEXT) ? 1 : -1;
        s_page += d;
        if (s_page < 0) s_page = s_page_count - 1;
        if (s_page >= s_page_count) s_page = 0;
        s_sel = page_first();
        s_scroll = 0;
        break; }
    default: return 0;
    }
    return 1;
}

/* ---- отрисовка ------------------------------------------------------------ */

#define UI_W 640
#define UI_H 480

/* Цвета в формате нашего слоя: 0xAABBGGRR. */
#define COL_BG      0xE6101214u   /* подложка меню */
#define COL_TITLE   0xFFFFFFFFu
#define COL_ITEM    0xFFC8CDC8u
#define COL_ITEM_ON 0xFFFFFFFFu
#define COL_VALUE   0xFFB4E6B4u
#define COL_HINT    0xFF8C968Cu
#define COL_TAB     0xFF8C968Cu
#define COL_TAB_ON  0xFFFFFFFFu
#define COL_LINE    0x50FFFFFFu
#define COL_SELBG   0x40FFFFFFu

/* Стрелка-треугольник высотой h (dir -1 влево, +1 вправо), из горизонтальных
 * полосок нашего слоя: остриё смотрит от значения наружу. */
static void ui_arrow(int x, int y, int h, int dir, uint32_t col) {
    for (int r = 0; r < h; r++) {
        int w = (r <= h / 2) ? (r + 1) : (h - r);
        if (w < 1) w = 1;
        if (dir > 0) dc_ui_rect(x, y + r, w, 1, col);            /* вертикальная грань слева, остриё вправо */
        else         dc_ui_rect(x + (h / 2 + 1) - w, y + r, w, 1, col);   /* остриё влево */
    }
}

void dc_menu_draw(void) {
    if (!s_open || !s_opts) return;

    const int lh = dc_font_line_height();
    const int x0 = 40, x1 = UI_W - 40;

    dc_ui_rect(0, 0, UI_W, UI_H, COL_BG);

    s_row_hits = 0; s_tab_hits = 0; s_val_x = x1 - 120; s_val_x2 = x1 - 46;

    /* Лента вкладок, как в ремейках: L1/R1 или Q/E переключают. Вкладки —
     * заголовки, жирным. Ширина ленты подгоняется под экран (игрок
     * 2026-09-02: «текст обрезался» — с широким шрифтом GAME уезжал за край):
     * промежуток ужимается, а если и так не влезает — обычное начертание. */
    int tabw[12], sumw = 0, use_bold = 1;
    for (int p = 0; p < s_page_count && p < 12; p++) { tabw[p] = dc_font_text_width_b(dc_tr(s_pages[p])); sumw += tabw[p]; }
    int gap = 26;
    if (s_page_count > 1 && sumw + gap * (s_page_count - 1) > x1 - x0) {
        gap = (x1 - x0 - sumw) / (s_page_count - 1);
        if (gap < 10) {
            use_bold = 0; sumw = 0;
            for (int p = 0; p < s_page_count && p < 12; p++) { tabw[p] = dc_font_text_width(dc_tr(s_pages[p])); sumw += tabw[p]; }
            gap = (x1 - x0 - sumw) / (s_page_count - 1);
            if (gap < 6) gap = 6;
        }
    }
    int tx = x0;
    for (int p = 0; p < s_page_count && p < 12; p++) {
        const char *name = dc_tr(s_pages[p]);
        int w = tabw[p];
        if (p == s_page) {
            dc_ui_rect(tx - 6, 22, w + 12, lh + 6, COL_SELBG);
            if (use_bold) dc_ui_text_b(tx, 24, name, COL_TAB_ON, 1); else dc_ui_text(tx, 24, name, COL_TAB_ON, 1);
        } else {
            if (use_bold) dc_ui_text_b(tx, 24, name, COL_TAB, 0); else dc_ui_text(tx, 24, name, COL_TAB, 0);
        }
        if (s_tab_hits < (int)(sizeof s_tab_hit / sizeof s_tab_hit[0])) {
            DcHit h = { tx - 6, 22, w + 12, lh + 6, p };
            s_tab_hit[s_tab_hits++] = h;
        }
        tx += w + gap;
    }
    dc_ui_rect(x0, 22 + lh + 10, x1 - x0, 1, COL_LINE);

    /* Список опций текущей вкладки. Внизу — три строки: подсказка в две
     * строки (перенос по словам, ничего не обрезается) и строка управления. */
    const int list_top = 22 + lh + 22;
    const int list_bot = UI_H - 3 * (lh + 4) - 18;
    const int row = lh + 6;
    const int rows_fit = (list_bot - list_top) / row;

    int visible = 0;
    for (int i = 0; i < s_count; i++) if (opt_on_page(i)) visible++;

    /* прокрутка так, чтобы выбранная строка была на экране */
    int idx_of_sel = 0, k = 0;
    for (int i = 0; i < s_count; i++) {
        if (!opt_on_page(i)) continue;
        if (i == s_sel) idx_of_sel = k;
        k++;
    }
    if (idx_of_sel < s_scroll)                 s_scroll = idx_of_sel;
    if (idx_of_sel >= s_scroll + rows_fit)     s_scroll = idx_of_sel - rows_fit + 1;
    if (s_scroll < 0) s_scroll = 0;

    int y = list_top;
    k = 0;
    for (int i = 0; i < s_count; i++) {
        if (!opt_on_page(i)) continue;
        int slot = k++;
        if (slot < s_scroll || slot >= s_scroll + rows_fit) continue;
        const DcOpt *o = &s_opts[i];

        if (o->type == DC_OPT_HEADER) {
            dc_ui_text(x0, y, dc_tr(o->title), COL_HINT, 0);
            dc_ui_rect(x0, y + lh + 1, x1 - x0, 1, COL_LINE);
            y += row;
            continue;
        }

        if (s_row_hits < (int)(sizeof s_row_hit / sizeof s_row_hit[0])) {
            DcHit h = { x0 - 8, y - 2, (x1 - x0) + 16, lh + 4, i };
            s_row_hit[s_row_hits++] = h;
        }
        int on = (i == s_sel);
        if (on) dc_ui_rect(x0 - 8, y - 2, (x1 - x0) + 16, lh + 4, COL_SELBG);

        /* Значение прижато к правому краю; вокруг него — стрелки-треугольники
         * в своих колонках, которые не наезжают ни на значение, ни на подпись
         * (игрок 2026-09-02: «стрелочки нормальные нарисуй, чтобы в обе стороны
         * работали и не оверлапились с текстом»). Зоны клика = колонки стрелок. */
        char val[64];
        value_text(o, val, sizeof val);
        int vw = dc_font_text_width(val);
        const int aw = lh / 2 + 2;                 /* ширина/высота треугольника */
        const int right_box = x1 - aw - 2;         /* колонка правой стрелки */
        const int vx1 = right_box - 8;             /* правый край значения */
        const int vx0 = vx1 - vw;
        const int left_box = vx0 - 8 - aw;         /* колонка левой стрелки */
        if (on) { s_val_x = left_box - 6; s_val_x2 = right_box - 6; }
        if (on && o->type != DC_OPT_ACTION) {
            ui_arrow(left_box,  y + (lh - aw) / 2, aw, -1, COL_VALUE);
            ui_arrow(right_box, y + (lh - aw) / 2, aw, +1, COL_VALUE);
        }
        dc_ui_text(vx0, y, val, COL_VALUE, on);

        /* подпись — не длиннее, чем до левой стрелки: режем с многоточием */
        {
            char title[128];
            snprintf(title, sizeof title, "%s", dc_tr(o->title));
            int maxw = left_box - 10 - x0;
            if (dc_font_text_width(title) > maxw) {
                size_t n = strlen(title);
                while (n > 1) {
                    n--;
                    while (n > 0 && ((unsigned char)title[n] & 0xC0) == 0x80) n--;   /* не рвать UTF-8 */
                    title[n] = 0;
                    char probe[136]; snprintf(probe, sizeof probe, "%s…", title);
                    if (dc_font_text_width(probe) <= maxw) { snprintf(title, sizeof title, "%s", probe); break; }
                }
            }
            dc_ui_text(x0, y, title, on ? COL_ITEM_ON : COL_ITEM, on);
        }
        y += row;
    }

    if (visible > rows_fit) {
        /* полоска прокрутки */
        int track_h = list_bot - list_top;
        int bar_h = track_h * rows_fit / visible;
        int bar_y = list_top + track_h * s_scroll / visible;
        dc_ui_rect(x1 + 10, list_top, 2, track_h, COL_LINE);
        dc_ui_rect(x1 + 9,  bar_y, 4, bar_h, COL_VALUE);
    }

    /* Подсказка к выбранной строке (до двух строк, перенос по словам — ничего
     * не обрезается) и строка управления. */
    const int hint_y = UI_H - 3 * (lh + 4) - 2;
    dc_ui_rect(x0, hint_y - 10, x1 - x0, 1, COL_LINE);
    if (s_sel >= 0 && s_sel < s_count && opt_hint(&s_opts[s_sel])) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s", dc_tr(opt_hint(&s_opts[s_sel])));
        char line1[256], line2[256];
        line2[0] = 0;
        snprintf(line1, sizeof line1, "%s", buf);
        /* первая строка: отрезаем слова с конца, пока не влезет; в японском и
         * китайском пробелов нет — там режем по знакам (10.09.2026) */
        while (line1[0] && dc_font_text_width(line1) > (x1 - x0)) {
            char *sp = strrchr(line1, ' ');
            if (sp) *sp = 0; else utf8_pop(line1);
        }
        size_t used = strlen(line1);
        if (used < strlen(buf)) {
            const char *rest = buf + used;
            while (*rest == ' ') rest++;
            snprintf(line2, sizeof line2, "%s", rest);
            /* вторая строка: если и она длиннее — режем с многоточием (по словам
             * или по знакам, не разрывая UTF-8) */
            while (line2[0] && dc_font_text_width(line2) > (x1 - x0)) {
                size_t n = strlen(line2);
                if (n >= 3 && strcmp(line2 + n - 3, "…") == 0) line2[n - 3] = 0;
                char *sp = strrchr(line2, ' ');
                if (sp) *sp = 0; else utf8_pop(line2);
                strncat(line2, "…", sizeof line2 - strlen(line2) - 1);
            }
        }
        dc_ui_text(x0, hint_y, line1, COL_HINT, 0);
        if (line2[0]) dc_ui_text(x0, hint_y + lh + 2, line2, COL_HINT, 0);
    }

    const int help_y = hint_y + 2 * (lh + 2) + 2;
    if (s_note_frames > 0) {
        s_note_frames--;
        dc_ui_text(x0, help_y, s_note, COL_VALUE, 0);
    } else {
        /* Строка управления — группы через три пробела. Не влезает (длинные
         * языки; русская не влезала и раньше) — снимаем группы с конца целиком,
         * а не режем посреди слова краем экрана. */
        char help[256];
        snprintf(help, sizeof help, "%s",
                 dc_tr("TAB/Q tabs   arrows/WASD select   ENTER change   ESC back   |   L1/R1   Cross   Circle"));
        while (help[0] && dc_font_text_width(help) > (x1 - x0)) {
            char *cut = NULL;
            for (char *p = strstr(help, "   "); p; p = strstr(p + 1, "   ")) cut = p;
            if (!cut) break;
            *cut = 0;
            size_t n = strlen(help);
            while (n && (help[n - 1] == ' ' || help[n - 1] == '|')) help[--n] = 0;
        }
        dc_ui_text(x0, help_y, help, COL_HINT, 0);
    }
}

/* ---- мышь ------------------------------------------------------------------
 * Наведение подсвечивает строку, щелчок по строке меняет значение (по правой
 * половине — вперёд, по левой — назад, как по стрелкам), щелчок по вкладке
 * переключает её, колесо прокручивает список. */
static int hit_test(const DcHit *a, int n, int x, int y) {
    for (int i = 0; i < n; i++)
        if (x >= a[i].x && x < a[i].x + a[i].w &&
            y >= a[i].y && y < a[i].y + a[i].h) return a[i].idx;
    return -1;
}

int dc_menu_mouse(int x, int y, int click, int wheel) {
    if (!s_open) return 0;

    if (wheel) {
        if (s_cap_opt < 0) s_sel = step_sel(wheel > 0 ? -1 : +1);
        return 1;
    }

    int tab = hit_test(s_tab_hit, s_tab_hits, x, y);
    if (tab >= 0) {
        if (click && tab != s_page) {
            s_page = tab;
            s_sel = page_first();
            s_scroll = 0;
        }
        return 1;
    }

    int row = hit_test(s_row_hit, s_row_hits, x, y);
    if (row >= 0) {
        s_sel = row;                                  /* подсветка по наведению */
        if (click) {
            const DcOpt *o = &s_opts[row];
            if (s_cap_opt >= 0) return 1;                     /* 04.10 (6): ждём клавишу — щелчок ловит main.cpp */
            if (o->type == DC_OPT_BIND)        { bind_start(row, 0); return 1; }
            if (o->type == DC_OPT_ACTION)      { if (o->set) o->set(1.0f); }
            else if (x >= s_val_x2)            opt_adjust(o, +1);  /* колонка правой стрелки */
            else if (x >= s_val_x && x < s_val_x + 26) opt_adjust(o, -1);  /* колонка левой стрелки */
            else if (x > s_val_x)              opt_adjust(o, +1);  /* по самому значению — вперёд */
            else if (o->type == DC_OPT_BOOL)   opt_adjust(o, +1);  /* клик по названию */
        }
        return 1;
    }
    return 1;   /* пока меню открыто, мышь принадлежит ему целиком */
}

/* ---- для меню в стиле экранов запуска (dc_splash.cpp, 10.09.2026) ---------- */

static int s_dev_pad;   /* последним жали геймпад — подсказки внизу под него */

int  dc_menu_page(void) { return s_page; }
int  dc_menu_page_count(void) { return s_page_count; }
const char *dc_menu_page_key(int page) { return (page >= 0 && page < s_page_count) ? s_pages[page] : ""; }
int  dc_menu_sel_opt(void) { return s_sel; }
int  dc_menu_sel_type(void) { return (s_open && s_sel >= 0 && s_sel < s_count) ? (int)s_opts[s_sel].type : -1; }
void dc_menu_set_dev(int pad) { s_dev_pad = pad ? 1 : 0; }
int  dc_menu_dev(void) { return s_dev_pad; }

/* Строки текущей вкладки; значение — ключом (как в value_text, до перевода). */
int dc_menu_rows(DcMenuRow *out, int max) {
    int n = 0;
    for (int i = 0; i < s_count && n < max; i++) {
        if (!opt_on_page(i)) continue;
        const DcOpt *o = &s_opts[i];
        DcMenuRow *r = &out[n++];
        r->opt = i; r->type = o->type; r->title = o->title; r->hint = opt_hint(o);
        r->value_key = NULL; r->value_num[0] = 0; r->choices = NULL;
        r->bind_dev = 0; r->bind_n = 0; r->capturing = 0;
        const float v = o->get ? o->get() : 0.0f;
        r->num_v = v; r->num_lo = o->lo; r->num_hi = o->hi;   /* 11.10: ползунок числовой строки */
        switch (o->type) {
        case DC_OPT_DEVICE: {                                 /* 04.10 (6): «2.» цифрами атласа + подпись типа */
            const int slot = (int)(v + 0.5f);
            r->value_key = s_dev_label ? s_dev_label(slot) : "Auto";
            if (slot > 0) snprintf(r->value_num, sizeof r->value_num, "%d.", slot);
            r->choices = o->choices;                          /* все подписи — колонка под самую широкую */
            break; }
        case DC_OPT_BIND: {                                   /* 04.10 (6): входы действия — коды и подписи */
            r->bind_dev = BIND_DEV(o);
            int c[DCB_MAX];
            const int k = dcb_get(BIND_MODE(o), r->bind_dev, (int)(o->lo + 0.5f), c);
            for (int j = 0; j < k && j < 4; j++) { r->bind_code[j] = c[j]; r->bind_lbl[j] = dcb_code_label(c[j]); }
            r->bind_n = (k < 4) ? k : 4;
            r->capturing = (s_cap_opt == i);
            break; }
        case DC_OPT_BOOL:
            r->value_key = (v >= 0.5f) ? "On" : "Off";
            break;
        case DC_OPT_ENUM: {
            int idx = (int)(v + 0.5f), cnt = 0;
            while (o->choices && o->choices[cnt]) cnt++;
            if (idx < 0) idx = 0;
            if (cnt && idx >= cnt) idx = cnt - 1;
            r->value_key = cnt ? o->choices[idx] : NULL;
            r->choices = o->choices;
            break; }
        case DC_OPT_INT:
        case DC_OPT_FLOAT:
            num_text(o, v, r->value_num, sizeof r->value_num);
            break;
        default:
            break;
        }
    }
    return n;
}

const char *dc_menu_note_key(void) {
    if (s_note_frames <= 0) return NULL;
    s_note_frames--;
    return s_note_key;
}

void dc_menu_hover(int opt) {
    if (s_cap_opt >= 0) return;                       /* 04.10 (6): ждём клавишу — строка не уезжает */
    if (opt >= 0 && opt < s_count && opt_on_page(opt) && opt_selectable(opt)) s_sel = opt;
}

/* Щелчок по строке: как у прежней мыши — стрелки меняют значение назад и
 * вперёд, само значение — вперёд, подпись переключателя — щёлкает его. */
void dc_menu_click(int opt, int dir) {
    if (opt < 0 || opt >= s_count || !opt_on_page(opt) || !opt_selectable(opt)) return;
    if (s_cap_opt >= 0) return;                       /* 04.10 (6): ждём клавишу — щелчок ловит main.cpp */
    s_sel = opt;
    const DcOpt *o = &s_opts[opt];
    if (o->type == DC_OPT_BIND) { bind_start(opt, 0); return; }
    if (o->type == DC_OPT_ACTION) { if (o->set) o->set(1.0f); return; }
    if (dir) opt_adjust(o, dir);
    else if (o->type == DC_OPT_BOOL) opt_adjust(o, +1);
}

/* 11.10.2026: ползунок числовой строки - значение с полосы (мышь), к шагу строки и в её пределы. */
void dc_menu_set_value(int opt, float v) {
    if (opt < 0 || opt >= s_count || !opt_on_page(opt) || !opt_selectable(opt)) return;
    if (s_cap_opt >= 0) return;
    const DcOpt *o = &s_opts[opt];
    if ((o->type != DC_OPT_INT && o->type != DC_OPT_FLOAT) || !o->get || !o->set) return;
    s_sel = opt;
    if (o->step > 0.0f) v = o->lo + floorf((v - o->lo) / o->step + 0.5f) * o->step;
    if (v < o->lo) v = o->lo;
    if (v > o->hi) v = o->hi;
    if (fabsf(v - o->get()) < 1e-4f) return;
    o->set(v);
}

/* ---- «Restore Defaults» вкладки (11.10.2026, игрок: «у каждого раздела сделай кнопку - сбросить до дефолта») -------------
 * Умолчание - то, с чем игра приходит к игроку: настройки установщика (rebuild.ps1 кладёт dc_user.cfg дева в
 * mod\config, установщик - в data и ещё раз в пакет данных программы как embed/dc_user_default.cfg). Установленная
 * игра читает копию из пакета, дев - тот же файл установщика в Installer\_source\mod\config; строки нет - поле def
 * таблицы. Язык не сбрасывается (он выбран на своём экране), выбор геймпада тоже; видимые строки без ключа
 * (Crosshair, Window Size) сбрасывают свою скрытую строку. Ключ отката DC_MENU_RESET_INSTALL=0 - только поле def. */
#if defined(MCM_NO_IMGUI) || defined(DC_EMBEDDED_ASSETS)
const unsigned char *dc_embedded(const char *name, unsigned *size);
#endif
#define DEF_MAX 256
static char  s_def_key[DEF_MAX][32];
static float s_def_val[DEF_MAX];
static int   s_def_n = -1;
static void defaults_parse(const char *t, size_t n) {
    size_t i = 0;
    while (i < n && s_def_n < DEF_MAX) {
        size_t e = i;
        while (e < n && t[e] != '\n') e++;
        char line[96];
        size_t k = e - i; if (k >= sizeof line) k = sizeof line - 1;
        memcpy(line, t + i, k); line[k] = 0;
        i = e + 1;
        char *eq = strchr(line, '=');
        if (!eq || eq == line || (size_t)(eq - line) >= sizeof s_def_key[0]) continue;
        *eq = 0;
        snprintf(s_def_key[s_def_n], sizeof s_def_key[0], "%s", line);
        s_def_val[s_def_n] = (float)atof(eq + 1);
        s_def_n++;
    }
}
static void defaults_load(void) {
    if (s_def_n >= 0) return;
    s_def_n = 0;
    const char *e = getenv("DC_MENU_RESET_INSTALL");
    if (e && *e == '0') { fprintf(stderr, "dc_settings: defaults - the table only (DC_MENU_RESET_INSTALL=0)\n"); return; }
    const char *src = "none";
#if defined(MCM_NO_IMGUI) || defined(DC_EMBEDDED_ASSETS)
    unsigned sz = 0;
    const unsigned char *p = dc_embedded("dc_user_default.cfg", &sz);
    if (p && sz) { defaults_parse((const char *)p, sz); src = "the program (dc_user_default.cfg)"; }
#else
    static const char *const cand[] = { "dc_user_default.cfg", "..\\..\\Installer\\_source\\mod\\config\\dc_user.cfg" };
    for (int c = 0; c < 2 && !s_def_n; c++) {
        FILE *f = fopen(cand[c], "rb");
        if (!f) continue;
        static char buf[16384];
        const size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        defaults_parse(buf, n);
        src = cand[c];
    }
#endif
    fprintf(stderr, "dc_settings: defaults - %d values from %s\n", s_def_n, src);
}
static float default_of(const DcOpt *o, const char *key) {
    for (int i = 0; i < s_def_n; i++)
        if (strcmp(s_def_key[i], key) == 0) return s_def_val[i];
    return o->def;
}
void dc_menu_reset_page(int page) {
    defaults_load();
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        const DcOpt *o = &s_opts[i];
        if (o->page != page) continue;
        if (o->type != DC_OPT_BOOL && o->type != DC_OPT_ENUM && o->type != DC_OPT_INT && o->type != DC_OPT_FLOAT) continue;
        if (o->id && strcmp(o->id, "ui.lang") == 0) continue;
        const char *twin = o->id ? NULL : (strcmp(o->title, "Crosshair") == 0 ? "hud.reticle" :
                                           strcmp(o->title, "Window Size") == 0 ? "scr.res" : NULL);
        /* 15.10: «Movement» показывает выбор текущего типа игры - сбрасываются строки обоих (inp.move.m / inp.move.o);
         * 21.10 вечер - строка называется «Movement Type» */
        const char *twin2 = NULL;
        if (!o->id && strcmp(o->title, "Movement Type") == 0) { twin = "inp.move.m"; twin2 = "inp.move.o"; }
        if (!o->id && !twin) continue;
        for (int k = 0; k < 2; k++) {
            const char *tw = k ? twin2 : twin;
            if (k && !tw) break;
            const DcOpt *t = o;
            if (tw) {
                t = NULL;
                for (int j = 0; j < s_count; j++) if (s_opts[j].id && strcmp(s_opts[j].id, tw) == 0) t = &s_opts[j];
                if (!t) continue;
            }
            if (!t->set) continue;
            const float v = default_of(t, t->id);
            if (!t->get || fabsf(t->get() - v) > 1e-4f) { t->set(v); n++; }
        }
    }
    fprintf(stderr, "dc_settings: page %d back to the defaults, changed %d\n", page, n);
    note_en("Restored the default settings");
}

void dc_menu_set_page(int page) {
    if (page < 0 || page >= s_page_count || page == s_page) return;
    if (s_cap_opt >= 0) return;                       /* 04.10 (6): ждём клавишу */
    if (is_sub(s_page)) sub_leave();                  /* щелчок по вкладке с подстраницы */
    if (page == s_page) return;
    s_page = page;
    s_sel = page_first();
    s_scroll = 0;
}
