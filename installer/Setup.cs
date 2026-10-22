// Dino Crisis ME - mod installer.
//
// One window: path to the game disc image, install folder, Install button.
// 08.10.2026: one setup for every supported disc (USA v1.1, the Russian
// translation, later others): the window reads the list from the payload
// (mod/versions/<id>/version.txt) and recognises the chosen image by the sha256
// of its SLUS_009.22 right away, before anything is installed.
// Everything the mod needs (sources, patches, shaders, configs, camera
// collision, the room-extraction scripts) is embedded in this exe as a resource
// and unpacked to a temp folder. installer\dcme-install.ps1 then gets the
// recompiler framework (downloads the archive itself, or takes the one the user
// downloaded by hand), applies the mod patch, pulls the game executable out of
// THE USER'S OWN disc image, recompiles the game, injects the mod hooks, builds
// the runtime, unpacks the rooms and lays out the finished folder.
//
// The window shows a simplified log: steps and their outcomes. The full log,
// including every compiler command, goes to install-log.txt next to the game
// while setup runs, and is deleted when setup succeeds.
//
// English is the default; Russian is a switch in the top-right corner.

using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

public class Setup : Form
{
    TextBox discBox, pathBox, fwUrlBox, fwZipBox;
    Button discBtn, pathBtn, goBtn, closeBtn, fwCopyBtn, fwZipBtn;
    CheckBox fwBox, autoBox, lnkBox;
    ComboBox langBox;
    ProgressBar bar;
    RichTextBox log;
    Label title, discLbl, pathLbl, fwZipLbl, langLbl, status;
    Label verLbl;                    // 08.10.2026: какой образ выбран (или список поддерживаемых)
    Label noteLbl;                   // 08.10.2026: мод любительский, Capcom к нему отношения не имеет
    Label buildHelp;                 // «?» у программ сборки: подсказка по наведению
    ToolTip tip;
    // 10.10.2026: список программ сборки под кнопкой Install - строка на программу, значок и состояние (installed / в
    // составе мода / не установлено / ставится / перезагрузка / не вышло). Состояния шлёт dcme-install.ps1 строками
    // «[tool] ключ состояние»; до установки окно узнаёт их само (компилятор - vswhere, остальное - содержимое пакета).
    class ToolRow { public string Key, Status = "checking"; public Label Icon, Name, State; }
    System.Collections.Generic.Dictionary<string, ToolRow> tools = new System.Collections.Generic.Dictionary<string, ToolRow>();
    TextBox msvcUrlBox;
    Button msvcCopyBtn;
    System.Windows.Forms.Timer spin;
    int spinAngle;
    bool detecting;
    DateTime lastDetect = DateTime.MinValue;
    System.Collections.Generic.List<string> rebootFor = new System.Collections.Generic.List<string>();
    static readonly Font HELPF = new Font("Segoe UI", 9f, FontStyle.Bold);
    Process proc;
    StreamWriter fullLog;
    string workDir;
    int lang = 0;                 // индекс в LANGS; 0 - английский по умолчанию

    // The framework is pinned to one commit: the mod's patch is built against
    // exactly this tree and will refuse to apply to any other.
    const string FW_URL = "https://github.com/mstan/psxrecomp/archive/f69b2dde99813a54cdc9287dd83f7c7fd035b32d.zip";
    // 10.10.2026: the C++ compiler is the one program setup cannot carry (Microsoft does not allow handing it out); this
    // is its page, for installing it by hand
    const string MSVC_URL = "https://visualstudio.microsoft.com/visual-cpp-build-tools/";

    // Long texts: one constant serves both the S(...) call and the key of its translations in TR.
    const string HELP_EN =
        "The game is compiled on your PC from your own disc, and that needs the C++ compiler.\n" +
        "Microsoft does not allow handing it out with mods, so it comes from Microsoft:\n" +
        "automatically via winget, or install it yourself from the address in the window\n" +
        "(the \"Desktop development with C++\" workload).\n" +
        "Ticking \"Install automatically\" means you accept the Microsoft licence.\n" +
        "The finished game does not need it.";
    const string HELP_RU =
        "Игра собирается на твоём компьютере из твоего диска, для этого нужен компилятор C++.\n" +
        "Microsoft не разрешает раздавать его с модами, поэтому он ставится от Microsoft:\n" +
        "автоматически через winget или тобой по адресу в окне\n" +
        "(набор «Разработка классических приложений на C++»).\n" +
        "Галочка «Поставить автоматически» означает, что ты принимаешь лицензию Microsoft.\n" +
        "Готовой игре он не нужен.";
    const string NOMSVC_EN =
        "The C++ compiler is not installed yet.\n\n" +
        "Install it from the address in the window (the \"Desktop development with C++\" workload), " +
        "or tick \"Install automatically\".";
    const string NOMSVC_RU =
        "Компилятор C++ ещё не установлен.\n\n" +
        "Поставь его по адресу в окне (набор «Разработка классических приложений на C++») " +
        "или поставь галочку «Поставить автоматически».";
    const string AUTO_EN = "Install automatically (about 3 GB), accepting the Microsoft licence";
    const string AUTO_RU = "Поставить автоматически (около 3 ГБ) и принять лицензию Microsoft";
    const string MSVC_EN = "C++ compiler (Microsoft Visual Studio Build Tools)";
    const string MSVC_RU = "Компилятор C++ (Microsoft Visual Studio Build Tools)";
    const string REBOOT_EN = "Restart the PC to finish installing {0}.";
    const string REBOOT_RU = "Перезагрузи компьютер, чтобы закончить установку {0}.";

    static readonly Color BG    = Color.FromArgb(28, 28, 30);
    static readonly Color FG    = Color.Gainsboro;
    static readonly Color DIM   = Color.FromArgb(225, 225, 230);
    static readonly Color FIELD = Color.FromArgb(45, 45, 48);
    static readonly Color OKC   = Color.FromArgb(120, 220, 120);
    static readonly Color WARNC = Color.FromArgb(240, 200, 90);
    static readonly Color ERRC  = Color.FromArgb(255, 110, 110);
    static readonly Color STEPC = Color.FromArgb(90, 200, 255);
    // 04.10.2026 (игрок: «сделай заметнее надписи Game disc image и Install to»): два поля, которые обязательно заполнить,
    // подписаны крупнее и жирно, цветом заголовков экранов мода (язык, раскладка, предупреждение)
    static readonly Color ACCENT = Color.FromArgb(110, 228, 208);

    // Языки окна. Английский и русский стоят прямо в вызовах S(...), остальные
    // берутся из таблицы по английской строке как по ключу. Так добавление
    // языка не требует трогать ни одного вызова, а строка без перевода просто
    // остаётся английской - вместо пустоты в окне.
    static readonly string[] LANGS = {
        "English", "Русский", "Deutsch", "Español", "Français",
        "Português (BR)", "日本語", "中文 (简体)", "Italiano"
    };

    string S(string en, string rus)
    {
        if (lang == 0) return en;
        if (lang == 1) return rus;
        string[] v;
        if (TR.TryGetValue(en, out v) && lang - 2 < v.Length && !string.IsNullOrEmpty(v[lang - 2]))
            return v[lang - 2];
        return en;
    }

    // Порядок в каждой строке: de, es, fr, pt, ja, zh, it (20.10.2026).
    static readonly System.Collections.Generic.Dictionary<string, string[]> TR =
        new System.Collections.Generic.Dictionary<string, string[]> {
{ "Dino Crisis ME - mod setup", new[]{
  "Dino Crisis ME - Mod-Installation", "Dino Crisis ME - instalación del mod",
  "Dino Crisis ME - installation du mod", "Dino Crisis ME - instalação do mod",
  "Dino Crisis ME - MOD インストーラー", "Dino Crisis ME - 模组安装程序", "Dino Crisis ME - installazione della mod" } },
{ "Game disc image:", new[]{
  "Spielabbild:", "Imagen del disco:", "Image du disque :", "Imagem do disco:", "ディスクイメージ:", "游戏镜像：", "Immagine del disco:" } },
{ "Build tools", new[]{
  "Build-Werkzeuge", "Herramientas de compilación", "Outils de compilation", "Ferramentas de compilação",
  "ビルドツール", "构建工具", "Strumenti di compilazione" } },
{ HELP_EN, new[]{
  "Das Spiel wird auf deinem PC aus deiner eigenen Disc kompiliert, dafür braucht es den C++-Compiler.\n" +
  "Microsoft erlaubt nicht, ihn mit Mods weiterzugeben, er kommt also von Microsoft:\n" +
  "automatisch per winget oder von dir selbst über die Adresse im Fenster\n" +
  "(Workload \"Desktopentwicklung mit C++\").\n" +
  "Mit dem Haken \"Automatisch installieren\" nimmst du die Microsoft-Lizenz an.\n" +
  "Das fertige Spiel braucht ihn nicht.",
  "El juego se compila en tu PC a partir de tu propio disco, y para eso hace falta el compilador C++.\n" +
  "Microsoft no permite distribuirlo con mods, así que viene de Microsoft:\n" +
  "automáticamente con winget, o instálalo tú desde la dirección de la ventana\n" +
  "(carga de trabajo \"Desarrollo para el escritorio con C++\").\n" +
  "Marcar \"Instalar automáticamente\" significa aceptar la licencia de Microsoft.\n" +
  "El juego terminado no lo necesita.",
  "Le jeu est compilé sur ton PC à partir de ton propre disque, et il faut pour cela le compilateur C++.\n" +
  "Microsoft n'autorise pas sa distribution avec des mods, il vient donc de Microsoft :\n" +
  "automatiquement via winget, ou installe-le toi-même depuis l'adresse de la fenêtre\n" +
  "(charge de travail \"Développement Desktop en C++\").\n" +
  "Cocher \"Installer automatiquement\" revient à accepter la licence Microsoft.\n" +
  "Le jeu terminé n'en a pas besoin.",
  "O jogo é compilado no seu PC a partir do seu próprio disco, e para isso é preciso o compilador C++.\n" +
  "A Microsoft não permite distribuí-lo com mods, então ele vem da Microsoft:\n" +
  "automaticamente via winget, ou instale você mesmo pelo endereço na janela\n" +
  "(carga de trabalho \"Desenvolvimento para desktop com C++\").\n" +
  "Marcar \"Instalar automaticamente\" significa aceitar a licença da Microsoft.\n" +
  "O jogo pronto não precisa dele.",
  "ゲームはお手持ちのディスクからお使いの PC 上でコンパイルされ、そのために C++ コンパイラが必要です。\n" +
  "Microsoft は MOD との同梱を認めていないため、Microsoft から導入します:\n" +
  "winget で自動的に、またはウィンドウのアドレスからご自身で\n" +
  "(ワークロード「C++ によるデスクトップ開発」)。\n" +
  "「自動でインストールする」にチェックを入れると Microsoft のライセンスに同意したことになります。\n" +
  "完成したゲームには不要です。",
  "游戏会用你自己的光盘在你的电脑上编译，为此需要 C++ 编译器。\n" +
  "Microsoft 不允许随模组分发它，因此需从 Microsoft 安装：\n" +
  "通过 winget 自动安装，或按窗口中的地址自行安装\n" +
  "（工作负载“使用 C++ 的桌面开发”）。\n" +
  "勾选“自动安装”即表示你接受 Microsoft 许可协议。\n" +
  "编译好的游戏不需要它。",
  "Il gioco viene compilato sul tuo PC a partire dal tuo disco, e per questo serve il compilatore C++.\n" +
  "Microsoft non permette di distribuirlo con le mod, quindi arriva da Microsoft:\n" +
  "automaticamente tramite winget, oppure installalo tu dall'indirizzo nella finestra\n" +
  "(carico di lavoro \"Sviluppo di applicazioni desktop con C++\").\n" +
  "Spuntare \"Installa automaticamente\" significa accettare la licenza Microsoft.\n" +
  "Il gioco finito non ne ha bisogno." } },
{ MSVC_EN, new[]{
  "C++-Compiler (Microsoft Visual Studio Build Tools)", "Compilador C++ (Microsoft Visual Studio Build Tools)",
  "Compilateur C++ (Microsoft Visual Studio Build Tools)", "Compilador C++ (Microsoft Visual Studio Build Tools)",
  "C++ コンパイラ (Microsoft Visual Studio Build Tools)", "C++ 编译器 (Microsoft Visual Studio Build Tools)",
  "Compilatore C++ (Microsoft Visual Studio Build Tools)" } },
{ AUTO_EN, new[]{
  "Automatisch installieren (ca. 3 GB) und die Microsoft-Lizenz annehmen",
  "Instalar automáticamente (unos 3 GB) y aceptar la licencia de Microsoft",
  "Installer automatiquement (environ 3 Go) et accepter la licence Microsoft",
  "Instalar automaticamente (cerca de 3 GB) e aceitar a licença da Microsoft",
  "自動でインストールする (約 3 GB、Microsoft のライセンスに同意)",
  "自动安装（约 3 GB）并接受 Microsoft 许可协议", "Installa automaticamente (circa 3 GB) accettando la licenza Microsoft" } },
{ "checking...", new[]{ "wird geprüft…", "comprobando…", "vérification…", "verificando…", "確認中…", "正在检查…",
  "verifica in corso…" } },
{ "installed", new[]{ "installiert", "instalado", "installé", "instalado", "インストール済み", "已安装", "installato" } },
{ "not installed - setup will install it", new[]{
  "nicht installiert - das Setup installiert ihn", "no instalado: el instalador lo instalará",
  "non installé - l'installateur l'installera", "não instalado - o instalador vai instalá-lo",
  "未インストール - インストーラーが導入します", "未安装 - 安装程序会安装它", "non installato: lo installerà il setup" } },
{ "not installed - install it from the address below", new[]{
  "nicht installiert - über die Adresse unten installieren", "no instalado: instálalo desde la dirección de abajo",
  "non installé - installe-le depuis l'adresse ci-dessous", "não instalado - instale pelo endereço abaixo",
  "未インストール - 下のアドレスからインストールしてください", "未安装 - 请从下方地址安装", "non installato: installalo dall'indirizzo qui sotto" } },
{ "installing... (10-30 minutes)", new[]{
  "wird installiert… (10-30 Minuten)", "instalando… (10-30 minutos)", "installation… (10-30 minutes)",
  "instalando… (10-30 minutos)", "インストール中… (10-30 分)", "正在安装…（10-30 分钟）", "installazione in corso… (10-30 minuti)" } },
{ "installed - restart the PC after setup", new[]{
  "installiert - nach dem Setup den PC neu starten", "instalado: reinicia el PC al terminar",
  "installé - redémarre le PC après l'installation", "instalado - reinicie o PC depois da instalação",
  "インストール済み - 完了後に PC を再起動してください", "已安装 - 安装完成后请重启电脑", "installato: riavvia il PC al termine" } },
{ "could not install", new[]{
  "Installation fehlgeschlagen", "no se pudo instalar", "échec de l'installation", "não foi possível instalar",
  "インストールできませんでした", "无法安装", "installazione non riuscita" } },
{ NOMSVC_EN, new[]{
  "Der C++-Compiler ist noch nicht installiert.\n\nInstalliere ihn über die Adresse im Fenster (Workload \"Desktopentwicklung mit C++\") oder setze den Haken \"Automatisch installieren\".",
  "El compilador C++ aún no está instalado.\n\nInstálalo desde la dirección de la ventana (carga de trabajo \"Desarrollo para el escritorio con C++\") o marca \"Instalar automáticamente\".",
  "Le compilateur C++ n'est pas encore installé.\n\nInstalle-le depuis l'adresse de la fenêtre (charge de travail \"Développement Desktop en C++\") ou coche \"Installer automatiquement\".",
  "O compilador C++ ainda não está instalado.\n\nInstale pelo endereço na janela (carga de trabalho \"Desenvolvimento para desktop com C++\") ou marque \"Instalar automaticamente\".",
  "C++ コンパイラがまだインストールされていません。\n\nウィンドウのアドレスからインストールするか (ワークロード「C++ によるデスクトップ開発」)、「自動でインストールする」にチェックを入れてください。",
  "C++ 编译器尚未安装。\n\n请按窗口中的地址安装（工作负载“使用 C++ 的桌面开发”），或勾选“自动安装”。",
  "Il compilatore C++ non è ancora installato.\n\n" +
  "Installalo dall'indirizzo nella finestra (carico di lavoro \"Sviluppo di applicazioni desktop con C++\") oppure spunta \"Installa automaticamente\"." } },
{ "Installation complete", new[]{
  "Installation abgeschlossen", "Instalación completada", "Installation terminée", "Instalação concluída",
  "インストール完了", "安装完成", "Installazione completata" } },
{ REBOOT_EN, new[]{
  "Starte den PC neu, um die Installation von {0} abzuschließen.",
  "Reinicia el PC para terminar de instalar {0}.",
  "Redémarre le PC pour terminer l'installation de {0}.",
  "Reinicie o PC para concluir a instalação de {0}.",
  "{0} のインストールを完了するには PC を再起動してください。",
  "请重启电脑以完成 {0} 的安装。", "Riavvia il PC per completare l'installazione di {0}." } },
{ "Play", new[]{ "Spielen", "Jugar", "Jouer", "Jogar", "プレイ", "开始游戏", "Gioca" } },
{ "Open folder", new[]{ "Ordner öffnen", "Abrir carpeta", "Ouvrir le dossier", "Abrir pasta", "フォルダーを開く", "打开文件夹",
  "Apri cartella" } },
{ "Install to:", new[]{ "Installieren nach:", "Instalar en:", "Installer dans :", "Instalar em:", "インストール先:", "安装到：",
  "Installa in:" } },
{ "Browse...", new[]{ "Durchsuchen…", "Examinar…", "Parcourir…", "Procurar…", "参照…", "浏览…", "Sfoglia…" } },
{ "Copy", new[]{ "Kopieren", "Copiar", "Copier", "Copiar", "コピー", "复制", "Copia" } },
{ "Downloaded .zip:", new[]{
  "Geladene .zip:", "Archivo .zip descargado:", "Fichier .zip téléchargé :",
  "Arquivo .zip baixado:", "ダウンロード済みの .zip:", "已下载的 .zip：", "File .zip scaricato:" } },
{ "Download the recompiler framework from the address below", new[]{
  "Recompiler-Gerüst von der Adresse unten laden",
  "Descargar el recompilador de la dirección de abajo",
  "Télécharger le recompilateur à l'adresse ci-dessous",
  "Baixar o recompilador do endereço abaixo",
  "リコンパイラを下のアドレスからダウンロード",
  "从下方地址下载重编译框架", "Scarica il ricompilatore dall'indirizzo qui sotto" } },
{ "Needs the disc's subchannel file next to the image: {0}", new[]{
  "Benötigt die Subchannel-Datei der Disc neben dem Abbild: {0}",
  "Necesita el archivo de subcanal del disco junto a la imagen: {0}",
  "Il faut le fichier de sous-canal du disque à côté de l'image : {0}",
  "Precisa do arquivo de subcanal do disco ao lado da imagem: {0}",
  "イメージと同じフォルダーにディスクのサブチャンネルファイルが必要です: {0}",
  "需要将光盘的子通道文件放在镜像旁边：{0}", "Serve il file del sottocanale del disco accanto all'immagine: {0}" } },
{ "Subchannel file: {0}", new[]{
  "Subchannel-Datei: {0}", "Archivo de subcanal: {0}", "Fichier de sous-canal : {0}", "Arquivo de subcanal: {0}",
  "サブチャンネルファイル: {0}", "子通道文件：{0}", "File del sottocanale: {0}" } },
{ "This disc is protected by LibCrypt: the game checks data kept in the disc's subchannel, and a .bin image does not hold it.\n\nPut the subchannel file of this disc next to the image:\n{0}\n\nIt is the .sbi file of the disc dump, the same one the DuckStation emulator uses.", new[]{
  "Diese Disc ist mit LibCrypt geschützt: Das Spiel prüft Daten im Subchannel der Disc, und ein .bin-Abbild enthält sie nicht.\n\nLege die Subchannel-Datei dieser Disc neben das Abbild:\n{0}\n\nDas ist die .sbi-Datei des Disc-Dumps, dieselbe, die der Emulator DuckStation verwendet.",
  "Este disco está protegido con LibCrypt: el juego comprueba datos del subcanal del disco, y una imagen .bin no los contiene.\n\nPon el archivo de subcanal de este disco junto a la imagen:\n{0}\n\nEs el archivo .sbi del volcado del disco, el mismo que usa el emulador DuckStation.",
  "Ce disque est protégé par LibCrypt : le jeu vérifie des données du sous-canal du disque, et une image .bin ne les contient pas.\n\nPlace le fichier de sous-canal de ce disque à côté de l'image :\n{0}\n\nC'est le fichier .sbi du dump du disque, le même que celui qu'utilise l'émulateur DuckStation.",
  "Este disco é protegido por LibCrypt: o jogo verifica dados do subcanal do disco, e uma imagem .bin não os contém.\n\nColoque o arquivo de subcanal deste disco ao lado da imagem:\n{0}\n\nÉ o arquivo .sbi do dump do disco, o mesmo que o emulador DuckStation usa.",
  "このディスクは LibCrypt で保護されています。ゲームはディスクのサブチャンネルのデータを確認しますが、.bin イメージにはそのデータが含まれていません。\n\nこのディスクのサブチャンネルファイルをイメージと同じフォルダーに置いてください:\n{0}\n\nディスクのダンプの .sbi ファイルで、エミュレーター DuckStation が使うものと同じです。",
  "此光盘受 LibCrypt 保护：游戏会检查光盘子通道中的数据，而 .bin 镜像不包含这些数据。\n\n请将此光盘的子通道文件放在镜像旁边：\n{0}\n\n这是光盘转储的 .sbi 文件，与 DuckStation 模拟器使用的文件相同。",
  "Questo disco è protetto da LibCrypt: il gioco controlla dati conservati nel sottocanale del disco, e un'immagine .bin non li contiene.\n\n" +
  "Metti il file del sottocanale di questo disco accanto all'immagine:\n" +
  "{0}\n\n" +
  "È il file .sbi del dump del disco, lo stesso che usa l'emulatore DuckStation." } },
{ "Recognised: {0}", new[]{
  "Erkannt: {0}", "Reconocida: {0}", "Reconnue : {0}", "Reconhecida: {0}", "認識しました: {0}", "已识别：{0}", "Riconosciuta: {0}" } },
{ "Unknown disc image", new[]{
  "Unbekanntes Abbild", "Imagen desconocida", "Image inconnue", "Imagem desconhecida",
  "不明なディスクイメージです", "未知的光盘镜像", "Immagine sconosciuta" } },
{ "Unknown disc image.", new[]{
  "Unbekanntes Abbild.", "Imagen desconocida.", "Image inconnue.", "Imagem desconhecida.",
  "不明なディスクイメージです。", "未知的光盘镜像。", "Immagine sconosciuta." } },
{ "Fan mod, not made or endorsed by Capcom. Dino Crisis belongs to Capcom: you need your own disc image.", new[]{
  "Fan-Mod, nicht von Capcom erstellt oder unterstützt. Dino Crisis gehört Capcom: du brauchst dein eigenes Abbild.",
  "Mod de fans, no creado ni respaldado por Capcom. Dino Crisis pertenece a Capcom: necesitas tu propia imagen.",
  "Mod de fans, ni créé ni approuvé par Capcom. Dino Crisis appartient à Capcom : il te faut ta propre image.",
  "Mod de fãs, não criado nem endossado pela Capcom. Dino Crisis pertence à Capcom: você precisa da sua imagem.",
  "ファン制作の MOD で、Capcom による制作・承認ではありません。Dino Crisis は Capcom の作品です。ディスクイメージはご自身でご用意ください。",
  "粉丝制作的模组，并非 Capcom 制作或认可。Dino Crisis 归 Capcom 所有，需要你自己的光盘镜像。",
  "Mod di fan, non realizzata né approvata da Capcom. Dino Crisis appartiene a Capcom: ti serve la tua immagine del disco." } },
{ "Install", new[]{ "Installieren", "Instalar", "Installer", "Instalar", "インストール", "安装", "Installa" } },
{ "Installing...", new[]{ "Installiere…", "Instalando…", "Installation…", "Instalando…", "インストール中…", "正在安装…",
  "Installazione…" } },
{ "Close", new[]{ "Schließen", "Cerrar", "Fermer", "Fechar", "閉じる", "关闭", "Chiudi" } },
{ "Ready to install.", new[]{
  "Bereit zur Installation.", "Listo para instalar.", "Prêt à installer.",
  "Pronto para instalar.", "準備完了。", "准备就绪。", "Pronto per l'installazione." } },
{ "Starting...", new[]{ "Starte…", "Iniciando…", "Démarrage…", "Iniciando…", "開始中…", "正在启动…", "Avvio…" } },
{ "Unpacking installer contents...", new[]{
  "Entpacke Inhalt…", "Descomprimiendo el contenido…", "Décompression du contenu…",
  "Descompactando o conteúdo…", "内容を展開中…", "正在解压内容…", "Estrazione dei contenuti…" } },
{ "Mod contents unpacked", new[]{
  "Inhalt entpackt", "Contenido descomprimido", "Contenu décompressé",
  "Conteúdo descompactado", "内容を展開しました", "内容已解压", "Contenuti estratti" } },
{ "Link copied.", new[]{
  "Adresse kopiert.", "Enlace copiado.", "Lien copié.", "Link copiado.",
  "リンクをコピーしました。", "链接已复制。", "Link copiato." } },
{ "Setup finished.", new[]{
  "Installation abgeschlossen.", "Instalación terminada.", "Installation terminée.",
  "Instalação concluída.", "インストールが完了しました。", "安装完成。", "Installazione terminata." } },
{ "Setup failed - see the last lines of the log.", new[]{
  "Fehlgeschlagen - siehe die letzten Zeilen des Protokolls.",
  "Ha fallado: mira las últimas líneas del registro.",
  "Échec - vois les dernières lignes du journal.",
  "Falhou - veja as últimas linhas do registro.",
  "失敗しました。ログの最後の行を確認してください。",
  "安装失败 - 请看日志最后几行。", "Installazione non riuscita: guarda le ultime righe del registro." } },
{ "Setup is still running. Abort?", new[]{
  "Die Installation läuft noch. Abbrechen?", "La instalación sigue en marcha. ¿Cancelar?",
  "L'installation est en cours. Interrompre ?", "A instalação ainda está rodando. Cancelar?",
  "インストールがまだ実行中です。中止しますか？", "安装仍在进行。要中止吗？", "L'installazione è ancora in corso. Annullare?" } },
{ "Error", new[]{ "Fehler", "Error", "Erreur", "Erro", "エラー", "错误", "Errore" } },
{ "Point at the game disc image (the Track 1 .bin file).", new[]{
  "Wähle das Spielabbild (die .bin-Datei von Track 1).",
  "Indica la imagen del juego (el archivo .bin de la pista 1).",
  "Indique l'image du jeu (le fichier .bin de la piste 1).",
  "Aponte para a imagem do jogo (o arquivo .bin da faixa 1).",
  "ゲームのディスクイメージ（トラック 1 の .bin）を指定してください。",
  "请指定游戏镜像（第 1 轨的 .bin 文件）。", "Indica l'immagine del gioco (il file .bin della traccia 1)." } },
{ "Choose where to install.", new[]{
  "Wähle das Installationsziel.", "Elige dónde instalar.", "Choisis où installer.",
  "Escolha onde instalar.", "インストール先を選んでください。", "请选择安装位置。", "Scegli dove installare." } },
{ "The install folder path is too long for the build tools (over 95 characters).\nChoose a shorter one, for example C:\\Games\\DinoCrisisME.", new[]{
  "Der Pfad des Installationsordners ist für die Build-Werkzeuge zu lang (über 95 Zeichen).\nWähle einen kürzeren, zum Beispiel C:\\Games\\DinoCrisisME.",
  "La ruta de la carpeta de instalación es demasiado larga para las herramientas de compilación (más de 95 caracteres).\nElige una más corta, por ejemplo C:\\Games\\DinoCrisisME.",
  "Le chemin du dossier d'installation est trop long pour les outils de compilation (plus de 95 caractères).\nChoisis-en un plus court, par exemple C:\\Games\\DinoCrisisME.",
  "O caminho da pasta de instalação é longo demais para as ferramentas de compilação (mais de 95 caracteres).\nEscolha um mais curto, por exemplo C:\\Games\\DinoCrisisME.",
  "インストール先のパスがビルドツールには長すぎます（95 文字超）。\nC:\\Games\\DinoCrisisME など短いパスを選んでください。",
  "安装文件夹路径对构建工具来说太长（超过 95 个字符）。\n请选择更短的路径，例如 C:\\Games\\DinoCrisisME。",
  "Il percorso della cartella di installazione è troppo lungo per gli strumenti di compilazione (più di 95 caratteri).\n" +
  "Scegline uno più corto, per esempio C:\\Games\\DinoCrisisME." } },
{ "Point at the psxrecomp archive you downloaded, or tick the box above to let setup download it.", new[]{
  "Wähle das geladene psxrecomp-Archiv - oder setze oben den Haken, dann wird es geladen.",
  "Indica el archivo psxrecomp que descargaste, o marca la casilla de arriba para descargarlo.",
  "Indique l'archive psxrecomp téléchargée, ou coche la case au-dessus pour la télécharger.",
  "Aponte para o arquivo psxrecomp que você baixou, ou marque a caixa acima para baixá-lo.",
  "ダウンロードした psxrecomp のアーカイブを指定するか、上のチェックを入れてください。",
  "请指定你下载的 psxrecomp 压缩包，或勾选上面的选项让程序自行下载。",
  "Indica l'archivio psxrecomp che hai scaricato, oppure spunta la casella qui sopra per scaricarlo." } },
{ "The psxrecomp archive you downloaded", new[]{
  "Das geladene psxrecomp-Archiv", "El archivo psxrecomp que descargaste",
  "L'archive psxrecomp téléchargée", "O arquivo psxrecomp que você baixou",
  "ダウンロードした psxrecomp アーカイブ", "你下载的 psxrecomp 压缩包", "L'archivio psxrecomp che hai scaricato" } },
{ "Where to install Dino Crisis ME", new[]{
  "Wohin Dino Crisis ME installiert wird", "Dónde instalar Dino Crisis ME",
  "Où installer Dino Crisis ME", "Onde instalar o Dino Crisis ME",
  "Dino Crisis ME のインストール先", "Dino Crisis ME 的安装位置", "Dove installare Dino Crisis ME" } },
{ "Dino Crisis disc image - Track 1 (.bin) or its .cue", new[]{
  "Dino Crisis Abbild - Track 1 (.bin) oder die .cue", "Imagen de Dino Crisis - pista 1 (.bin) o su .cue",
  "Image de Dino Crisis - piste 1 (.bin) ou son .cue", "Imagem de Dino Crisis - faixa 1 (.bin) ou seu .cue",
  "Dino Crisis のイメージ - トラック 1 (.bin) または .cue", "Dino Crisis 镜像 - 第 1 轨 (.bin) 或其 .cue",
  "Immagine di Dino Crisis - traccia 1 (.bin) o il suo .cue" } },
{ "This installer carries no mod payload.", new[]{
  "Dieser Installer enthält keine Mod-Daten.", "Este instalador no lleva los datos del mod.",
  "Cet installateur ne contient pas les données du mod.", "Este instalador não traz os dados do mod.",
  "このインストーラーに MOD 本体が入っていません。", "此安装程序中没有模组内容。", "Questo programma di installazione non contiene i dati della mod." } },
{ "Installer language", new[]{
  "Sprache des Installers", "Idioma del instalador", "Langue de l'installateur",
  "Idioma do instalador", "インストーラーの言語", "安装程序语言", "Lingua dell'installer" } },
{ "Create a desktop shortcut", new[]{
  "Verknüpfung auf dem Desktop anlegen",
  "Crear un acceso directo en el escritorio",
  "Créer un raccourci sur le bureau",
  "Criar um atalho na área de trabalho",
  "デスクトップにショートカットを作成する",
  "在桌面创建快捷方式", "Crea un collegamento sul desktop" } },
{ "Setup was interrupted.\n\nThe reason is in the last lines of the log.\nFull log: install-log.txt in the install folder.", new[]{
  "Die Installation wurde abgebrochen.\n\nDer Grund steht in den letzten Zeilen des Protokolls.\nVollständig: install-log.txt im Zielordner.",
  "La instalación se interrumpió.\n\nEl motivo está en las últimas líneas del registro.\nRegistro completo: install-log.txt en la carpeta de destino.",
  "L'installation a été interrompue.\n\nLa raison est dans les dernières lignes du journal.\nJournal complet : install-log.txt dans le dossier d'installation.",
  "A instalação foi interrompida.\n\nO motivo está nas últimas linhas do registro.\nRegistro completo: install-log.txt na pasta de destino.",
  "インストールが中断されました。\n\n原因はログの最後の行にあります。\n詳細: インストール先の install-log.txt。",
  "安装已中断。\n\n原因见日志最后几行。\n完整日志：安装目录下的 install-log.txt。",
  "L'installazione si è interrotta.\n\n" +
  "Il motivo è nelle ultime righe del registro.\n" +
  "Registro completo: install-log.txt nella cartella di installazione." } },
        };

    [STAThread]
    public static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new Setup());
    }

    public Setup()
    {
        ClientSize = new Size(740, 682);
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        BackColor = BG; ForeColor = FG;
        Font = new Font("Segoe UI", 9f);

        // Значок окна. /win32icon даёт значок ФАЙЛУ; форме его нужно назначить
        // отдельно, иначе в заголовке и на панели задач останется стандартный.
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

        SetBackground();

        title = MakeLabel("", 20, 14, 460, 32);
        title.Font = new Font("Segoe UI", 15f, FontStyle.Bold);

        langBox = new ComboBox();
        langBox.DropDownStyle = ComboBoxStyle.DropDownList;
        langBox.Items.AddRange(LANGS);
        langBox.SelectedIndex = 0;
        langBox.Location = new Point(610, 32);
        langBox.Size = new Size(110, 24);
        langBox.FlatStyle = FlatStyle.Flat;
        langBox.BackColor = FIELD; langBox.ForeColor = FG;
        langBox.SelectedIndexChanged += (s, e) => { lang = langBox.SelectedIndex; Retranslate(); };
        Controls.Add(langBox);
        langLbl = MakeLabel("", 610, 14, 130, 16, true);

        discLbl = MakeLabel("", 20, 60, 600, 26);
        MakeKeyLabel(discLbl);
        discBox = MakeBox(20, 88, 600);
        discBtn = MakeButton("", 630, 87, 90, 25);
        discBtn.Click += (s, e) => {
            using (var d = new OpenFileDialog()) {
                d.Title  = S("Dino Crisis disc image - Track 1 (.bin) or its .cue",
                             "Образ Dino Crisis - дорожка 1 (.bin) или его .cue");
                d.Filter = S("Disc image (*.bin;*.img;*.iso;*.cue)|*.bin;*.img;*.iso;*.cue|All files (*.*)|*.*",
                             "Образ диска (*.bin;*.img;*.iso;*.cue)|*.bin;*.img;*.iso;*.cue|Все файлы (*.*)|*.*");
                if (d.ShowDialog() == DialogResult.OK) discBox.Text = d.FileName;
            }
        };
        // 08.10.2026: what the chosen image is - right under the field, before anything is installed
        verLbl = MakeLabel("", 20, 114, 700, 32, true);   /* два ряда: список образов растёт */
        discBox.TextChanged += (s, e) => DetectDisc();

        pathLbl = MakeLabel("", 20, 154, 600, 26);
        MakeKeyLabel(pathLbl);
        pathBox = MakeBox(20, 182, 600);
        pathBox.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Games\\DinoCrisisME");
        pathBtn = MakeButton("", 630, 181, 90, 25);
        pathBtn.Click += (s, e) => {
            using (var d = new FolderBrowserDialog()) {
                d.Description = S("Where to install Dino Crisis ME", "Куда установить Dino Crisis ME");
                if (d.ShowDialog() == DialogResult.OK) pathBox.Text = d.SelectedPath;
            }
        };

        // Recompiler framework: setup can fetch it, or the user can download the
        // archive himself and point at the file. The address stays visible and
        // selectable either way - it is somebody else's work, not ours.
        fwBox = MakeCheck(20, 216, 700, true);
        fwBox.CheckedChanged += (s, e) => UpdateFwRow();

        fwUrlBox = MakeBox(20, 242, 600);
        fwUrlBox.ReadOnly = true;
        fwUrlBox.Text = FW_URL;
        fwCopyBtn = MakeButton("", 630, 241, 90, 25);
        fwCopyBtn.Click += (s, e) => {
            try { Clipboard.SetText(FW_URL); status.Text = S("Link copied.", "Ссылка скопирована."); } catch { }
        };

        fwZipLbl = MakeLabel("", 20, 276, 145, 18, true);
        fwZipBox = MakeBox(170, 272, 450);
        fwZipBtn = MakeButton("", 630, 271, 90, 25);
        fwZipBtn.Click += (s, e) => {
            using (var d = new OpenFileDialog()) {
                d.Title  = S("The psxrecomp archive you downloaded", "Скачанный архив psxrecomp");
                d.Filter = S("Archive (*.zip)|*.zip|All files (*.*)|*.*", "Архив (*.zip)|*.zip|Все файлы (*.*)|*.*");
                if (d.ShowDialog() == DialogResult.OK) fwZipBox.Text = d.FileName;
            }
        };

        // 10.10.2026 (player): the "uninstall the build tools afterwards" box is gone - what setup carries is deleted with
        // its temp folder by itself, and the C++ compiler is the player's to keep or remove (dcme-install.ps1 -RemoveTools
        // still does it from the command line)
        lnkBox  = MakeCheck(20, 306, 700, true);

        goBtn = MakeButton("", 20, 334, 150, 32);
        goBtn.BackColor = Color.FromArgb(0, 100, 60);
        goBtn.Click += OnInstall;

        closeBtn = MakeButton("", 180, 334, 100, 32);
        closeBtn.Click += (s, e) => Close();

        bar = new ProgressBar();
        bar.Location = new Point(300, 338);
        bar.Size = new Size(420, 24);
        bar.Maximum = 14;
        Controls.Add(bar);

        // 10.10.2026 (игрок: «вместо строчки Install missing build tools - каждая нужная программа отдельно, ниже кнопки
        // Install, напротив галочка: установлено или нет; у программы - как у рекомпилятора: поставить самому или
        // автоматически, и адрес»; затем: «то, что включено в мод, не показывать, ничего лишнего»): ставить нужно только
        // компилятор C++ (Microsoft не разрешает его раздавать) - его строка с галочкой состояния, «поставить
        // автоматически» и адрес. Python, Vulkan и SDL2 едут в пакете и в окне не упоминаются; с компилятором из пакета
        // (Clang) и планами сборки не видна и строка компилятора (bundledCompiler).
        buildHelp = new Label();
        buildHelp.Size = new Size(18, 18);
        buildHelp.BackColor = Color.Transparent;
        buildHelp.Cursor = Cursors.Help;
        buildHelp.Paint += (s, e) => {
            e.Graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
            using (var pen = new Pen(ACCENT, 1.6f)) e.Graphics.DrawEllipse(pen, 1, 1, 15, 15);
            TextRenderer.DrawText(e.Graphics, "?", HELPF, new Rectangle(0, 0, 18, 18), ACCENT,
                                  TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding);
        };
        Controls.Add(buildHelp);
        tip = new ToolTip();
        tip.AutoPopDelay = 30000; tip.InitialDelay = 150; tip.ReshowDelay = 100; tip.ShowAlways = true;
        tip.ToolTipIcon = ToolTipIcon.Info;
        buildHelp.Click += (s, e) => tip.Show(tip.GetToolTip(buildHelp), buildHelp, 22, 0, 30000);

        AddTool("msvc", 20, 380);
        autoBox = MakeCheck(44, 402, 676, true);
        autoBox.AutoSize = true;          // ширина по тексту: «?» встаёт сразу за ним (PlaceHelp)
        autoBox.CheckedChanged += (s, e) => ShowTools();
        msvcUrlBox = MakeBox(44, 428, 576);
        msvcUrlBox.ReadOnly = true;
        msvcUrlBox.Text = MSVC_URL;
        msvcCopyBtn = MakeButton("", 630, 427, 90, 25);
        msvcCopyBtn.Click += (s, e) => {
            try { Clipboard.SetText(MSVC_URL); status.Text = S("Link copied.", "Ссылка скопирована."); } catch { }
        };
        spin = new System.Windows.Forms.Timer();
        spin.Interval = 60;
        spin.Tick += (s, e) => {
            spinAngle = (spinAngle + 24) % 360;
            foreach (var r in tools.Values) if (r.Status == "installing") r.Icon.Invalidate();
        };
        // компилятор ищется в фоне, когда окно уже на экране; вернулся в окно (поставил сам по адресу) - ищется заново
        Shown += (s, e) => StartDetect();
        Activated += (s, e) => { if ((DateTime.Now - lastDetect).TotalSeconds > 2) StartDetect(); };
        Load += (s, e) => FitToScreen();

        log = new RichTextBox();
        log.Location = new Point(20, 464);
        log.Size = new Size(700, 160);
        log.ReadOnly = true;
        log.BackColor = Color.FromArgb(18, 18, 20);
        log.ForeColor = FG;
        log.Font = new Font("Consolas", 9f);
        log.BorderStyle = BorderStyle.FixedSingle;
        Controls.Add(log);

        status = MakeLabel("", 20, 630, 700, 18, true);
        // 08.10.2026 (licence audit): a fan mod - Capcom has nothing to do with it
        noteLbl = MakeLabel("", 20, 650, 700, 30, true);
        noteLbl.Font = new Font("Segoe UI", 8.25f);

        // 10.10.2026 (player: "let us try the free compiler, so nobody downloads Visual Studio Build Tools"): when the
        // payload carries the compiler (Clang, mod/tools/llvm) and the recorded build plans (mod/build; then CMake is not
        // needed either) there is nothing to install - the compiler row, its tick, the address and the "?" are not shown
        // at all, and the log moves up into their place. The same test as Find-BundledLlvm of dcme-install.ps1.
        bundledCompiler = PayloadHas("mod/tools/llvm/bin/x86_64-w64-mingw32-clang.exe") && PayloadHas("mod/tools/run_plan.py") &&
                          PayloadHas("mod/build/recompiler.plan.json") && PayloadHas("mod/build/game.plan.json");
        if (bundledCompiler) {
            foreach (Control c in new Control[] { tools["msvc"].Icon, tools["msvc"].Name, tools["msvc"].State, autoBox,
                                                  msvcUrlBox, msvcCopyBtn, buildHelp })
                c.Visible = false;
            const int up = 84;                 // the compiler block: 380 -> 464
            log.Top -= up; status.Top -= up; noteLbl.Top -= up;
            ClientSize = new Size(ClientSize.Width, ClientSize.Height - up);
        }

        Retranslate();
        UpdateFwRow();

        FormClosing += (s, e) => {
            if (proc != null && !proc.HasExited) {
                var r = MessageBox.Show(S("Setup is still running. Abort?", "Установка ещё идёт. Прервать?"),
                                        "Dino Crisis ME", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
                if (r == DialogResult.No) { e.Cancel = true; return; }
                try { proc.Kill(); } catch { }
            }
            CloseLog();
        };
    }

    void Retranslate()
    {
        Text  = S("Dino Crisis ME - mod setup", "Dino Crisis ME - установка мода");
        title.Text = "Dino Crisis Modern Edition";
        discLbl.Text = S("Game disc image:", "Образ игры:");
        pathLbl.Text = S("Install to:", "Куда установить:");
        discBtn.Text = pathBtn.Text = fwZipBtn.Text = S("Browse...", "Обзор…");
        // 10.10.2026 (player): no size and no licence name here - the licence is in README and THIRD-PARTY.txt
        fwBox.Text = S("Download the recompiler framework from the address below",
                       "Скачать каркас рекомпилятора по адресу ниже");
        fwCopyBtn.Text = S("Copy", "Копировать");
        fwZipLbl.Text = S("Downloaded .zip:", "Скачанный .zip:");
        autoBox.Text = S(AUTO_EN, AUTO_RU);
        msvcCopyBtn.Text = S("Copy", "Копировать");
        tools["msvc"].Name.Text = S(MSVC_EN, MSVC_RU);
        tip.ToolTipTitle = S("Build tools", "Программы сборки");
        tip.SetToolTip(buildHelp, S(HELP_EN, HELP_RU));
        PlaceHelp();
        ShowTools();
        lnkBox.Text  = S("Create a desktop shortcut", "Создать ярлык на рабочем столе");
        langLbl.Text  = S("Installer language", "Язык установщика");
        goBtn.Text    = S("Install", "Установить");
        closeBtn.Text = S("Close", "Закрыть");
        if (proc == null) status.Text = S("Ready to install.", "Готов к установке.");
        noteLbl.Text = S("Fan mod, not made or endorsed by Capcom. Dino Crisis belongs to Capcom: you need your own disc image.",
                         "Любительский мод: Capcom его не делала и не одобряла. Dino Crisis принадлежит Capcom, нужен свой образ диска.");
        ShowDetect();
        UpdateFwRow();
    }

    // The address is always readable: it is where the framework comes from. Only
    // the "point at the file" line switches on and off.
    void UpdateFwRow()
    {
        bool manual = !fwBox.Checked;
        fwZipLbl.Enabled = manual;
        fwZipBox.Enabled = manual;
        fwZipBtn.Enabled = manual;
        fwZipBox.ForeColor = manual ? FG : DIM;
        fwUrlBox.ForeColor = manual ? DIM : FG;
    }

    // Фон окна - картинка, вшитая в этот exe. Её притемняем: исходник местами
    // почти белый (зубы, чешуя), и по светлым местам серый текст пропадал бы.
    // Не вышло загрузить - остаёмся на ровном тёмном фоне, окно работает как
    // работало.
    void SetBackground()
    {
        try {
            using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream("bg.jpg")) {
                if (s == null) return;
                using (var raw = new Bitmap(s)) {
                    var dark = new Bitmap(raw.Width, raw.Height);
                    using (var g = Graphics.FromImage(dark)) {
                        g.DrawImage(raw, 0, 0, raw.Width, raw.Height);
                        using (var veil = new SolidBrush(Color.FromArgb(215, BG)))
                            g.FillRectangle(veil, 0, 0, raw.Width, raw.Height);
                    }
                    BackgroundImage = dark;
                    BackgroundImageLayout = ImageLayout.Stretch;
                }
            }
        } catch { }
    }

    Label MakeLabel(string text, int x, int y, int w, int h) { return MakeLabel(text, x, y, w, h, false); }
    Label MakeLabel(string text, int x, int y, int w, int h, bool dim)
    {
        var l = new Label();
        l.Text = text; l.Location = new Point(x, y); l.Size = new Size(w, h);
        l.ForeColor = dim ? DIM : FG;
        l.BackColor = Color.Transparent;
        Controls.Add(l);
        return l;
    }

    void MakeKeyLabel(Label l)
    {
        l.Font = new Font("Segoe UI", 11.5f, FontStyle.Bold);
        l.ForeColor = ACCENT;
    }

    // «?» сразу за текстом галочки (текст меняется с языком)
    void PlaceHelp()
    {
        buildHelp.Location = new Point(autoBox.Right + 2, autoBox.Top + (autoBox.Height - buildHelp.Height) / 2);
        buildHelp.BringToFront();
    }

    // ---- 10.10.2026: the program to install --------------------------------------------------------------------------
    // A row: status icon, name, status text right after the name. Only the C++ compiler has one: what comes with the mod
    // is not shown (player: "nothing extra").
    void AddTool(string key, int x, int y)
    {
        var r = new ToolRow();
        r.Key = key;
        r.Icon = new Label();
        r.Icon.Location = new Point(x, y + 1);
        r.Icon.Size = new Size(18, 18);
        r.Icon.BackColor = Color.Transparent;
        r.Icon.Paint += (s, e) => PaintToolIcon(r, e.Graphics);
        Controls.Add(r.Icon);
        r.Name = MakeLabel("", x + 24, y + 2, 10, 18);
        r.Name.AutoSize = true;
        r.State = MakeLabel("", x + 120, y + 2, 10, 18, true);
        r.State.AutoSize = true;
        tools[key] = r;
    }

    void PaintToolIcon(ToolRow r, Graphics g)
    {
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        string st = r.Status;
        if (st == "ok" || st == "installed" || st == "reboot") {
            using (var b = new SolidBrush(OKC)) g.FillEllipse(b, 1, 1, 15, 15);
            using (var p = new Pen(BG, 2.2f)) {
                p.StartCap = p.EndCap = System.Drawing.Drawing2D.LineCap.Round;
                p.LineJoin = System.Drawing.Drawing2D.LineJoin.Round;
                g.DrawLines(p, new[] { new PointF(4.6f, 8.9f), new PointF(7.4f, 11.6f), new PointF(12.4f, 5.9f) });
            }
        } else if (st == "installing") {
            using (var p = new Pen(STEPC, 2.2f)) {
                p.StartCap = p.EndCap = System.Drawing.Drawing2D.LineCap.Round;
                g.DrawArc(p, 2.5f, 2.5f, 12f, 12f, spinAngle, 270);
            }
        } else if (st == "failed") {
            using (var b = new SolidBrush(ERRC)) g.FillEllipse(b, 1, 1, 15, 15);
            using (var p = new Pen(BG, 2.2f)) {
                p.StartCap = p.EndCap = System.Drawing.Drawing2D.LineCap.Round;
                g.DrawLine(p, 5.6f, 5.6f, 11.4f, 11.4f);
                g.DrawLine(p, 11.4f, 5.6f, 5.6f, 11.4f);
            }
        } else if (st == "missing") {
            using (var p = new Pen(WARNC, 1.8f)) g.DrawEllipse(p, 2, 2, 13, 13);
        } else {
            using (var p = new Pen(Color.FromArgb(150, 150, 156), 1.5f)) {
                p.DashStyle = System.Drawing.Drawing2D.DashStyle.Dot;
                g.DrawEllipse(p, 2, 2, 13, 13);
            }
        }
    }

    void ShowTools()
    {
        if (tools.Count == 0) return;
        bool spinning = false;
        foreach (var r in tools.Values) {
            string st = r.Status, text;
            Color c;
            switch (st) {
                case "ok": case "installed": text = S("installed", "установлен"); c = OKC; break;
                case "missing":
                    text = autoBox.Checked ? S("not installed - setup will install it", "не установлен - установщик поставит")
                                           : S("not installed - install it from the address below", "не установлен - поставь по адресу ниже");
                    c = WARNC; break;
                case "installing": text = S("installing... (10-30 minutes)", "ставится… (10-30 минут)"); c = STEPC; spinning = true; break;
                case "reboot":     text = S("installed - restart the PC after setup", "установлен - после установки перезагрузи компьютер"); c = WARNC; break;
                case "failed":     text = S("could not install", "не удалось установить"); c = ERRC; break;
                case "checking":   text = S("checking...", "проверяю…"); c = DIM; break;
                default:           text = ""; c = DIM; break;
            }
            r.State.Text = text;
            r.State.ForeColor = c;
            r.State.Left = r.Name.Right + 6;
            r.Icon.Invalidate();
        }
        if (spinning && !spin.Enabled) spin.Start();
        if (!spinning && spin.Enabled) spin.Stop();
    }

    // a "[tool] key state" line of the setup script, or a find of our own
    void SetTool(string key, string st)
    {
        ToolRow r;
        if (!tools.TryGetValue(key, out r)) return;
        if (st == "reboot") {
            string who = key == "msvc" ? "Visual Studio Build Tools" : r.Name.Text;
            if (!rebootFor.Contains(who)) rebootFor.Add(who);
        }
        r.Status = st;
        ShowTools();
    }

    // The C++ compiler as dcme-install.ps1 looks for it (Find-Msvc): a Visual Studio or Build Tools with the C++ tools.
    static string FindMsvc()
    {
        try {
            string vswhere = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
                                          "Microsoft Visual Studio\\Installer\\vswhere.exe");
            if (!File.Exists(vswhere)) return null;
            var psi = new ProcessStartInfo(vswhere, "-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 " +
                                                    "-property installationPath -utf8");
            psi.UseShellExecute = false;
            psi.CreateNoWindow = true;
            psi.RedirectStandardOutput = true;
            psi.StandardOutputEncoding = Encoding.UTF8;
            using (var p = Process.Start(psi)) {
                string o = p.StandardOutput.ReadToEnd();
                p.WaitForExit(15000);
                foreach (string line in o.Split('\n'))
                    if (line.Trim().Length > 0) return line.Trim();
            }
        } catch { }
        return null;
    }

    // What the payload of this exe carries (entry names may use either slash: ZipFile of older .NET writes backslashes).
    bool bundledCompiler;
    System.Collections.Generic.HashSet<string> payloadNames;
    bool PayloadHas(string entry)
    {
        if (payloadNames == null) {
            payloadNames = new System.Collections.Generic.HashSet<string>(StringComparer.OrdinalIgnoreCase);
            try {
                using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip")) {
                    if (s != null)
                        using (var z = new ZipArchive(s, ZipArchiveMode.Read))
                            foreach (ZipArchiveEntry e in z.Entries) payloadNames.Add(e.FullName.Replace('\\', '/'));
                }
            } catch { }
        }
        return payloadNames.Contains(entry);
    }

    void StartDetect()
    {
        if (detecting || proc != null || bundledCompiler) return;
        detecting = true;
        lastDetect = DateTime.Now;
        if (tools["msvc"].Status == "checking") ShowTools();
        var th = new System.Threading.Thread(() => {
            string vs = FindMsvc();
            try {
                BeginInvoke((Action)(() => {
                    detecting = false;
                    lastDetect = DateTime.Now;
                    ToolRow r = tools["msvc"];
                    // what the setup run said last (installed, restart needed) stays
                    if (proc == null && r.Status != "installed" && r.Status != "reboot")
                        SetTool("msvc", vs != null ? "ok" : "missing");
                }));
            } catch { detecting = false; }
        });
        th.IsBackground = true;
        th.Start();
    }

    // Small screens (1366x768 laptops): the log gives up height first, so the whole window fits.
    void FitToScreen()
    {
        Rectangle wa = Screen.FromControl(this).WorkingArea;
        int over = Height - wa.Height;
        if (over <= 0) return;
        int cut = Math.Min(over, log.Height - 64);
        if (cut <= 0) return;
        log.Height -= cut;
        status.Top -= cut;
        noteLbl.Top -= cut;
        ClientSize = new Size(ClientSize.Width, ClientSize.Height - cut);
        Location = new Point(wa.Left + Math.Max(0, (wa.Width - Width) / 2), wa.Top + Math.Max(0, (wa.Height - Height) / 2));
    }

    TextBox MakeBox(int x, int y, int w)
    {
        var t = new TextBox();
        t.Location = new Point(x, y); t.Size = new Size(w, 24);
        t.BackColor = FIELD; t.ForeColor = FG; t.BorderStyle = BorderStyle.FixedSingle;
        Controls.Add(t);
        return t;
    }

    CheckBox MakeCheck(int x, int y, int w, bool on)
    {
        var c = new CheckBox();
        c.Location = new Point(x, y); c.Size = new Size(w, 22);
        c.Checked = on; c.ForeColor = FG;
        c.BackColor = Color.Transparent;
        Controls.Add(c);
        return c;
    }

    Button MakeButton(string text, int x, int y, int w, int h)
    {
        var b = new Button();
        b.Text = text; b.Location = new Point(x, y); b.Size = new Size(w, h);
        b.FlatStyle = FlatStyle.Flat;
        b.BackColor = Color.FromArgb(60, 60, 64);
        b.ForeColor = FG;
        Controls.Add(b);
        return b;
    }

    void Say(string text, Color c)
    {
        if (log.InvokeRequired) { log.BeginInvoke((Action)(() => Say(text, c))); return; }
        log.SelectionStart = log.TextLength;
        log.SelectionLength = 0;
        log.SelectionColor = c;
        log.AppendText(text + "\n");
        log.ScrollToCaret();
    }

    void CloseLog()
    {
        if (fullLog != null) { try { fullLog.Flush(); fullLog.Close(); } catch { } fullLog = null; }
    }

    // ---- 08.10.2026: which game the chosen image is ----------------------------
    // The supported versions come from the payload: mod/versions/<id>/version.txt (rebuild.ps1, step 8b), lines
    // "key = value". The image is recognised by the sha256 of its executable (SLUS_009.22), read straight from the
    // ISO 9660 root of the image: raw 2352-byte sectors (Mode 2 Form 1, user data at 24) or a plain 2048-byte image.
    class GameVersion { public string Id, Name, NameRu, Exe, Sha, Subchannel; public int Order = 99; }
    System.Collections.Generic.List<GameVersion> versions;
    GameVersion detected;            // the version of the chosen image, null - none
    string detectedFor;              // the path it was worked out for

    System.Collections.Generic.List<GameVersion> Versions()
    {
        if (versions != null) return versions;
        versions = new System.Collections.Generic.List<GameVersion>();
        try {
            using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip")) {
                if (s == null) return versions;
                using (var z = new ZipArchive(s, ZipArchiveMode.Read)) {
                    foreach (ZipArchiveEntry e in z.Entries) {
                        string n = e.FullName.Replace('\\', '/');
                        if (!n.StartsWith("mod/versions/", StringComparison.OrdinalIgnoreCase) ||
                            !n.EndsWith("/version.txt", StringComparison.OrdinalIgnoreCase)) continue;
                        var v = new GameVersion();
                        using (var r = new StreamReader(e.Open(), Encoding.UTF8)) {
                            string line;
                            while ((line = r.ReadLine()) != null) {
                                int eq = line.IndexOf('=');
                                if (eq <= 0 || line.TrimStart().StartsWith("#")) continue;
                                string k = line.Substring(0, eq).Trim(), val = line.Substring(eq + 1).Trim();
                                if (k == "id") v.Id = val;
                                else if (k == "name") v.Name = val;
                                else if (k == "name_ru") v.NameRu = val;
                                else if (k == "exe") v.Exe = val;
                                else if (k == "exe_sha256") v.Sha = val.ToLowerInvariant();
                                else if (k == "order") int.TryParse(val, out v.Order);
                                else if (k == "subchannel") v.Subchannel = val;
                            }
                        }
                        if (v.Id != null && v.Name != null && v.Sha != null) {
                            if (v.Exe == null) v.Exe = "SLUS_009.22";
                            versions.Add(v);
                        }
                    }
                }
            }
        } catch { }
        versions.Sort((a, b) => a.Order.CompareTo(b.Order));
        return versions;
    }

    string VName(GameVersion v) { return (lang == 1 && !string.IsNullOrEmpty(v.NameRu)) ? v.NameRu : v.Name; }

    string VersionList(string sep)
    {
        var parts = new System.Collections.Generic.List<string>();
        foreach (var v in Versions()) parts.Add(VName(v));
        return string.Join(sep, parts.ToArray());
    }

    static string Sha256Hex(byte[] b)
    {
        using (var h = System.Security.Cryptography.SHA256.Create()) {
            var sb = new StringBuilder();
            foreach (byte x in h.ComputeHash(b)) sb.Append(x.ToString("x2"));
            return sb.ToString();
        }
    }

    // A file of the image's root directory, or null (not such an image, or no file of that name).
    static byte[] ReadDiscFile(string path, string want)
    {
        try {
            using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) {
                var head = new byte[16];
                if (fs.Read(head, 0, 16) != 16) return null;
                bool raw = head[0] == 0 && head[11] == 0;
                for (int i = 1; i <= 10 && raw; i++) raw = head[i] == 0xFF;
                long secsize = raw ? 2352 : 2048, dataoff = raw ? 24 : 0;
                Func<long, int, byte[]> read = (lba, len) => {
                    var outb = new byte[len];
                    for (int got = 0; got < len; lba++) {
                        fs.Seek(lba * secsize + dataoff, SeekOrigin.Begin);
                        int n = Math.Min(2048, len - got), r = 0;
                        while (r < n) {
                            int k = fs.Read(outb, got + r, n - r);
                            if (k <= 0) throw new EndOfStreamException();
                            r += k;
                        }
                        got += n;
                    }
                    return outb;
                };
                byte[] pvd = read(16, 2048);
                if (pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' || pvd[4] != '0' || pvd[5] != '1') return null;
                long rootLba = BitConverter.ToUInt32(pvd, 156 + 2);
                long rootLen = BitConverter.ToUInt32(pvd, 156 + 10);
                if (rootLen <= 0 || rootLen > (1 << 20)) return null;
                byte[] dir = read(rootLba, (int)rootLen);
                for (int off = 0; off + 33 < dir.Length; ) {
                    int rlen = dir[off];
                    if (rlen == 0) { off = (off / 2048 + 1) * 2048; continue; }
                    int nl = Math.Min((int)dir[off + 32], dir.Length - off - 33);
                    string name = Encoding.ASCII.GetString(dir, off + 33, nl);
                    int semi = name.IndexOf(';');
                    if (semi >= 0) name = name.Substring(0, semi);
                    if ((dir[off + 25] & 2) == 0 && string.Equals(name, want, StringComparison.OrdinalIgnoreCase)) {
                        long lba = BitConverter.ToUInt32(dir, off + 2);
                        long len = BitConverter.ToUInt32(dir, off + 10);
                        if (len <= 0 || len > (8 << 20)) return null;
                        return read(lba, (int)len);
                    }
                    off += rlen;
                }
            }
        } catch { }
        return null;
    }

    // A .cue names the .bin of every track; Track 1 is the first FILE line.
    static string CueToBin(string cue)
    {
        try {
            foreach (string line in File.ReadAllLines(cue)) {
                string l = line.Trim();
                if (!l.StartsWith("FILE", StringComparison.OrdinalIgnoreCase)) continue;
                int a = l.IndexOf('"'), b = a >= 0 ? l.IndexOf('"', a + 1) : -1;
                string name = (a >= 0 && b > a) ? l.Substring(a + 1, b - a - 1) : l.Substring(4).Trim().Split(' ')[0];
                string bin = Path.Combine(Path.GetDirectoryName(cue), name);
                return File.Exists(bin) ? bin : null;
            }
        } catch { }
        return null;
    }

    // 08.10.2026: the disc's subchannel file (.sbi / .lsd) next to the image, found as the game finds it (cdrom.c,
    // subq_load): "<image>.sbi", "<image without (Track N)>.sbi", the same with .lsd. want - the name to ask for.
    static string FindSubchannel(string bin, out string want)
    {
        string dir = Path.GetDirectoryName(bin) ?? "", stem = Path.GetFileNameWithoutExtension(bin);
        var bases = new System.Collections.Generic.List<string>();
        var m = System.Text.RegularExpressions.Regex.Match(stem, @"^(.*\S) \(Track [^)]+\)$",
                                                          System.Text.RegularExpressions.RegexOptions.IgnoreCase);
        if (m.Success) bases.Add(m.Groups[1].Value);
        bases.Add(stem);
        want = Path.Combine(dir, bases[0] + ".sbi");
        foreach (string b in bases)
            foreach (string ext in new[] { ".sbi", ".lsd" }) {
                string p = Path.Combine(dir, b + ext);
                if (File.Exists(p)) return p;
            }
        return null;
    }

    void DetectDisc()
    {
        string p = discBox.Text.Trim().Trim('"');
        if (p.EndsWith(".cue", StringComparison.OrdinalIgnoreCase) && File.Exists(p)) {
            string bin = CueToBin(p);
            if (bin != null) { discBox.Text = bin; return; }      // TextChanged comes back with the .bin
        }
        if (p != detectedFor) {
            detectedFor = p; detected = null;
            if (p.Length > 0 && File.Exists(p)) {
                var seen = new System.Collections.Generic.Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                foreach (var v in Versions()) {
                    string sha;
                    if (!seen.TryGetValue(v.Exe, out sha)) {
                        byte[] exe = ReadDiscFile(p, v.Exe);
                        sha = exe == null ? "" : Sha256Hex(exe);
                        seen[v.Exe] = sha;
                    }
                    if (sha == v.Sha) { detected = v; break; }
                }
            }
        }
        ShowDetect();
    }

    void ShowDetect()
    {
        if (verLbl == null) return;
        string p = discBox.Text.Trim().Trim('"');
        if (Versions().Count == 0) { verLbl.Text = ""; return; }
        if (detected != null && p == detectedFor) {
            verLbl.Text = string.Format(S("Recognised: {0}", "Опознан: {0}"), VName(detected));
            verLbl.ForeColor = OKC;
            if (detected.Subchannel == "sbi") {                 // 08.10.2026: LibCrypt (Europe)
                string want, sbi = FindSubchannel(p, out want);
                if (sbi != null) {
                    verLbl.Text += "\n" + string.Format(S("Subchannel file: {0}", "Файл служебного канала: {0}"),
                                                         Path.GetFileName(sbi));
                } else {
                    verLbl.Text += "\n" + string.Format(S("Needs the disc's subchannel file next to the image: {0}",
                                                           "Нужен файл служебного канала диска рядом с образом: {0}"),
                                                         Path.GetFileName(want));
                    verLbl.ForeColor = WARNC;
                }
            }
        } else if (p.Length > 0 && File.Exists(p)) {
            // 08.10.2026 (player): no list of the versions here - it is in README
            verLbl.Text = S("Unknown disc image", "Неизвестный образ игры");
            verLbl.ForeColor = WARNC;
        } else {
            verLbl.Text = "";
        }
    }

    // Unpacks the payload embedded in this exe into a temp folder.
    string Unpack()
    {
        string dir = Path.Combine(Path.GetTempPath(), "DinoCrisisME_Setup_" + Guid.NewGuid().ToString("N").Substring(0, 8));
        Directory.CreateDirectory(dir);
        string zip = Path.Combine(dir, "payload.zip");
        using (Stream src = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
        using (FileStream dst = File.Create(zip))
        {
            if (src == null) throw new Exception(S("This installer carries no mod payload.",
                                                   "В установщике нет вшитого содержимого мода."));
            src.CopyTo(dst);
        }
        ZipFile.ExtractToDirectory(zip, dir);
        File.Delete(zip);
        return dir;
    }

    void OnInstall(object sender, EventArgs e)
    {
        if (!File.Exists(discBox.Text)) {
            MessageBox.Show(S("Point at the game disc image (the Track 1 .bin file).",
                              "Укажи файл образа игры (.bin первой дорожки)."),
                            "Dino Crisis ME", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        // 08.10.2026: an image of no supported version - say so now, not after half an hour of building
        DetectDisc();
        if (Versions().Count > 0 && detected == null) {
            MessageBox.Show(S("Unknown disc image.", "Неизвестный образ игры."),
                            "Dino Crisis ME", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        if (detected != null && detected.Subchannel == "sbi") {
            // 08.10.2026: LibCrypt - without the subchannel file the game stops at its first screen
            string want;
            if (FindSubchannel(discBox.Text.Trim().Trim('"'), out want) == null) {
                MessageBox.Show(string.Format(S("This disc is protected by LibCrypt: the game checks data kept in the disc's subchannel, and a .bin image does not hold it.\n\nPut the subchannel file of this disc next to the image:\n{0}\n\nIt is the .sbi file of the disc dump, the same one the DuckStation emulator uses.",
                                                "Этот диск защищён LibCrypt: игра сверяет данные служебного канала диска (subchannel), а в образе .bin их нет.\n\nПоложи файл служебного канала этого диска рядом с образом:\n{0}\n\nЭто файл .sbi от дампа диска, тот же, что берёт эмулятор DuckStation."),
                                              want),
                                "Dino Crisis ME", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
        }
        if (pathBox.Text.Trim().Length == 0) {
            MessageBox.Show(S("Choose where to install.", "Укажи, куда установить."),
                            "Dino Crisis ME", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        // 08.10.2026: the build tree lies under the install folder; trial installs: 95 characters build, 105 do not
        // (CMake's test projects under MSBuild)
        string full = pathBox.Text.Trim();
        try { full = Path.GetFullPath(full); } catch { }
        if (full.Length > 95) {
            MessageBox.Show(S("The install folder path is too long for the build tools (over 95 characters).\nChoose a shorter one, for example C:\\Games\\DinoCrisisME.",
                              "Путь папки установки слишком длинный для программ сборки (больше 95 символов).\nВыбери покороче, например C:\\Games\\DinoCrisisME."),
                            "Dino Crisis ME", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        if (!fwBox.Checked && !File.Exists(fwZipBox.Text.Trim())) {
            MessageBox.Show(S("Point at the psxrecomp archive you downloaded, or tick the box above to let setup download it.",
                              "Укажи скачанный архив psxrecomp - или поставь галочку выше, и установщик скачает его сам."),
                            "Dino Crisis ME", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        // 10.10.2026: the compiler by hand - is it there now (the player may have just installed it from the address)
        if (!autoBox.Checked && !bundledCompiler) {
            string vs = FindMsvc();
            string was = tools["msvc"].Status;
            if (was != "installed" && was != "reboot") SetTool("msvc", vs != null ? "ok" : "missing");
            if (vs == null) {
                MessageBox.Show(S(NOMSVC_EN, NOMSVC_RU), "Dino Crisis ME", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
        }
        rebootFor.Clear();

        log.Clear();
        bar.Value = 0;
        goBtn.Enabled = false;
        goBtn.Text = S("Installing...", "Идёт установка…");
        discBox.Enabled = pathBox.Enabled = discBtn.Enabled = pathBtn.Enabled =
            autoBox.Enabled = lnkBox.Enabled = langBox.Enabled =
            fwBox.Enabled = fwZipBox.Enabled = fwZipBtn.Enabled = false;
        log.Focus();                     // not the address boxes: the focus there selects the whole address
        status.Text = S("Unpacking installer contents...", "Распаковываю содержимое установщика…");

        try { workDir = Unpack(); }
        catch (Exception ex) {
            Say("x " + ex.Message, ERRC);
            goBtn.Enabled = true; goBtn.Text = S("Install", "Установить");
            return;
        }
        Say("> " + S("Mod contents unpacked", "Содержимое мода распаковано"), STEPC);

        try {
            Directory.CreateDirectory(pathBox.Text);
            fullLog = new StreamWriter(Path.Combine(pathBox.Text, "install-log.txt"), false, new UTF8Encoding(true));
            fullLog.AutoFlush = true;
        } catch { fullLog = null; }

        string script = Path.Combine(workDir, "installer\\dcme-install.ps1");
        var args = new StringBuilder();
        args.Append("-NoProfile -ExecutionPolicy Bypass -File \"").Append(script).Append("\"");
        args.Append(" -DiscPath \"").Append(discBox.Text).Append("\"");
        args.Append(" -InstallPath \"").Append(pathBox.Text).Append("\"");
        // Журнал установки существует на двух языках; для остальных - английский.
        args.Append(" -Lang ").Append(lang == 1 ? "ru" : "en");
        if (!fwBox.Checked) args.Append(" -RecompZip \"").Append(fwZipBox.Text.Trim()).Append("\"");
        if (!autoBox.Checked) args.Append(" -SkipPrereq");
        if (!lnkBox.Checked)  args.Append(" -NoShortcut");

        var psi = new ProcessStartInfo();
        psi.FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
                                    "WindowsPowerShell\\v1.0\\powershell.exe");
        psi.Arguments = args.ToString();
        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        psi.StandardOutputEncoding = Encoding.UTF8;
        psi.StandardErrorEncoding = Encoding.UTF8;
        psi.WorkingDirectory = workDir;

        proc = new Process();
        proc.StartInfo = psi;
        proc.EnableRaisingEvents = true;
        proc.OutputDataReceived += OnLine;
        proc.ErrorDataReceived += OnLine;
        proc.Exited += OnExit;

        status.Text = S("Starting...", "Запускаю…");
        proc.Start();
        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();
    }

    // Simplified log: only steps and their outcomes reach the window, everything
    // else goes to the file. Otherwise the window drowns in compiler command lines.
    void OnLine(object sender, DataReceivedEventArgs e)
    {
        if (e.Data == null) return;
        string s = e.Data;
        if (fullLog != null) { try { fullLog.WriteLine(s); } catch { } }

        string t = s.Trim();
        if (t.Length == 0) return;

        // 09.10.2026: after each build tool it installed, the script asks for the window back - the tools' own
        // installer windows cover this one, and a player took setup for closed while it kept going. Not shown.
        if (t == "[front]") { BeginInvoke((Action)BringBack); return; }
        // 10.10.2026: state of a build program for the list under Install (not shown in the log)
        Match tm = Regex.Match(t, @"^\[tool\]\s+(\w+)\s+(\w+)$");
        if (tm.Success) {
            string key = tm.Groups[1].Value, st = tm.Groups[2].Value;
            BeginInvoke((Action)(() => SetTool(key, st)));
            return;
        }

        Match m = Regex.Match(t, @"^==\s*(?:Step|Шаг)\s+(\d+)\s+(?:of|из)\s+(\d+):\s*(.+)$");
        if (m.Success) {
            int n = int.Parse(m.Groups[1].Value), all = int.Parse(m.Groups[2].Value);
            string name = m.Groups[3].Value;
            BeginInvoke((Action)(() => {
                bar.Maximum = all;
                bar.Value = Math.Min(n, all);
                status.Text = name;
            }));
            Say("> " + name, STEPC);
            return;
        }
        if (t.StartsWith("==")) { Say(t.Substring(2).Trim(), STEPC); return; }
        if (t.StartsWith("[ok]")) { Say("    + " + t.Substring(4).Trim(), OKC);   return; }
        if (t.StartsWith("[!]"))  { Say("    ! " + t.Substring(3).Trim(), WARNC); return; }
        if (t.StartsWith("[x]"))  { Say("    x " + t.Substring(3).Trim(), ERRC);  return; }
        // everything else (commands and compiler output) - file only
    }

    // Raise the window above the others once, without keeping it on top.
    void BringBack()
    {
        if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal;
        TopMost = true;
        TopMost = false;
        Activate();
    }

    // 10.10.2026 (player: "the box saying the mod is installed looks like some error - redo it, Completed"): our own dialog
    // instead of the Information box - a green check, "Installation complete", where the game is and how to start it, a
    // note if Windows asked for a restart; Play / Open folder / Close. A failed setup keeps the error box.
    void ShowDone()
    {
        string dir = pathBox.Text.Trim();
        try { dir = Path.GetFullPath(dir); } catch { }
        string exe = Path.Combine(dir, "DinoCrisisME.exe");
        const int W = 600, X = 100, R = 28;
        var f = new Form();
        f.Text = "Dino Crisis ME";
        f.FormBorderStyle = FormBorderStyle.FixedDialog;
        f.MaximizeBox = false; f.MinimizeBox = false; f.ShowInTaskbar = false;
        f.StartPosition = FormStartPosition.CenterParent;
        f.BackColor = BG; f.ForeColor = FG; f.Font = Font;
        try { f.Icon = Icon; } catch { }

        var mark = new Label();
        mark.Location = new Point(30, 26);
        mark.Size = new Size(56, 56);
        mark.Paint += (s, e) => {
            e.Graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
            using (var b = new SolidBrush(OKC)) e.Graphics.FillEllipse(b, 2, 2, 51, 51);
            using (var p = new Pen(BG, 5.5f)) {
                p.StartCap = p.EndCap = System.Drawing.Drawing2D.LineCap.Round;
                p.LineJoin = System.Drawing.Drawing2D.LineJoin.Round;
                e.Graphics.DrawLines(p, new[] { new PointF(16f, 28.5f), new PointF(24.5f, 37f), new PointF(40f, 20f) });
            }
        };
        f.Controls.Add(mark);

        var head = new Label();
        head.Text = S("Installation complete", "Установка завершена");
        head.Font = new Font("Segoe UI", 16f, FontStyle.Bold);
        head.ForeColor = OKC;
        head.AutoSize = true;
        head.Location = new Point(X - 3, 24);
        f.Controls.Add(head);

        Func<string, Color, int, Label> para = (text, color, top) => {
            var l = new Label();
            l.Text = text;
            l.ForeColor = color;
            Size sz = TextRenderer.MeasureText(text, f.Font, new Size(W - X - R, 0), TextFormatFlags.WordBreak);
            l.Location = new Point(X, top);
            l.Size = new Size(W - X - R, sz.Height + 2);
            f.Controls.Add(l);
            return l;
        };
        // 10.10.2026 (player: "a lot of text there too - remove everything extra"): the title, the folder, a restart note
        // only when Windows asked for one, the buttons
        var path = new TextBox();
        path.ReadOnly = true; path.BorderStyle = BorderStyle.None; path.TabStop = false;
        path.BackColor = BG; path.ForeColor = DIM;
        path.Text = dir;
        path.Location = new Point(X, 64);
        path.Width = W - X - R;
        f.Controls.Add(path);
        int y = path.Bottom;
        if (rebootFor.Count > 0)
            y = para(string.Format(S(REBOOT_EN, REBOOT_RU), string.Join(", ", rebootFor.ToArray())), WARNC, y + 10).Bottom;
        y = Math.Max(y + 24, 104);

        Func<string, int, Color, Button> btn = (text, w, back) => {
            var b = new Button();
            b.Text = text;
            b.Size = new Size(w, 32);
            b.FlatStyle = FlatStyle.Flat;
            b.BackColor = back;
            b.ForeColor = FG;
            f.Controls.Add(b);
            return b;
        };
        var close = btn(S("Close", "Закрыть"), 110, Color.FromArgb(60, 60, 64));
        var open  = btn(S("Open folder", "Открыть папку"), 150, Color.FromArgb(60, 60, 64));
        var play  = btn(S("Play", "Играть"), 130, Color.FromArgb(0, 100, 60));
        close.Location = new Point(W - R - close.Width, y);
        open.Location  = new Point(close.Left - 10 - open.Width, y);
        play.Location  = new Point(open.Left - 10 - play.Width, y);
        play.Enabled = File.Exists(exe);
        f.ClientSize = new Size(W, y + 32 + 22);
        f.AcceptButton = play.Enabled ? (IButtonControl)play : close;
        f.CancelButton = close;

        bool launch = false;
        play.Click  += (s, e) => { launch = true; f.Close(); };
        open.Click  += (s, e) => { try { Process.Start("explorer.exe", "\"" + dir + "\""); } catch { } };
        close.Click += (s, e) => f.Close();
        f.ShowDialog(this);
        if (!launch) return;
        try {
            var psi = new ProcessStartInfo(exe);
            psi.WorkingDirectory = dir;
            psi.UseShellExecute = true;
            Process.Start(psi);
            Close();                     // the game is starting - setup has done its job
        } catch (Exception ex) {
            MessageBox.Show(this, ex.Message, "Dino Crisis ME", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    void OnExit(object sender, EventArgs e)
    {
        int code = -1;
        try { code = proc.ExitCode; } catch { }
        BeginInvoke((Action)(() => {
            proc = null;
            CloseLog();
            BringBack();       // the final message must not open behind other windows
            goBtn.Enabled = true; goBtn.Text = S("Install", "Установить");
            discBox.Enabled = pathBox.Enabled = discBtn.Enabled = pathBtn.Enabled =
                autoBox.Enabled = lnkBox.Enabled = langBox.Enabled = fwBox.Enabled = true;
            UpdateFwRow();
            foreach (var r in tools.Values) if (r.Status == "installing") r.Status = code == 0 ? "installed" : "failed";
            ShowTools();
            try { if (Directory.Exists(workDir)) Directory.Delete(workDir, true); } catch { }
            if (code == 0) {
                // The full log is for a failed setup. After a good one nothing
                // of the installer stays next to the game (17.09.2026).
                try { File.Delete(Path.Combine(pathBox.Text, "install-log.txt")); } catch { }
                bar.Value = bar.Maximum;
                status.Text = S("Setup finished.", "Установка завершена.");
                ShowDone();
            } else {
                status.Text = S("Setup failed - see the last lines of the log.",
                                "Установка не удалась - смотри последние строки журнала.");
                MessageBox.Show(this, S("Setup was interrupted.\n\nThe reason is in the last lines of the log.\nFull log: install-log.txt in the install folder.",
                                  "Установка прервалась.\n\nПричина - в последних строках журнала.\nПолный журнал: install-log.txt в папке установки."),
                                S("Error", "Ошибка"), MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }));
    }
}
