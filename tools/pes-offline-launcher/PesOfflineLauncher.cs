// PES 2021 Offline Launcher
// Launcher independente que reproduz o fluxo do BMPES Launcher (sider.exe, espera 1200 ms,
// PES2021.exe pelo caminho salvo em %LOCALAPPDATA%\BMPES Launcher\pes2021-path.txt) e, quando
// nao ha internet, usa o modo offline nativo da Steam (campos WantsOfflineMode e
// SkipOfflineModeWarning do loginusers.vdf) apenas durante a sessao de jogo, restaurando os
// valores originais quando o jogo fecha. Nao altera o jogo, o exe nem DLLs.
//
// Compilar com build.ps1 (csc do .NET Framework 4, C# 5).

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

namespace PesOfflineLauncher
{
    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            bool created;
            using (var mutex = new Mutex(true, "PesOfflineLauncher.SingleInstance", out created))
            {
                if (!created)
                {
                    MessageBox.Show("O PES Offline Launcher ja esta aberto.", LauncherForm.Title,
                        MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return;
                }
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new LauncherForm());
            }
        }
    }

    // ------------------------------------------------------------------ Log

    static class Log
    {
        public static readonly string Dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PES Offline Launcher");
        static readonly object Gate = new object();

        public static void Write(string msg)
        {
            try
            {
                lock (Gate)
                {
                    Directory.CreateDirectory(Dir);
                    string file = Path.Combine(Dir, "launcher.log");
                    if (File.Exists(file) && new FileInfo(file).Length > 512 * 1024)
                    {
                        File.Copy(file, file + ".old", true);
                        File.Delete(file);
                    }
                    File.AppendAllText(file, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "  " + msg + Environment.NewLine);
                }
            }
            catch { }
        }
    }

    // ------------------------------------------------------------------ Caminhos (mesma logica do BMPES Launcher)

    // Nenhum caminho fixo: tudo e descoberto a partir da pasta do proprio exe, do registro da Steam,
    // do libraryfolders.vdf e do pes2021-path.txt do BMPES Launcher.
    static class Paths
    {
        const string PesAppId = "1259970";

        static string bmpesCache;

        // Pastas steamapps de todas as bibliotecas da Steam.
        static IEnumerable<string> SteamAppsDirs()
        {
            var result = new List<string>();
            try
            {
                string main = Path.Combine(Steam.Dir(), "steamapps");
                result.Add(main);
                string lib = Path.Combine(main, "libraryfolders.vdf");
                if (File.Exists(lib))
                    foreach (Match m in Regex.Matches(File.ReadAllText(lib), "\"path\"\\s*\"([^\"]+)\""))
                        result.Add(Path.Combine(m.Groups[1].Value.Replace("\\\\", "\\"), "steamapps"));
            }
            catch (Exception ex) { Log.Write("Falha lendo bibliotecas da Steam: " + ex.Message); }
            return result.Distinct(StringComparer.OrdinalIgnoreCase).Where(Directory.Exists);
        }

        public static string BmpesDir()
        {
            if (bmpesCache != null) return bmpesCache;
            var candidates = new List<string> { AppDomain.CurrentDomain.BaseDirectory };
            string game = GameExeOrNull();
            if (game != null)
            {
                string gameDir = Path.GetDirectoryName(game);
                candidates.Add(gameDir);
                candidates.Add(Path.Combine(Path.GetDirectoryName(gameDir), "BMPES"));
            }
            foreach (string apps in SteamAppsDirs()) candidates.Add(Path.Combine(apps, @"common\BMPES"));
            bmpesCache = candidates.FirstOrDefault(d => File.Exists(Path.Combine(d, "sider.exe")));
            return bmpesCache;
        }

        public static string SiderExe()
        {
            string d = BmpesDir();
            return d == null ? null : Path.Combine(d, "sider.exe");
        }

        public static string GameExe() { return GameExeOrNull(); }

        static string GameExeOrNull()
        {
            try
            {
                string saved = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    @"BMPES Launcher\pes2021-path.txt");
                if (File.Exists(saved))
                {
                    string p = File.ReadAllText(saved).Trim();
                    if (p.Length > 0 && File.Exists(p)) return p;
                }
            }
            catch (Exception ex) { Log.Write("Falha lendo pes2021-path.txt: " + ex.Message); }

            string local = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "PES2021.exe");
            if (File.Exists(local)) return local;

            foreach (string apps in SteamAppsDirs())
            {
                string acf = Path.Combine(apps, "appmanifest_" + PesAppId + ".acf");
                string installDir = "eFootball PES 2021";
                if (File.Exists(acf))
                {
                    var m = Regex.Match(File.ReadAllText(acf), "\"installdir\"\\s*\"([^\"]+)\"");
                    if (m.Success) installDir = m.Groups[1].Value;
                }
                string exe = Path.Combine(apps, "common", installDir, "PES2021.exe");
                if (File.Exists(exe)) return exe;
            }
            return null;
        }
    }

    // ------------------------------------------------------------------ Steam

    static class Steam
    {
        public const long SteamId64Base = 76561197960265728L;

        public static string Dir()
        {
            using (var k = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam"))
            {
                object v = k == null ? null : k.GetValue("SteamPath");
                if (v == null) throw new InvalidOperationException("Steam nao encontrada no registro.");
                return ((string)v).Replace('/', '\\');
            }
        }

        public static string Exe() { return Path.Combine(Dir(), "steam.exe"); }
        public static string LoginUsersVdf() { return Path.Combine(Dir(), @"config\loginusers.vdf"); }

        public static bool IsRunning() { return Process.GetProcessesByName("steam").Length > 0; }

        public static int ActiveUser()
        {
            using (var k = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam\ActiveProcess"))
            {
                object v = k == null ? null : k.GetValue("ActiveUser");
                return v is int ? (int)v : 0;
            }
        }

        // Steam fechada com ActiveUser preso de um encerramento anormal confundiria a espera de login.
        public static void ClearStaleActiveUser()
        {
            if (IsRunning()) return;
            using (var k = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam\ActiveProcess", true))
            {
                if (k != null && k.GetValue("ActiveUser") is int && (int)k.GetValue("ActiveUser") != 0)
                    k.SetValue("ActiveUser", 0, RegistryValueKind.DWord);
            }
        }

        public static bool Shutdown(int timeoutMs)
        {
            if (!IsRunning()) return true;
            Log.Write("Encerrando a Steam (-shutdown).");
            try { Process.Start(Exe(), "-shutdown"); } catch (Exception ex) { Log.Write("-shutdown falhou: " + ex.Message); }
            var sw = Stopwatch.StartNew();
            while (IsRunning() && sw.ElapsedMilliseconds < timeoutMs) Thread.Sleep(500);
            if (!IsRunning()) { Thread.Sleep(1000); return true; }
            return false;
        }

        public static void Kill()
        {
            Log.Write("Steam nao respondeu ao -shutdown. Finalizando processos da Steam.");
            foreach (string name in new[] { "steam", "steamwebhelper", "steamservice" })
                foreach (var p in Process.GetProcessesByName(name))
                {
                    try { p.Kill(); p.WaitForExit(5000); } catch { }
                }
            Thread.Sleep(1500);
        }

        public static void Start()
        {
            Log.Write("Iniciando a Steam (-silent).");
            Process.Start(new ProcessStartInfo(Exe(), "-silent") { WorkingDirectory = Dir() });
        }

        public static bool WaitLogin(int timeoutMs)
        {
            var sw = Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < timeoutMs)
            {
                if (IsRunning() && ActiveUser() != 0) return true;
                Thread.Sleep(500);
            }
            return false;
        }
    }

    // ------------------------------------------------------------------ loginusers.vdf

    sealed class VdfAccount
    {
        public string SteamId;
        public int BlockIndex;
        public int BlockLength;
        public string Block;

        public string Get(string key)
        {
            var m = Regex.Match(Block, "\"" + key + "\"\\s*\"([^\"]*)\"");
            return m.Success ? m.Groups[1].Value : null;
        }
    }

    static class LoginUsers
    {
        static readonly Regex UserBlock = new Regex("\"(\\d{17})\"\\s*\\{([^{}]*)\\}");

        public static string Read() { return File.ReadAllText(Steam.LoginUsersVdf(), Encoding.UTF8); }

        public static VdfAccount FindAccount(string text, string steamId)
        {
            var list = new List<VdfAccount>();
            foreach (Match m in UserBlock.Matches(text))
                list.Add(new VdfAccount
                {
                    SteamId = m.Groups[1].Value,
                    BlockIndex = m.Groups[2].Index,
                    BlockLength = m.Groups[2].Length,
                    Block = m.Groups[2].Value
                });
            if (steamId != null) return list.FirstOrDefault(a => a.SteamId == steamId);
            var auto = list.FirstOrDefault(a => a.Get("AutoLogin") == "1");
            if (auto != null) return auto;
            return list.Count == 1 ? list[0] : null;
        }

        // Grava WantsOfflineMode e SkipOfflineModeWarning so no bloco da conta indicada. Steam precisa estar fechada.
        public static void SetOffline(string steamId, string wants, string skip)
        {
            if (Steam.IsRunning()) throw new InvalidOperationException("A Steam precisa estar fechada para editar o loginusers.vdf.");
            string path = Steam.LoginUsersVdf();
            string text = Read();
            var acc = FindAccount(text, steamId);
            if (acc == null) throw new InvalidOperationException("Conta " + steamId + " nao encontrada no loginusers.vdf.");

            string backup = Path.Combine(Log.Dir, "loginusers.vdf.bak");
            Directory.CreateDirectory(Log.Dir);
            File.Copy(path, backup, true);

            string block = acc.Block;
            block = SetKey(block, "WantsOfflineMode", wants);
            block = SetKey(block, "SkipOfflineModeWarning", skip);
            string updated = text.Substring(0, acc.BlockIndex) + block + text.Substring(acc.BlockIndex + acc.BlockLength);

            string tmp = path + ".pesoffline.tmp";
            File.WriteAllText(tmp, updated, new UTF8Encoding(false));
            File.Copy(tmp, path, true);
            File.Delete(tmp);
            Log.Write("loginusers.vdf [" + steamId + "] WantsOfflineMode=" + wants + " SkipOfflineModeWarning=" + skip);
        }

        static string SetKey(string block, string key, string value)
        {
            if (value == null) // valor original ausente: remover a chave
                return Regex.Replace(block, "\\r?\\n?[ \\t]*\"" + key + "\"\\s*\"[^\"]*\"", "");
            var rx = new Regex("(\"" + key + "\"\\s*\")[^\"]*(\")");
            if (rx.IsMatch(block)) return rx.Replace(block, "${1}" + value + "${2}", 1);
            return block.TrimEnd() + "\n\t\t\"" + key + "\"\t\t\"" + value + "\"\n\t";
        }
    }

    // ------------------------------------------------------------------ Estado de restauracao pendente

    sealed class PendingState
    {
        public string SteamId, Wants, Skip;
        static string File_ { get { return Path.Combine(Log.Dir, "restaurar-steam.txt"); } }

        public static PendingState Load()
        {
            try
            {
                if (!File.Exists(File_)) return null;
                var d = File.ReadAllLines(File_).Where(l => l.Contains("="))
                    .ToDictionary(l => l.Substring(0, l.IndexOf('=')), l => l.Substring(l.IndexOf('=') + 1));
                return new PendingState
                {
                    SteamId = d["SteamId"],
                    Wants = d["WantsOfflineMode"] == "(ausente)" ? null : d["WantsOfflineMode"],
                    Skip = d["SkipOfflineModeWarning"] == "(ausente)" ? null : d["SkipOfflineModeWarning"]
                };
            }
            catch (Exception ex) { Log.Write("Estado pendente ilegivel: " + ex.Message); return null; }
        }

        public void Save()
        {
            Directory.CreateDirectory(Log.Dir);
            File.WriteAllLines(File_, new[] {
                "SteamId=" + SteamId,
                "WantsOfflineMode=" + (Wants ?? "(ausente)"),
                "SkipOfflineModeWarning=" + (Skip ?? "(ausente)")
            });
        }

        public static void Clear() { try { File.Delete(File_); } catch { } }

        public void Restore()
        {
            LoginUsers.SetOffline(SteamId, Wants, Skip);
            Clear();
            Log.Write("Configuracao original da Steam restaurada.");
        }
    }

    // ------------------------------------------------------------------ Rede

    static class Net
    {
        public static bool IsOnline()
        {
            foreach (var target in new[] { Tuple.Create("api.steampowered.com", 443), Tuple.Create("1.1.1.1", 443), Tuple.Create("8.8.8.8", 53) })
            {
                try
                {
                    using (var c = new TcpClient())
                    {
                        var ar = c.BeginConnect(target.Item1, target.Item2, null, null);
                        if (ar.AsyncWaitHandle.WaitOne(2500) && c.Connected)
                        {
                            c.EndConnect(ar);
                            Log.Write("Internet OK via " + target.Item1);
                            return true;
                        }
                    }
                }
                catch { }
            }
            Log.Write("Sem internet.");
            return false;
        }
    }

    // ------------------------------------------------------------------ Interface

    sealed class LauncherForm : Form
    {
        public const string Title = "PES 2021 Offline Launcher";

        static readonly Color Bg = Color.FromArgb(14, 17, 23);
        static readonly Color Panel = Color.FromArgb(22, 27, 36);
        static readonly Color Text1 = Color.FromArgb(232, 236, 242);
        static readonly Color Text2 = Color.FromArgb(140, 150, 166);
        static readonly Color Accent = Color.FromArgb(0, 168, 255);
        static readonly Color Ok = Color.FromArgb(46, 204, 113);
        static readonly Color Warn = Color.FromArgb(241, 196, 15);
        static readonly Color Err = Color.FromArgb(231, 76, 60);

        readonly Label status;
        readonly Label detail;
        readonly Label modeBadge;
        readonly Button actionButton;
        readonly NotifyIcon tray;
        Point dragStart;
        volatile bool busy;

        public LauncherForm()
        {
            Text = Title;
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Bg;
            DoubleBuffered = true;
            KeyPreview = true;
            AutoScaleMode = AutoScaleMode.Dpi;
            AutoScaleDimensions = new SizeF(96f, 96f);
            ClientSize = new Size(520, 220);
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            var title = new Label
            {
                Text = "PES 2021  |  OFFLINE LAUNCHER",
                Font = new Font("Segoe UI Semibold", 11f),
                ForeColor = Text1, AutoSize = false, Location = new Point(24, 18), Size = new Size(360, 26)
            };
            modeBadge = new Label
            {
                Text = "VERIFICANDO", Font = new Font("Segoe UI Semibold", 8.5f), ForeColor = Bg, BackColor = Text2,
                TextAlign = ContentAlignment.MiddleCenter, AutoSize = false, Location = new Point(384, 20), Size = new Size(112, 22)
            };
            var panel = new Panel { BackColor = Panel, Location = new Point(24, 58), Size = new Size(472, 92) };
            status = new Label
            {
                Text = "Verificando conexao...", Font = new Font("Segoe UI", 12f), ForeColor = Text1,
                AutoSize = false, Location = new Point(16, 14), Size = new Size(440, 30), AutoEllipsis = true
            };
            detail = new Label
            {
                Text = "", Font = new Font("Segoe UI", 9f), ForeColor = Text2,
                AutoSize = false, Location = new Point(16, 46), Size = new Size(440, 40), AutoEllipsis = true
            };
            panel.Controls.Add(status);
            panel.Controls.Add(detail);

            actionButton = new Button
            {
                Text = "FECHAR  (Esc)", Font = new Font("Segoe UI Semibold", 9f), ForeColor = Text1, BackColor = Panel,
                FlatStyle = FlatStyle.Flat, Location = new Point(366, 166), Size = new Size(130, 34), Enabled = false
            };
            actionButton.FlatAppearance.BorderColor = Accent;
            actionButton.FlatAppearance.MouseOverBackColor = Color.FromArgb(30, 40, 55);
            actionButton.Click += delegate { Close(); };

            var footer = new Label
            {
                Text = "Sider + PES2021 no fluxo do BMPES. Steam offline so quando nao ha internet.",
                Font = new Font("Segoe UI", 8f), ForeColor = Text2, AutoSize = false,
                Location = new Point(24, 166), Size = new Size(330, 34), TextAlign = ContentAlignment.MiddleLeft
            };

            Controls.AddRange(new Control[] { title, modeBadge, panel, actionButton, footer });

            foreach (Control c in new Control[] { this, title, panel, status, detail, footer })
            {
                c.MouseDown += (s, e) => { if (e.Button == MouseButtons.Left) dragStart = e.Location; };
                c.MouseMove += (s, e) =>
                {
                    if (e.Button == MouseButtons.Left)
                        Location = new Point(Location.X + e.X - dragStart.X, Location.Y + e.Y - dragStart.Y);
                };
            }

            tray = new NotifyIcon { Icon = Icon ?? SystemIcons.Application, Text = Title, Visible = false };
            tray.DoubleClick += delegate { ShowFromTray(); };
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            using (var pen = new Pen(Color.FromArgb(40, 48, 62)))
                e.Graphics.DrawRectangle(pen, 0, 0, ClientSize.Width - 1, ClientSize.Height - 1);
            using (var b = new SolidBrush(Accent))
                e.Graphics.FillRectangle(b, 0, 0, ClientSize.Width, 3);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            base.OnKeyDown(e);
            if ((e.KeyCode == Keys.Escape || e.KeyCode == Keys.Enter) && actionButton.Enabled) Close();
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            busy = true;
            var t = new Thread(Run) { IsBackground = true, Name = "launcher" };
            t.Start();
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (busy && e.CloseReason == CloseReason.UserClosing)
            {
                e.Cancel = true;
                return;
            }
            tray.Visible = false;
            tray.Dispose();
            base.OnFormClosing(e);
        }

        // ---- UI helpers (thread-safe)

        void Ui(Action a) { if (IsDisposed) return; if (InvokeRequired) BeginInvoke(a); else a(); }

        void Step(string s, string d = "")
        {
            Log.Write("[status] " + s + (d.Length > 0 ? " | " + d : ""));
            Ui(() => { status.Text = s; detail.Text = d; status.ForeColor = Text1; });
        }

        void Badge(string text, Color c) { Ui(() => { modeBadge.Text = text; modeBadge.BackColor = c; }); }

        void Finish(string s, string d, Color c, bool autoClose)
        {
            Log.Write("[fim] " + s + (d.Length > 0 ? " | " + d : ""));
            busy = false;
            Ui(() =>
            {
                ShowFromTray();
                status.Text = s; detail.Text = d; status.ForeColor = c;
                actionButton.Enabled = true; actionButton.Focus();
                if (autoClose)
                {
                    var timer = new System.Windows.Forms.Timer { Interval = 2500 };
                    timer.Tick += delegate { timer.Stop(); timer.Dispose(); Close(); };
                    timer.Start();
                }
            });
        }

        void HideToTray(string tip)
        {
            Ui(() =>
            {
                tray.Text = tip.Length > 63 ? tip.Substring(0, 63) : tip;
                tray.Visible = true;
                Hide();
            });
        }

        void ShowFromTray()
        {
            if (!Visible) { Show(); Activate(); }
            tray.Visible = false;
        }

        // ---- Fluxo principal

        void Run()
        {
            try
            {
                Log.Write("==== Inicio. BMPES=" + Paths.BmpesDir() + " Jogo=" + Paths.GameExe());

                if (Process.GetProcessesByName("PES2021").Length > 0)
                {
                    Finish("O PES 2021 ja esta aberto.", "Feche o jogo antes de usar o launcher.", Warn, false);
                    return;
                }
                if (Paths.GameExe() == null)
                {
                    Finish("PES2021.exe nao encontrado.", "Escolha o exe uma vez no BMPES Launcher (botao ESCOLHER).", Err, false);
                    return;
                }
                if (Paths.SiderExe() == null)
                {
                    Finish("sider.exe nao encontrado.", "Coloque este launcher na pasta do BMPES, junto do sider.exe.", Err, false);
                    return;
                }

                // Restauracao pendente de uma sessao anterior interrompida.
                var pending = PendingState.Load();
                if (pending != null && !Steam.IsRunning())
                {
                    Step("Restaurando a Steam de uma sessao anterior...");
                    pending.Restore();
                    pending = null;
                }

                Step("Verificando conexao...");
                bool online = Net.IsOnline();

                if (online) RunOnline(pending);
                else RunOffline(pending);
            }
            catch (Exception ex)
            {
                Log.Write("ERRO: " + ex);
                Finish("Erro inesperado.", ex.Message + "  (log em " + Log.Dir + ")", Err, false);
            }
        }

        void RunOnline(PendingState pending)
        {
            Badge("ONLINE", Ok);

            // Steam aberta em modo offline por uma sessao anterior: volta ao normal antes de jogar online.
            if (pending != null)
            {
                Step("Voltando a Steam ao modo normal...", "Uma sessao offline anterior nao foi restaurada.");
                if (!Steam.Shutdown(30000)) Steam.Kill();
                pending.Restore();
            }

            LaunchBmpesFlow();
            Finish("PES 2021 iniciado.", "Internet disponivel. Steam no modo normal.", Ok, true);
        }

        void RunOffline(PendingState pending)
        {
            Badge("OFFLINE", Warn);

            string text = LoginUsers.Read();
            var acc = LoginUsers.FindAccount(text, pending != null ? pending.SteamId : null);
            if (acc == null)
            {
                Finish("Nenhuma conta Steam com login salvo.", "Entre na Steam com internet e marque \"Lembrar senha\".", Err, false);
                return;
            }
            if (acc.Get("RememberPassword") != "1")
            {
                Finish("A Steam nao tem a senha salva.", "Entre na Steam com internet e marque \"Lembrar senha\".", Err, false);
                return;
            }

            int accountId = (int)(long.Parse(acc.SteamId) - Steam.SteamId64Base);
            bool steamWasOpen = Steam.IsRunning();
            // Steam ja aberta e logada (offline, ou online que perdeu a conexao): o jogo abre direto.
            bool loggedIn = steamWasOpen && Steam.ActiveUser() == accountId;

            if (!loggedIn)
            {
                if (steamWasOpen)
                {
                    Step("Reiniciando a Steam...", "Sem internet a Steam fica presa no login. Ela sera reaberta em modo offline.");
                    if (!Steam.Shutdown(20000)) Steam.Kill();
                }

                if (pending == null)
                {
                    pending = new PendingState { SteamId = acc.SteamId, Wants = acc.Get("WantsOfflineMode"), Skip = acc.Get("SkipOfflineModeWarning") };
                    pending.Save();
                }

                Step("Ativando o modo offline da Steam...", "Configuracao original salva e restaurada ao fechar o jogo.");
                LoginUsers.SetOffline(acc.SteamId, "1", "1");
                Steam.ClearStaleActiveUser();
                Steam.Start();

                Step("Aguardando a Steam entrar em modo offline...");
                if (!Steam.WaitLogin(90000))
                {
                    Step("Restaurando a Steam...");
                    if (!Steam.Shutdown(20000)) Steam.Kill();
                    pending.Restore();
                    Finish("A Steam nao entrou em modo offline.", "Abra a Steam uma vez com internet e tente de novo.", Err, false);
                    return;
                }
                Thread.Sleep(2000); // deixa a Steam terminar de subir os servicos locais
            }
            else
            {
                Log.Write("Steam ja logada (provavelmente em modo offline). Nada a alterar.");
            }

            LaunchBmpesFlow();

            if (pending == null)
            {
                Finish("PES 2021 iniciado.", "Steam ja estava pronta. Nenhuma configuracao alterada.", Ok, true);
                return;
            }

            // Espera o jogo abrir e fechar para restaurar a Steam.
            Step("PES 2021 iniciado.", "O launcher fica na bandeja e restaura a Steam quando o jogo fechar.");
            if (!WaitForGame(true, 90000))
            {
                Log.Write("PES2021.exe nao apareceu em 90 s.");
            }
            else
            {
                HideToTray("PES Offline Launcher: aguardando o jogo fechar");
                WaitForGame(false, int.MaxValue);
            }

            Step("Jogo fechado. Restaurando a Steam...");
            if (!Steam.Shutdown(30000)) Steam.Kill();
            pending.Restore();
            if (steamWasOpen)
            {
                // A Steam estava aberta antes do launcher: reabre no modo normal, como o usuario deixou.
                Steam.ClearStaleActiveUser();
                Steam.Start();
            }
            Finish("Tudo pronto.", "Steam restaurada ao modo normal.", Ok, true);
        }

        static bool WaitForGame(bool running, int timeoutMs)
        {
            var sw = Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < timeoutMs)
            {
                bool now = Process.GetProcessesByName("PES2021").Length > 0;
                if (now == running)
                {
                    if (running) return true;
                    // confirma que o jogo nao foi so reiniciado pela Steam
                    Thread.Sleep(5000);
                    if (Process.GetProcessesByName("PES2021").Length == 0) return true;
                }
                Thread.Sleep(1000);
            }
            return false;
        }

        // Mesmo fluxo do BMPES Launcher: sider.exe, 1200 ms, PES2021.exe.
        void LaunchBmpesFlow()
        {
            string sider = Paths.SiderExe();
            if (Process.GetProcessesByName("sider").Length == 0)
            {
                Step("Iniciando o Sider...");
                Process.Start(new ProcessStartInfo(sider) { WorkingDirectory = Path.GetDirectoryName(sider) });
                Thread.Sleep(1200);
            }
            else Log.Write("Sider ja estava aberto.");

            string game = Paths.GameExe();
            Step("Iniciando o PES 2021...");
            Process.Start(new ProcessStartInfo(game) { WorkingDirectory = Path.GetDirectoryName(game) });
        }
    }
}
