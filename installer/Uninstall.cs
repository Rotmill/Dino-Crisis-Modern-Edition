// Dino Crisis ME - uninstaller (11.10.2026, player: "after the game is installed, an uninstall exe in the same folder that
// removes the game").
//
// Uninstall.exe lies in the game folder next to DinoCrisisME.exe (dcme-install.ps1 puts it there). It asks once, then copies
// itself to %TEMP% and starts the copy (a running exe cannot delete itself); the copy waits for the first one to exit and
// removes ONLY what setup put there: DinoCrisisME.exe, SDL2.dll, LICENSE.txt, THIRD-PARTY.txt, install-log.txt,
// Uninstall.exe and the folders data (saves, settings), mods and .build-src. The folder itself goes only if nothing else is
// left in it (someone may have installed into a folder with other files). Then the desktop shortcut "Dino Crisis ME" if
// it points into this folder. The copy in %TEMP% removes itself after it exits.
// Language: the game's own (ui.lang in data\dc_user.cfg: en ru de es fr pt ja zh it), otherwise the Windows language.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows.Forms;

static class Uninstall
{
    // en, ru, de, es, fr, pt, ja, zh, it
    static int lang = 0;
    static readonly string[][] T = {
        new[] { "Remove Dino Crisis ME?\n\nThe game, its saves and mods in this folder will be deleted:",
                "Удалить Dino Crisis ME?\n\nИгра, её сохранения и моды в этой папке будут удалены:",
                "Dino Crisis ME entfernen?\n\nDas Spiel, seine Spielstände und Mods in diesem Ordner werden gelöscht:",
                "¿Desinstalar Dino Crisis ME?\n\nSe eliminarán el juego, sus partidas guardadas y los mods de esta carpeta:",
                "Désinstaller Dino Crisis ME ?\n\nLe jeu, ses sauvegardes et les mods de ce dossier seront supprimés :",
                "Desinstalar o Dino Crisis ME?\n\nO jogo, os saves e os mods desta pasta serão apagados:",
                "Dino Crisis ME をアンインストールしますか?\n\nこのフォルダーのゲーム、セーブデータ、MOD が削除されます:",
                "要卸载 Dino Crisis ME 吗？\n\n此文件夹中的游戏、存档和模组将被删除：",
                "Disinstallare Dino Crisis ME?\n\nIl gioco, i suoi salvataggi e le mod in questa cartella verranno eliminati:" },
        new[] { "Close Dino Crisis ME first.",
                "Сначала закрой Dino Crisis ME.",
                "Schließe zuerst Dino Crisis ME.",
                "Cierra primero Dino Crisis ME.",
                "Ferme d'abord Dino Crisis ME.",
                "Feche o Dino Crisis ME primeiro.",
                "先に Dino Crisis ME を終了してください。",
                "请先关闭 Dino Crisis ME。",
                "Chiudi prima Dino Crisis ME." },
        new[] { "Dino Crisis ME has been removed.",
                "Dino Crisis ME удалена.",
                "Dino Crisis ME wurde entfernt.",
                "Dino Crisis ME se ha desinstalado.",
                "Dino Crisis ME a été désinstallé.",
                "O Dino Crisis ME foi desinstalado.",
                "Dino Crisis ME をアンインストールしました。",
                "Dino Crisis ME 已卸载。",
                "Dino Crisis ME è stato disinstallato." },
        new[] { "Dino Crisis ME has been removed. These could not be deleted (in use?):",
                "Dino Crisis ME удалена. Не удалось удалить (заняты?):",
                "Dino Crisis ME wurde entfernt. Nicht gelöscht (in Benutzung?):",
                "Dino Crisis ME se ha desinstalado. No se pudieron eliminar (¿en uso?):",
                "Dino Crisis ME a été désinstallé. Impossible de supprimer (utilisés ?) :",
                "O Dino Crisis ME foi desinstalado. Não foi possível apagar (em uso?):",
                "Dino Crisis ME をアンインストールしました。削除できなかったもの (使用中?):",
                "Dino Crisis ME 已卸载。以下内容无法删除（正在使用？）：",
                "Dino Crisis ME è stato disinstallato. Impossibile eliminare (in uso?):" },
        new[] { "This is not the Dino Crisis ME folder.",
                "Это не папка Dino Crisis ME.",
                "Das ist nicht der Ordner von Dino Crisis ME.",
                "Esta no es la carpeta de Dino Crisis ME.",
                "Ce n'est pas le dossier de Dino Crisis ME.",
                "Esta não é a pasta do Dino Crisis ME.",
                "これは Dino Crisis ME のフォルダーではありません。",
                "这不是 Dino Crisis ME 的文件夹。",
                "Questa non è la cartella di Dino Crisis ME." },
    };
    const int ASK = 0, CLOSE = 1, DONE = 2, PARTIAL = 3, NOTGAME = 4;
    static string S(int id) { return T[id][lang]; }

    static readonly string[] FILES = { "DinoCrisisME.exe", "SDL2.dll", "LICENSE.txt", "THIRD-PARTY.txt", "install-log.txt", "Uninstall.exe" };
    static readonly string[] DIRS = { "data", "mods", ".build-src" };
    const string TITLE = "Dino Crisis ME";
    static bool quiet = false;   // --quiet: no windows (tests, scripted removal)

    [STAThread]
    static int Main(string[] args)
    {
        foreach (string a in args) if (a == "--quiet") quiet = true;
        if (args.Length >= 3 && args[0] == "--remove") {
            if (args.Length >= 4) int.TryParse(args[3], out lang);
            if (lang < 0 || lang >= T[0].Length) lang = 0;
            int pid; int.TryParse(args[2], out pid);
            return Remove(args[1], pid);
        }
        string dir = Path.GetDirectoryName(Application.ExecutablePath);
        lang = DetectLang(dir);
        if (!File.Exists(Path.Combine(dir, "DinoCrisisME.exe")) || !Directory.Exists(Path.Combine(dir, "data"))) {
            if (!quiet) MessageBox.Show(S(NOTGAME), TITLE, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return 1;
        }
        if (GameRunning(dir)) {
            if (!quiet) MessageBox.Show(S(CLOSE), TITLE, MessageBoxButtons.OK, MessageBoxIcon.Information);
            return 1;
        }
        if (!quiet && MessageBox.Show(S(ASK) + "\n" + dir, TITLE, MessageBoxButtons.YesNo, MessageBoxIcon.Warning,
                                      MessageBoxDefaultButton.Button2) != DialogResult.Yes)
            return 0;
        string tmp = Path.Combine(Path.GetTempPath(), "DinoCrisisME_Uninstall_" + Guid.NewGuid().ToString("N").Substring(0, 8) + ".exe");
        File.Copy(Application.ExecutablePath, tmp, true);
        var psi = new ProcessStartInfo(tmp, "--remove \"" + dir + "\" " + Process.GetCurrentProcess().Id + " " + lang + (quiet ? " --quiet" : ""));
        psi.UseShellExecute = false;
        Process.Start(psi);
        return 0;
    }

    static int Remove(string dir, int pid)
    {
        try { if (pid > 0) Process.GetProcessById(pid).WaitForExit(15000); } catch { }
        Thread.Sleep(300);
        var failed = new List<string>();
        if (File.Exists(Path.Combine(dir, "DinoCrisisME.exe")) || File.Exists(Path.Combine(dir, "Uninstall.exe"))) {
            foreach (string d in DIRS) {
                string p = Path.Combine(dir, d);
                if (Directory.Exists(p) && !Try(() => Directory.Delete(p, true))) failed.Add(p);
            }
            foreach (string f in FILES) {
                string p = Path.Combine(dir, f);
                if (File.Exists(p) && !Try(() => { File.SetAttributes(p, FileAttributes.Normal); File.Delete(p); })) failed.Add(p);
            }
            try {
                if (Directory.Exists(dir) && Directory.GetFileSystemEntries(dir).Length == 0) Directory.Delete(dir, false);
            } catch { }
        }
        RemoveShortcut(dir);
        if (quiet) { }
        else if (failed.Count == 0) MessageBox.Show(S(DONE), TITLE, MessageBoxButtons.OK, MessageBoxIcon.Information);
        else MessageBox.Show(S(PARTIAL) + "\n\n" + string.Join("\n", failed.ToArray()), TITLE, MessageBoxButtons.OK, MessageBoxIcon.Warning);
        // this copy in %TEMP%: removed once it has exited
        try {
            var del = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "cmd.exe"),
                "/c ping 127.0.0.1 -n 3 > nul & del /f /q \"" + Application.ExecutablePath + "\"");
            del.CreateNoWindow = true;
            del.UseShellExecute = false;
            del.WindowStyle = ProcessWindowStyle.Hidden;
            Process.Start(del);
        } catch { }
        return failed.Count == 0 ? 0 : 2;
    }

    // a few tries: an antivirus or Explorer may hold a file for a moment
    static bool Try(Action a)
    {
        for (int i = 0; i < 6; i++) {
            try { a(); return true; } catch { Thread.Sleep(400); }
        }
        return false;
    }

    static bool GameRunning(string dir)
    {
        string exe = Path.Combine(dir, "DinoCrisisME.exe");
        foreach (var p in Process.GetProcessesByName("DinoCrisisME")) {
            try { if (string.Equals(p.MainModule.FileName, exe, StringComparison.OrdinalIgnoreCase)) return true; }
            catch { return true; }   // cannot read it: better ask to close than delete under a running game
        }
        return false;
    }

    static void RemoveShortcut(string dir)
    {
        string exe = Path.Combine(dir, "DinoCrisisME.exe");
        foreach (var folder in new[] { Environment.SpecialFolder.DesktopDirectory, Environment.SpecialFolder.CommonDesktopDirectory }) {
            try {
                string lnk = Path.Combine(Environment.GetFolderPath(folder), "Dino Crisis ME.lnk");
                if (!File.Exists(lnk)) continue;
                if (string.Equals(ShortcutTarget(lnk), exe, StringComparison.OrdinalIgnoreCase)) File.Delete(lnk);
            } catch { }
        }
    }

    static string ShortcutTarget(string lnk)
    {
        Type t = Type.GetTypeFromProgID("WScript.Shell");
        object sh = Activator.CreateInstance(t);
        object sc = t.InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod, null, sh, new object[] { lnk });
        return (string)sc.GetType().InvokeMember("TargetPath", System.Reflection.BindingFlags.GetProperty, null, sc, null);
    }

    static int DetectLang(string dir)
    {
        try {
            foreach (string line in File.ReadAllLines(Path.Combine(dir, "data\\dc_user.cfg"))) {
                if (!line.StartsWith("ui.lang=")) continue;
                double v;
                if (double.TryParse(line.Substring(8), NumberStyles.Float, CultureInfo.InvariantCulture, out v)) {
                    int i = (int)Math.Round(v);
                    if (i >= 0 && i < T[0].Length) return i;
                }
            }
        } catch { }
        string two = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName;
        string[] codes = { "en", "ru", "de", "es", "fr", "pt", "ja", "zh", "it" };
        for (int i = 0; i < codes.Length; i++) if (codes[i] == two) return i;
        return 0;
    }
}
