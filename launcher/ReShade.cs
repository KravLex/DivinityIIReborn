// Divinity II Reborn launcher - one-click ReShade (Vulkan layer + effects + mod presets).
// Facts used here were checked against the official sources (crosire/reshade setup/MainWindow.xaml.cs, 06.11.2026):
//  * ReShade_Setup_<v>.exe "<game exe>" --headless --api vulkan  installs the global Vulkan layer
//    (C:\ProgramData\ReShade, HKLM ...\Khronos\Vulkan\ImplicitLayers) and adds the exe to ReShadeApps.ini;
//    it needs admin rights, so it is started elevated (one UAC prompt) and then stays headless; exit code 0 = success.
//  * Headless setup refuses to run when <game>\bin\ReShade.ini exists, and never downloads effects.
//  * Effect packages = EffectPackages.ini: Standard  https://github.com/crosire/reshade-shaders/archive/slim.zip,
//    SweetFX https://github.com/CeeJayDK/SweetFX/archive/master.zip (Template.fx denied), into .\reshade-shaders\{Shaders,Textures}.
//  * Latest version = first "v*" tag of https://api.github.com/repos/crosire/reshade/tags; download
//    https://reshade.me/downloads/ReShade_Setup_<v>.exe. The setup is signed "CN=ReShade, E=info@reshade.me" with a
//    certificate Windows does not chain to a trusted root, so we pin the signer and require an intact signature.
// Nothing is deleted: ReShade.ini is backed up to ReShade.ini.div2bak before any change; effect files are only added.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace Div2Reborn
{
    sealed class RsState
    {
        public bool Layer, App, IniExists, Effects, Presets;
        public string Version, PresetFile, ConflictFile;
        public List<string> MissingFx = new List<string>();
        public bool Ready { get { return Layer && App && IniExists && Effects && Presets && ConflictFile == null; } }
    }

    sealed class RsError : Exception { public RsError(string m) : base(m) { } }

    // One-click Div2Patch 0.97 (https://github.com/BlanknameES/Div2Patch, release v0.97). The NoSetup archive holds six files for the
    // game folder and is not signed, so the exact archive is pinned by SHA-256. What it replaces is copied to _Div2Reborn\Div2Patch_backup first.
    sealed class DpState { public bool Dll, Exe; public bool Ready { get { return Dll && Exe; } } }

    static class Div2PatchHelper
    {
        const string Url = "https://github.com/BlanknameES/Div2Patch/releases/download/v0.97/Div2Patch_v0.97_NoSetup.zip";
        const string Sha = "3A0D592507D379607900E92BBA1DB2E7E2CCCADD5E46F60CDA1A202275F1BFF6";
        static readonly string[] Files = { "Data/globalswitches.xml", "bin/Div2Patch.dll", "bin/Div2Patch.exe", "bin/Div2Patch.ini", "bin/Divinity2-debug.exe", "bin/Divinity2.exe" };

        public static DpState Detect(GameInstall g)
        {
            var s = new DpState();
            if (g == null) return s;
            try {
                s.Dll = File.Exists(Path.Combine(g.Root, @"bin\Div2Patch.dll"));
                s.Exe = File.Exists(g.Exe) && ReShadeHelper.FileHas(g.Exe, "v1.4.700.56");
            } catch { }
            return s;
        }

        public static void Install(GameInstall g, Action<string> progress)
        {
            if (g == null) throw new RsError("Папка игры не найдена.");
            if (Process.GetProcessesByName("Divinity2").Length > 0) throw new RsError("Игра запущена. Закройте её и нажмите «Установить» ещё раз.");
            string tmp = Path.Combine(Path.GetTempPath(), "Div2Reborn"), zip = Path.Combine(tmp, "Div2Patch_v0.97_NoSetup.zip");
            Directory.CreateDirectory(tmp);
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
            progress("Скачиваю Div2Patch 0.97");
            ReShadeHelper.Download(Url, zip);
            progress("Проверяю архив");
            string h;
            using (var sha = System.Security.Cryptography.SHA256.Create()) using (var f = File.OpenRead(zip)) h = BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "");
            if (!h.Equals(Sha, StringComparison.OrdinalIgnoreCase)) { File.Delete(zip); throw new RsError("Скачанный архив Div2Patch не совпадает с проверенной версией 0.97. Установка остановлена, файлы игры не изменены."); }
            string backup = Path.Combine(g.Root, @"_Div2Reborn\Div2Patch_backup");
            try {
                using (var z = ZipFile.OpenRead(zip)) {
                    foreach (var name in Files) if (z.GetEntry(name) == null) throw new RsError("В архиве Div2Patch нет файла " + name + ". Установка остановлена.");
                    progress("Сохраняю заменяемые файлы");
                    foreach (var name in Files) {
                        string rel = name.Replace('/', '\\'), d = Path.Combine(g.Root, rel), b = Path.Combine(backup, rel);
                        if (File.Exists(d) && !File.Exists(b)) { Directory.CreateDirectory(Path.GetDirectoryName(b)); File.Copy(d, b); }
                    }
                    progress("Устанавливаю Div2Patch 0.97");
                    foreach (var name in Files) {
                        string d = Path.Combine(g.Root, name.Replace('/', '\\')), part = d + ".div2new";
                        Directory.CreateDirectory(Path.GetDirectoryName(d));
                        z.GetEntry(name).ExtractToFile(part, true);
                        File.Copy(part, d, true);
                        File.Delete(part);
                    }
                }
            }
            catch (UnauthorizedAccessException) { throw new RsError("Нет прав на запись в папку игры. Закройте игру и запустите лаунчер от имени администратора."); }
            catch (IOException e) { throw new RsError("Не удалось записать файлы Div2Patch: " + e.Message + " Закройте игру и попробуйте ещё раз."); }
            if (!Detect(g).Ready) throw new RsError("Файлы Div2Patch записаны, но проверка их не нашла. Попробуйте ещё раз.");
            progress("Готово");
        }
    }

    static class ReShadeHelper
    {
        public static readonly string[] PresetFiles = { "Div2Reborn_0_Off.ini", "Div2Reborn_1_Recommended.ini", "Div2Reborn_2_Clean.ini", "Div2Reborn_3_Vivid.ini" };
        public static readonly string[] PresetNames = { "Выключен", "Рекомендуемый", "Чистый", "Яркий" };
        // what the four mod presets use (Techniques= lines) plus the shared headers
        static readonly string[] NeededFx = { "ReShade.fxh", "ReShadeUI.fxh", "Curves.fx", "Vibrance.fx", "CAS.fx", "Deband.fx" };
        static readonly string[][] Packages = {
            new[] { "Standard effects", "https://github.com/crosire/reshade-shaders/archive/slim.zip" },
            new[] { "SweetFX", "https://github.com/CeeJayDK/SweetFX/archive/master.zip" },
        };

        static string Common { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "ReShade"); } }
        static string Bin(GameInstall g) { return Path.GetDirectoryName(g.Exe); }

        public static RsState Detect(GameInstall g)
        {
            var s = new RsState();
            if (g == null) return s;
            string bin = Bin(g);
            try {
                using (var hk = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32))
                using (var k = hk.OpenSubKey(@"SOFTWARE\Khronos\Vulkan\ImplicitLayers"))
                    if (k != null)
                        foreach (var n in k.GetValueNames())
                            if (n.EndsWith("ReShade32.json", StringComparison.OrdinalIgnoreCase) && File.Exists(n) && Convert.ToInt32(k.GetValue(n)) == 0) {
                                s.Layer = true;
                                string dll = Path.Combine(Path.GetDirectoryName(n), "ReShade32.dll");
                                if (File.Exists(dll)) s.Version = FileVersionInfo.GetVersionInfo(dll).ProductVersion;
                            }
            } catch { }
            try {
                string apps = Path.Combine(Common, "ReShadeApps.ini");
                if (File.Exists(apps)) s.App = File.ReadAllText(apps).IndexOf(g.Exe, StringComparison.OrdinalIgnoreCase) >= 0;
            } catch { }
            string ini = Path.Combine(bin, "ReShade.ini");
            s.IniExists = File.Exists(ini);
            if (s.IniExists) {
                string pp = IniRead(ini, "GENERAL", "PresetPath");
                if (pp != null) s.PresetFile = Path.GetFileName(pp.Trim());
            }
            var have = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            string sh = Path.Combine(bin, @"reshade-shaders\Shaders");
            try { if (Directory.Exists(sh)) foreach (var f in Directory.EnumerateFiles(sh, "*", SearchOption.AllDirectories)) have.Add(Path.GetFileName(f)); } catch { }
            foreach (var f in NeededFx) if (!have.Contains(f)) s.MissingFx.Add(f);
            s.Effects = s.MissingFx.Count == 0;
            s.Presets = PresetFiles.All(f => File.Exists(Path.Combine(bin, f)));
            // ReShade installed the DirectX 9 way would sit in place of DXVK
            foreach (var f in new[] { "d3d9.dll", "dxgi.dll", "opengl32.dll", "d3d11.dll" }) {
                string p = Path.Combine(bin, f);
                try { if (File.Exists(p) && FileHas(p, "crosire")) { s.ConflictFile = p; break; } } catch { }
            }
            return s;
        }

        // The whole install. progress gets short Russian status lines. Throws RsError with a user-facing message.
        public static void Install(GameInstall g, int preset, Action<string> progress)
        {
            if (g == null) throw new RsError("Папка игры не найдена.");
            string bin = Bin(g);
            var s = Detect(g);
            if (s.ConflictFile != null)
                throw new RsError("В папке игры лежит ReShade, поставленный как " + Path.GetFileName(s.ConflictFile) + ": он мешает DXVK мода. Удалите этот файл (или переустановите мод) и нажмите «Установить» ещё раз.");
            string tmp = Path.Combine(Path.GetTempPath(), "Div2Reborn");
            Directory.CreateDirectory(tmp);
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;   // TLS 1.2

            if (!s.Layer || !s.App) {
                progress("Узнаю последнюю версию ReShade");
                string ver = LatestVersion();
                string setup = Path.Combine(tmp, "ReShade_Setup_" + ver + ".exe");
                progress("Скачиваю ReShade " + ver);
                Download("https://reshade.me/downloads/ReShade_Setup_" + ver + ".exe", setup);
                VerifySetup(setup);

                string ini = Path.Combine(bin, "ReShade.ini"), bak = ini + ".div2bak";
                bool moved = false;
                if (File.Exists(ini)) { File.Copy(ini, bak, true); File.Delete(ini); moved = true; }   // headless setup refuses an existing ReShade.ini
                int code;
                try {
                    progress("Устанавливаю ReShade " + ver + ". Подтвердите запрос Windows на права администратора");
                    var psi = new ProcessStartInfo(setup, "\"" + g.Exe + "\" --headless --api vulkan") { UseShellExecute = true, Verb = "runas", WorkingDirectory = tmp };
                    Process p;
                    try { p = Process.Start(psi); }
                    catch (System.ComponentModel.Win32Exception e) {
                        if (e.NativeErrorCode == 1223) throw new RsError("Установка отменена: Windows не получила разрешение на права администратора. Они нужны ReShade, чтобы подключиться к Vulkan.");
                        throw new RsError("Не удалось запустить установщик ReShade: " + e.Message);
                    }
                    if (p == null) throw new RsError("Не удалось запустить установщик ReShade.");
                    p.WaitForExit(); code = p.ExitCode;
                } finally {
                    if (moved) File.Copy(bak, ini, true);                     // the player's own settings win
                }
                if (code != 0) throw new RsError("Установщик ReShade завершился с ошибкой (код " + code + ").");
                s = Detect(g);
                if (!s.Layer || !s.App) throw new RsError("Установщик ReShade отработал, но слой Vulkan не найден. Попробуйте ещё раз или поставьте ReShade вручную (_Div2Reborn\\ReShade\\ReShade_install_RU.txt).");
            }

            FinishSetup(g, preset, progress);
        }

        // Effects + mod presets + PresetPath. Runs after the Vulkan layer is in place (also used alone by tests).
        public static void FinishSetup(GameInstall g, int preset, Action<string> progress)
        {
            string bin = Bin(g), tmp = Path.Combine(Path.GetTempPath(), "Div2Reborn");
            Directory.CreateDirectory(tmp);
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
            var s = Detect(g);
            if (!s.Effects) {
                foreach (var pk in Packages) {
                    progress("Скачиваю эффекты: " + pk[0]);
                    string zip = Path.Combine(tmp, pk[0].Replace(' ', '_') + ".zip");
                    Download(pk[1], zip);
                    progress("Распаковываю эффекты: " + pk[0]);
                    ExtractEffects(zip, Path.Combine(bin, "reshade-shaders"));
                }
            }

            progress("Копирую пресеты мода");
            string src = Path.Combine(g.Root, @"_Div2Reborn\ReShade");
            foreach (var f in PresetFiles) {
                string d = Path.Combine(bin, f), sp = Path.Combine(src, f);
                if (!File.Exists(d) && File.Exists(sp)) File.Copy(sp, d);
            }
            SetPreset(g, preset);
            s = Detect(g);
            if (!s.Effects || !s.Presets) {
                if (!s.Effects) throw new RsError("Не хватает эффектов: " + string.Join(", ", s.MissingFx) + ".");
                if (!s.Presets) throw new RsError("Не найдены пресеты мода в _Div2Reborn\\ReShade - распакуйте архив мода заново.");
            }
            if (!s.Ready) throw new RsError("ReShade установлен не полностью. Нажмите «Установить» ещё раз.");
            progress("Готово");
        }

        // PresetPath in bin\ReShade.ini; creates the file with ReShade's default search paths when missing.
        public static void SetPreset(GameInstall g, int preset)
        {
            string ini = Path.Combine(Bin(g), "ReShade.ini");
            string value = @".\" + PresetFiles[preset];
            var lines = File.Exists(ini) ? File.ReadAllLines(ini).ToList() : new List<string>();
            if (File.Exists(ini) && !File.Exists(ini + ".div2bak")) File.Copy(ini, ini + ".div2bak");
            int gen = lines.FindIndex(l => l.Trim().Equals("[GENERAL]", StringComparison.OrdinalIgnoreCase));
            if (gen < 0) { lines.Insert(0, "[GENERAL]"); gen = 0; if (lines.Count > 1) lines.Insert(1, ""); }
            int end = gen + 1; while (end < lines.Count && !lines[end].TrimStart().StartsWith("[")) end++;
            Action<string, string> set = (key, val) => {
                for (int i = gen + 1; i < end; i++) {
                    int eq = lines[i].IndexOf('=');
                    if (eq > 0 && lines[i].Substring(0, eq).Trim().Equals(key, StringComparison.OrdinalIgnoreCase)) { lines[i] = key + "=" + val; return; }
                }
                lines.Insert(end, key + "=" + val); end++;
            };
            set("PresetPath", value);
            if (IniRead(ini, "GENERAL", "EffectSearchPaths") == null) set("EffectSearchPaths", @".\reshade-shaders\Shaders\**");
            if (IniRead(ini, "GENERAL", "TextureSearchPaths") == null) set("TextureSearchPaths", @".\reshade-shaders\Textures\**");
            File.WriteAllLines(ini, lines, new UTF8Encoding(false));
        }

        static void ExtractEffects(string zip, string dest)
        {
            using (var z = ZipFile.OpenRead(zip))
                foreach (var e in z.Entries) {
                    if (string.IsNullOrEmpty(e.Name)) continue;                      // folder
                    var parts = e.FullName.Split('/');
                    if (parts.Length < 3 || (parts[1] != "Shaders" && parts[1] != "Textures")) continue;
                    if (e.Name.Equals("Template.fx", StringComparison.OrdinalIgnoreCase)) continue;
                    string rel = string.Join("\\", parts.Skip(2));
                    if (rel.Contains("..")) continue;
                    string outp = Path.Combine(dest, parts[1], rel);
                    if (File.Exists(outp)) continue;                                   // never overwrite the player's files
                    Directory.CreateDirectory(Path.GetDirectoryName(outp));
                    e.ExtractToFile(outp);
                }
        }

        static WebClient Client() { var w = new WebClient(); w.Headers.Add("User-Agent", "Div2RebornLauncher"); return w; }

        internal static void Download(string url, string file)
        {
            try { using (var w = Client()) w.DownloadFile(url, file + ".part"); }
            catch (WebException e) { throw new RsError("Не удалось скачать " + url + ": " + e.Message + ". Проверьте подключение к интернету."); }
            if (File.Exists(file)) File.Delete(file);
            File.Move(file + ".part", file);
        }

        static string LatestVersion()
        {
            string json;
            try { using (var w = Client()) json = w.DownloadString("https://api.github.com/repos/crosire/reshade/tags"); }
            catch (WebException e) { throw new RsError("Не удалось узнать версию ReShade (GitHub): " + e.Message + ". Проверьте подключение к интернету."); }
            var m = Regex.Match(json, "\"name\"\\s*:\\s*\"v(\\d+(?:\\.\\d+)+)\"");
            if (!m.Success) throw new RsError("GitHub не вернул номер версии ReShade.");
            return m.Groups[1].Value;
        }

        // --- signature check: exact signer + intact signature (the root is not trusted by Windows, see header) ---
        const uint CERT_E_UNTRUSTEDROOT = 0x800B0109, CERT_E_CHAINING = 0x800B010A;
        static void VerifySetup(string file)
        {
            string subj;
            try { subj = X509Certificate.CreateFromSignedFile(file).Subject; }
            catch { throw new RsError("Скачанный установщик ReShade не подписан. Установка остановлена."); }
            if (!subj.Contains("CN=ReShade") || !subj.Contains("E=info@reshade.me")) throw new RsError("Установщик ReShade подписан не автором ReShade (" + subj + "). Установка остановлена.");
            uint r = (uint)WinTrust(file);
            if (r != 0 && r != CERT_E_UNTRUSTEDROOT && r != CERT_E_CHAINING) throw new RsError("Подпись установщика ReShade повреждена (0x" + r.ToString("X8") + "). Установка остановлена.");
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct WINTRUST_FILE_INFO { public uint cbStruct; public string pcwszFilePath; public IntPtr hFile; public IntPtr pgKnownSubject; }
        [StructLayout(LayoutKind.Sequential)]
        struct WINTRUST_DATA { public uint cbStruct; public IntPtr pPolicyCallbackData, pSIPClientData; public uint dwUIChoice, fdwRevocationChecks, dwUnionChoice; public IntPtr pFile; public uint dwStateAction; public IntPtr hWVTStateData; public IntPtr pwszURLReference; public uint dwProvFlags, dwUIContext; public IntPtr pSignatureSettings; }
        [DllImport("wintrust.dll", CharSet = CharSet.Unicode)] static extern int WinVerifyTrust(IntPtr hwnd, ref Guid action, ref WINTRUST_DATA data);
        static int WinTrust(string file)
        {
            var fi = new WINTRUST_FILE_INFO { cbStruct = (uint)Marshal.SizeOf(typeof(WINTRUST_FILE_INFO)), pcwszFilePath = file };
            IntPtr pfi = Marshal.AllocHGlobal(Marshal.SizeOf(fi));
            try {
                Marshal.StructureToPtr(fi, pfi, false);
                var wd = new WINTRUST_DATA { cbStruct = (uint)Marshal.SizeOf(typeof(WINTRUST_DATA)), dwUIChoice = 2 /*WTD_UI_NONE*/, fdwRevocationChecks = 0, dwUnionChoice = 1 /*FILE*/, pFile = pfi, dwStateAction = 0, dwProvFlags = 0x40 /*WTD_REVOCATION_CHECK_NONE*/ };
                var action = new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");   // WINTRUST_ACTION_GENERIC_VERIFY_V2
                return WinVerifyTrust(IntPtr.Zero, ref action, ref wd);
            } finally { Marshal.DestroyStructure(pfi, typeof(WINTRUST_FILE_INFO)); Marshal.FreeHGlobal(pfi); }
        }

        internal static bool FileHas(string p, string ascii)
        {
            byte[] d = File.ReadAllBytes(p), k = Encoding.ASCII.GetBytes(ascii);
            for (int i = 0; i + k.Length <= d.Length; i++) { int j = 0; while (j < k.Length && d[i + j] == k[j]) j++; if (j == k.Length) return true; }
            return false;
        }

        static string IniRead(string p, string sec, string key)
        {
            if (!File.Exists(p)) return null;
            string cur = null;
            foreach (var l in File.ReadAllLines(p)) {
                var t = l.Trim();
                if (t.StartsWith("[")) { cur = t.Trim('[', ']'); continue; }
                if (cur == null || !cur.Equals(sec, StringComparison.OrdinalIgnoreCase)) continue;
                int eq = t.IndexOf('='); if (eq < 0) continue;
                if (t.Substring(0, eq).Trim().Equals(key, StringComparison.OrdinalIgnoreCase)) return t.Substring(eq + 1).Trim();
            }
            return null;
        }

        // self-test hook for --rs-verify <file>: prints signer and WinVerifyTrust result
        public static string Probe(string file)
        {
            string subj = "-";
            try { subj = X509Certificate.CreateFromSignedFile(file).Subject; } catch (Exception e) { subj = "no signature: " + e.Message; }
            string vr; try { VerifySetup(file); vr = "accepted"; } catch (RsError e) { vr = "rejected: " + e.Message; }
            return "signer=" + subj + " wintrust=0x" + ((uint)WinTrust(file)).ToString("X8") + " -> " + vr;
        }
    }
}
