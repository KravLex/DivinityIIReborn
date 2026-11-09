// Divinity II Reborn - launcher (replaces the Larian "Divinity Anthology" Autorun menu that Steam starts).
// One button starts the normal game exactly the way the original Autorun does: installation.ini [general]
// ExecutableFilename / ExecutableWorkingDir / ExecutableParameters (bin\Divinity2.exe, bin\, none).
// The Developer Mode build (bin\Divinity2-debug.exe) is not offered anywhere.
// The installation check only reads files; it never writes, moves or deletes anything.
// Build: native\launcher\build.cmd (csc from .NET Framework 4.x, no other dependencies).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: System.Reflection.AssemblyTitle("Divinity II Reborn Launcher")]
[assembly: System.Reflection.AssemblyProduct("Divinity II Reborn")]
[assembly: System.Reflection.AssemblyVersion("2.0.0.0")]

namespace Div2Reborn
{
    enum Sev { Ok, Info, Warn, Error }

    sealed class Finding
    {
        public Sev Severity; public string Title, Path, Description, Fix;
        public Finding(Sev s, string t, string p, string d, string f) { Severity = s; Title = t; Path = p; Description = d; Fix = f; }
    }

    sealed class CheckResult
    {
        public List<Finding> Items = new List<Finding>();
        public bool Crashed; public string CrashText;
        public int Count(Sev s) { return Items.Count(i => i.Severity == s); }
        public Sev Overall { get { return Crashed || Count(Sev.Error) > 0 ? Sev.Error : Count(Sev.Warn) > 0 ? Sev.Warn : Sev.Ok; } }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Game location and launch command - read from the real installation, never invented.
    // ------------------------------------------------------------------------------------------------------------
    sealed class GameInstall
    {
        public string Root;                 // folder with bin\ and Data\
        public string ExeRel = @"bin\Divinity2.exe", WorkRel = @"bin\", Args = "";
        public bool FromIni;
        public string Exe { get { return Path.Combine(Root, ExeRel); } }
        public string WorkDir { get { return Path.Combine(Root, WorkRel); } }
        public string ModVersion;           // "Divinity II Reborn 1.5" from _Div2Reborn\README.txt

        public static GameInstall Find(string overrideRoot)
        {
            var cands = new List<string>();
            if (!string.IsNullOrEmpty(overrideRoot)) cands.Add(overrideRoot);
            string me = AppDomain.CurrentDomain.BaseDirectory;
            for (string d = me; d != null; d = Path.GetDirectoryName(d.TrimEnd('\\'))) { cands.Add(d); if (cands.Count > 6) break; }
            try {   // Steam installscript.vdf writes this key
                using (var k = Registry.CurrentUser.OpenSubKey(@"Software\Larian Studios\Divinity II - DKS"))
                    if (k != null && k.GetValue("target_folder") is string) cands.Add((string)k.GetValue("target_folder"));
            } catch { }
            foreach (var c in cands) {
                try {
                    if (File.Exists(Path.Combine(c, "installation.ini")) || File.Exists(Path.Combine(c, @"bin\Divinity2.exe"))) {
                        var g = new GameInstall { Root = Path.GetFullPath(c) };
                        g.ReadIni(); g.ReadModVersion(); return g;
                    }
                } catch { }
            }
            return null;
        }

        void ReadIni()
        {
            string ini = Path.Combine(Root, "installation.ini");
            if (!File.Exists(ini)) return;
            string[] lines;
            try { lines = File.ReadAllLines(ini); } catch { return; }   // UTF-16 with BOM - detected by ReadAllLines
            bool general = false;
            foreach (var raw in lines) {
                var l = raw.Trim();
                if (l.StartsWith("[")) { general = l.Equals("[general]", StringComparison.OrdinalIgnoreCase); continue; }
                if (!general) continue;
                int eq = l.IndexOf('='); if (eq < 0) continue;
                string k = l.Substring(0, eq).Trim(), v = l.Substring(eq + 1).Trim();
                if (k.Equals("ExecutableFilename", StringComparison.OrdinalIgnoreCase) && v.Length > 0) { ExeRel = v; FromIni = true; }
                else if (k.Equals("ExecutableWorkingDir", StringComparison.OrdinalIgnoreCase) && v.Length > 0) WorkRel = v;
                else if (k.Equals("ExecutableParameters", StringComparison.OrdinalIgnoreCase)) Args = v;
            }
        }

        void ReadModVersion()
        {
            try {
                string r = Path.Combine(Root, @"_Div2Reborn\README.txt");
                if (!File.Exists(r)) return;
                string first = File.ReadLines(r).FirstOrDefault() ?? "";
                int dash = first.IndexOf(" - ");
                ModVersion = (dash > 0 ? first.Substring(0, dash) : first).Trim().TrimStart('﻿');
            } catch { }
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Installation check. The file list comes from the Reborn 2.0 release (release\2.0, _Div2Reborn\uninstall.cmd,
    // README.txt). SHA-256 values are from DivinityIIReborn_v2.0_SHA256.txt; a mismatch is only a warning (other build).
    // ------------------------------------------------------------------------------------------------------------
    static class ModChecker
    {
        struct F { public string Rel, Sha, What; public bool Required; public F(string r, string s, string w, bool req) { Rel = r; Sha = s; What = w; Required = req; } }

        static readonly F[] Core = {
            new F(@"bin\dinput8.dll", "10c271e9a08750b4c0fe6f89be5b5735d008333b65c4b1bf31338aad43d125c9", "Div2Fix - основной модуль мода", true),
            new F(@"bin\Div2Fix.ini", null, "настройки Div2Fix", true),
            new F(@"bin\Div2Fix_mindread.txt", "ec01d6d55a5278c159b808274fd40cbe9a96505159105960e199a61dd801fb28", "таблица советника чтения мыслей", true),
            new F(@"bin\d3d9.dll", "ef8089d0a2d18c348e557d5557373abf3984db0e305bcc2e3eed308b7a4d0557", "DXVK с патчами Reborn (рендер, SMAA/TAA)", true),
            new F(@"bin\dxvk.conf", null, "настройки DXVK", true),
        };
        static readonly F[] Override = {
            new F(@"bin\Div2Reborn_override\far\d50c5e8e8201ae4165e7edeca1b991f8cf1c16bf.dxso", "1e7e90055d08d596b33d83c55958e3074ef50f4e071f941a3dfcfa59a1bcd04d", "шейдер освещения (тени)", true),
            new F(@"bin\Div2Reborn_override\far\e6bef6f7376173eb73df5390447a1067939c5e8a.dxso", "a6705812f2bfe4e5ec24139922bccc1b5bb29e6626aa267e8239de7f3565c1af", "шейдер освещения (тени)", true),
        };
        static readonly F[] Post = {
            new F(@"bin\Div2Reborn_post\AreaTex.bin", "3b6c18f1d963e03745b6f4cbddace35d2eb33c0fdb125b852ec3c493cd4ddd15", "SMAA", true),
            new F(@"bin\Div2Reborn_post\SearchTex.bin", "8488e2e35e9dd4f1eef642057366f247fa2c9d3eefae84cdd76810d94a5e5a3a", "SMAA", true),
            new F(@"bin\Div2Reborn_post\smaa_blend.ps.dxso", "f3fa807234d7053cf7f4997be4860e800d13c9974853c00b6a4ffa9eb9f9f586", "SMAA", true),
            new F(@"bin\Div2Reborn_post\smaa_blend.vs.dxso", "193a9e94f450cdc9ed1944facb897568892a38f53a0b9a409de4f1f3f9f0b1dd", "SMAA", true),
            new F(@"bin\Div2Reborn_post\smaa_edge.ps.dxso", "96b6e720cf0420be38718bc577b9923b764b49471e6744a35858df7af77eb71b", "SMAA", true),
            new F(@"bin\Div2Reborn_post\smaa_edge.vs.dxso", "c445039645a43bb86ff0b179b09a425d5305e01005bb9a4e299a473ad3284c25", "SMAA", true),
            new F(@"bin\Div2Reborn_post\smaa_edge_geo.ps.dxso", "27d0a46b94ba75d5054801719c05537254ab35ec3c06727d096ecb9b58a8571b", "SMAA (края по геометрии)", true),
            new F(@"bin\Div2Reborn_post\smaa_weight.ps.dxso", "461f293edd12ed73cc42c04272fcd10ebc978ecefb85ec40aafa2bf8db0edce7", "SMAA", true),
            new F(@"bin\Div2Reborn_post\smaa_weight.vs.dxso", "068b061b604bebde2d52413def0b51cae9bac6af6d527f7a328a802b2c1830bd", "SMAA", true),
            new F(@"bin\Div2Reborn_post\taa.ps.dxso", "ec420d51736007ee6834d1c6ecb7be51db9ac503ae48d620bbff7136abf98fc8", "TAA", true),
            new F(@"bin\Div2Reborn_post\taa.vs.dxso", "23f0b00cf63a1083d0214459dac6e509a7d6e476be8dfc2f1a11221fba53034b", "TAA", true),
        };
        static readonly F[] Skills = {
            new F(@"Data\Win32\Packed\Reborn\MageExpansion.dv2", "f57fd75ff343f6337d07a2b2a9f8a96ad6b44e85c0d2d816b9f58de76a13d004", "новые навыки и эффекты", true),
            new F(@"Data\Win32\Packed\Reborn\MageData.dv2", "562d86eb0b8c613a076954c47a5e52fe7b1c459968ef80632bdbb1e2d26fe10d", "данные новых навыков", true),
            new F(@"Data\Win32\Packed\Reborn\GamepadGlyphs.dv2", "8310b7c7df25e5b036e50c85584bb27ec22ffb2e1ab5813f6faa9a81a49eaff9", "значки кнопок геймпада", true),
            new F(@"bin\MageExpansion\ArcanePush.wav", "02bb7e4bed163f002df62acf6cbadfde1bb0f776624d41b85363599e9dd7649d", "звук навыка", true),
            new F(@"bin\MageExpansion\ArcaneShield.wav", "5b19465f5085da0532619be03105b24cc825a9ec79e78a800cc361dc2d6dcdca", "звук навыка", true),
            new F(@"bin\MageExpansion\BattleCry_F.wav", "24642f6d3f12a4558d3a8e43ae71ff672698127d2354decd72c676082c6efdc5", "звук навыка", true),
            new F(@"bin\MageExpansion\BattleCry_F_game.wav", "7069b1d4d7c3f61c39fe6a380a33674bb431ebd2c79a14446175260b7d892dcb", "звук навыка", true),
            new F(@"bin\MageExpansion\BattleCry_M.wav", "118880987b450bcc3211891899b19da5ff9259632b027f6522d4c3875d15922f", "звук навыка", true),
            new F(@"bin\MageExpansion\BattleCry_M_game.wav", "3f5278348843890ef3b5a75530eac8017154d77f54653c92eca14e4e6ac7c240", "звук навыка", true),
            new F(@"bin\MageExpansion\Blessing.wav", "8e76d137926fc1239cc3e6bb77d3e5f473456bdc4388c65c5c01a37c12c1f7e3", "звук навыка", true),
            new F(@"bin\MageExpansion\ChainLightning.wav", "814f20114d09546bb14ee7ec6739c657a363898b599a95dbddda45464e389703", "звук навыка", true),
            new F(@"bin\MageExpansion\Drain.wav", "122cfd2fb1706607395d31702c371be06bd712fad85ce637ee645c213606c6b8", "звук навыка", true),
            new F(@"bin\MageExpansion\FrostNova.wav", "f025cc971311932760ea4cc9c4035a27cd7a270eba7902848dd9c2a7fa71013e", "звук навыка", true),
            new F(@"bin\MageExpansion\HolySmite.wav", "6e8f2ad1a819023b09e92c879070004222029ea5c0a8dbe9c47626e3b35520f4", "звук навыка", true),
            new F(@"bin\MageExpansion\IceBolt.wav", "215aa6ba7641d36107d4fd1c3c383326cd51f1a537334e5dc3948938fbca4763", "звук навыка", true),
        };
        static readonly F FxPack = new F(@"Data\Win32\Packed\Reborn\Reborn_FX.dv2", "e6fe96fbaee6bfa731f5c50c2b7962d089f9d7aabdcb4a0d644806b50f16ae93", "пак эффектов (необязательный)", false);
        static readonly string[] HdPack = { "Patch.dv2", "Patch_2.dv2", "Patch_3.dv2", "Patch_4.dv2", "DKS_Patch.dv2", "DKS_Patch_2.dv2", "DKS_Patch_3.dv2", "DKS_Patch_4.dv2", "FOV_Patch.dv2", "FOV_Patch_2.dv2", "FOV_Patch_3.dv2", "FOV_Patch_4.dv2" };

        // quick = no hashing (for the status dot at start-up). progress(0..1, text) may be null.
        public static CheckResult Run(GameInstall g, bool quick, CancellationToken ct, Action<double, string> progress)
        {
            var r = new CheckResult();
            try { RunInner(g, quick, ct, progress, r); }
            catch (OperationCanceledException) { throw; }
            catch (Exception e) { r.Crashed = true; r.CrashText = e.Message; r.Items.Add(new Finding(Sev.Error, "Проверка прервалась", g != null ? g.Root : "", e.GetType().Name + ": " + e.Message, "Запустите проверку ещё раз. Если ошибка повторяется - сообщите о ней автору мода.")); }
            return r;
        }

        static void Add(CheckResult r, Sev s, string t, string p, string d, string f) { r.Items.Add(new Finding(s, t, p, d, f)); }

        static void RunInner(GameInstall g, bool quick, CancellationToken ct, Action<double, string> progress, CheckResult r)
        {
            Action<double, string> P = (x, s) => { ct.ThrowIfCancellationRequested(); if (progress != null) progress(x, s); };
            if (g == null) {
                Add(r, Sev.Error, "Папка игры не найдена", AppDomain.CurrentDomain.BaseDirectory,
                    "Рядом с лаунчером нет bin\\Divinity2.exe и installation.ini, в реестре нет пути Steam.",
                    "Положите лаунчер в папку игры (рядом с bin и Data).");
                return;
            }
            string root = g.Root;
            P(0.02, "Папки игры");
            foreach (var d in new[] { "bin", "Data", @"Data\Win32\Packed" }) {
                string p = Path.Combine(root, d);
                if (!Directory.Exists(p)) Add(r, Sev.Error, "Нет папки игры", p, "Папка " + d + " отсутствует - это не папка Divinity II или установка повреждена.", "Steam: ПКМ по игре > Свойства > Установленные файлы > Проверить целостность.");
            }

            // --- game exe (Div2Patch build) ---
            P(0.06, "Исполняемый файл игры");
            string exe = g.Exe;
            if (!g.FromIni) Add(r, Sev.Warn, "installation.ini не прочитан", Path.Combine(root, "installation.ini"), "Путь к игре взят по умолчанию (bin\\Divinity2.exe).", "Проверьте целостность файлов в Steam.");
            if (!File.Exists(exe)) Add(r, Sev.Error, "Нет исполняемого файла игры", exe, "Без него игра не запустится.", "Проверьте целостность файлов в Steam, затем заново установите Div2Patch.");
            else {
                string ver = ReadGameVersion(exe, r);
                if (ver == null) Add(r, Sev.Warn, "Версия игры не определена", exe, "В exe не найдена строка версии v1.4.700.*.", "Установите Div2Patch 0.97 - мод рассчитан на v1.4.700.56.");
                else if (ver != "v1.4.700.56") Add(r, Sev.Error, "Неподходящая версия игры: " + ver, exe, "Div2Fix рассчитан на v1.4.700.56 (её ставит Div2Patch). На другой версии большинство функций мода выключится.", "Установите Div2Patch 0.97: https://github.com/BlanknameES/Div2Patch");
                else Add(r, Sev.Ok, "Игра " + ver + " (сборка Div2Patch)", exe, "Версия подходит для мода.", "");
            }

            // --- Div2Patch ---
            P(0.10, "Div2Patch");
            string dpDll = Path.Combine(root, @"bin\Div2Patch.dll"), dpIni = Path.Combine(root, @"bin\Div2Patch.ini");
            if (!File.Exists(dpDll)) Add(r, Sev.Error, "Не установлен Div2Patch", dpDll, "Div2Patch 0.97 - обязательная зависимость мода.", "Установите Div2Patch: https://github.com/BlanknameES/Div2Patch");
            else if (!CanRead(dpDll)) Add(r, Sev.Error, "Div2Patch.dll недоступен для чтения", dpDll, "Файл заблокирован или нет прав.", "Закройте игру и проверьте права на папку bin.");
            else {
                string en = File.Exists(dpIni) ? IniGet(dpIni, "Main", "EnablePatch") : null;
                if (en == "0") Add(r, Sev.Warn, "Div2Patch выключен", dpIni, "EnablePatch=0 - исправления Div2Patch не действуют.", "Поставьте EnablePatch=1 в Div2Patch.ini.");
                else Add(r, Sev.Ok, "Div2Patch установлен", dpDll, "", "");
            }

            // --- mod files ---
            var groups = new[] { Core, Override, Post, Skills };
            int total = Core.Length + Override.Length + Post.Length + Skills.Length + 1, done = 0;
            int missingPost = 0, missingOverride = 0;
            foreach (var grp in groups) foreach (var f in grp) {
                P(0.12 + 0.6 * done++ / total, f.Rel);
                bool ok = CheckFile(r, root, f, quick);
                if (!ok && grp == Post) missingPost++;
                if (!ok && grp == Override) missingOverride++;
            }
            int present = total - 1 - r.Items.Count(i => i.Severity == Sev.Error && i.Title.StartsWith("Нет файла"));
            if (present == total - 1) Add(r, Sev.Ok, "Файлы мода на месте: " + present + " из " + (total - 1), Path.Combine(root, "bin"), "Div2Fix, DXVK, настройки, шейдеры, навыки и значки геймпада.", "");
            P(0.12 + 0.6 * done / total, FxPack.Rel);
            if (CheckFile(r, root, FxPack, quick)) Add(r, Sev.Ok, "Пак эффектов Reborn_FX.dv2 подключён", Path.Combine(root, FxPack.Rel), "", "");

            // --- content checks: right DLL in the right place ---
            P(0.75, "Содержимое модулей");
            string dinput = Path.Combine(root, @"bin\dinput8.dll"), d3d9 = Path.Combine(root, @"bin\d3d9.dll");
            if (File.Exists(dinput) && CanRead(dinput) && !Contains(dinput, "Div2Fix"))
                Add(r, Sev.Error, "bin\\dinput8.dll - не Div2Fix", dinput, "На месте модуля мода лежит другой dinput8.dll (другой мод или трейнер). Div2Fix не загрузится.", "Переустановите мод (распакуйте архив в папку игры с заменой).");
            if (File.Exists(d3d9) && CanRead(d3d9) && !Contains(d3d9, "dxvk") && !Contains(d3d9, "DXVK"))
                Add(r, Sev.Error, "bin\\d3d9.dll - не DXVK", d3d9, "Вместо DXVK из мода лежит другой d3d9.dll (часто - ReShade, установленный как DirectX 9). Сглаживание, тени и исправление вылета работать не будут.", "Переустановите мод. ReShade ставьте с API Vulkan (_Div2Reborn\\ReShade\\ReShade_install_RU.txt).");
            foreach (var other in new[] { "dxgi.dll", "d3d11.dll", "opengl32.dll", "dsound.dll" }) {
                string p = Path.Combine(root, "bin", other);
                if (File.Exists(p)) Add(r, Sev.Warn, "Посторонний перехватчик: " + other, p, "Этот файл не входит ни в игру, ни в мод. Возможен конфликт с DXVK/Div2Fix (часто это ReShade или старый мод).", "Если не ставили его специально - перенесите файл из bin в другую папку.");
            }

            // --- configs ---
            P(0.82, "Настройки");
            string fixIni = Path.Combine(root, @"bin\Div2Fix.ini");
            if (File.Exists(fixIni) && CanRead(fixIni)) {
                var secs = IniSections(fixIni);
                var need = new[] { "General", "Graphics", "Overlay" };
                var miss = need.Where(s => !secs.Contains(s)).ToArray();
                if (miss.Length > 0) Add(r, Sev.Warn, "Div2Fix.ini неполный", fixIni, "Нет разделов: " + string.Join(", ", miss.Select(s => "[" + s + "]")) + " - файл от старой версии или испорчен; Div2Fix возьмёт значения по умолчанию.", "Возьмите Div2Fix.ini из архива мода (свои правки перенесите вручную).");
                if (IniGet(fixIni, "Overlay", "Enable") == "0" && File.Exists(Path.Combine(root, FxPack.Rel)))
                    Add(r, Sev.Warn, "Пак эффектов отключён", fixIni, "Reborn_FX.dv2 есть, но [Overlay] Enable=0 - он не подключается.", "Поставьте Enable=1 в разделе [Overlay] или удалите Reborn_FX.dv2.");
            }
            string conf = Path.Combine(root, @"bin\dxvk.conf");
            if (File.Exists(conf) && CanRead(conf)) {
                var kv = DxvkConf(conf);
                string ov;
                if (kv.TryGetValue("d3d9.shaderOverridePath", out ov)) {
                    string od = Path.Combine(root, "bin", ov);
                    if (!Directory.Exists(od)) Add(r, Sev.Error, "Папка замены шейдеров не найдена", od, "dxvk.conf ссылается на d3d9.shaderOverridePath = " + ov + ", но папки нет.", "Переустановите мод или закомментируйте строку в dxvk.conf.");
                } else if (missingOverride == 0)
                    Add(r, Sev.Info, "Замена шейдеров теней выключена", conf, "В dxvk.conf нет d3d9.shaderOverridePath - тени как в оригинальной игре.", "Если выключали не специально - верните строку d3d9.shaderOverridePath = Div2Reborn_override\\far.");
                string pa;
                bool aaOn = !(kv.TryGetValue("d3d9.postAA", out pa) && pa.Equals("False", StringComparison.OrdinalIgnoreCase));
                string pp; string postDir = Path.Combine(root, "bin", kv.TryGetValue("d3d9.postAAPath", out pp) ? pp : "Div2Reborn_post");
                if (aaOn && !Directory.Exists(postDir)) Add(r, Sev.Error, "Папка SMAA/TAA не найдена", postDir, "Сглаживание включено в dxvk.conf, но шейдеров нет.", "Переустановите мод.");
            }

            // --- optional pieces ---
            P(0.9, "HD-текстуры");
            string packed = Path.Combine(root, @"Data\Win32\Packed");
            int hd = HdPack.Count(n => File.Exists(Path.Combine(packed, n)));
            if (hd == 0) Add(r, Sev.Info, "Пак HD-текстур не установлен", packed, "Пак скачивается отдельно (~17 ГБ) и не обязателен.", "");
            else Add(r, Sev.Ok, "Пак HD-текстур: " + hd + " из " + HdPack.Length + " архивов", packed, "", "");
            foreach (var n in HdPack) { string p = Path.Combine(packed, n); if (File.Exists(p) && !CanRead(p)) Add(r, Sev.Error, "Архив недоступен для чтения", p, "Игра не сможет его прочитать.", "Закройте игру и проверьте права на папку Data."); }

            string lic = Path.Combine(root, "_Div2Reborn");
            if (!Directory.Exists(lic)) Add(r, Sev.Warn, "Нет папки _Div2Reborn", lic, "В ней README, лицензии и удаление мода (uninstall.cmd).", "Распакуйте архив мода в папку игры полностью.");
            else if (!File.Exists(Path.Combine(lic, "uninstall.cmd"))) Add(r, Sev.Warn, "Нет _Div2Reborn\\uninstall.cmd", lic, "Без него мод придётся удалять вручную.", "Распакуйте архив мода ещё раз.");

            // --- last run log: did Div2Fix load and accept the game version? ---
            P(0.95, "Журнал Div2Fix");
            string log = Path.Combine(root, @"bin\Div2Fix.log");
            if (File.Exists(log) && CanRead(log)) {
                string head = "";
                try { using (var fs = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) using (var sr = new StreamReader(fs, Encoding.Default)) { var b = new char[4096]; int n = sr.Read(b, 0, b.Length); head = new string(b, 0, n); } } catch { }
                if (head.Contains("needs v1.4.700.56")) Add(r, Sev.Warn, "При прошлом запуске Div2Fix отключил функции", log, "В журнале: версия игры не подходит.", "Установите Div2Patch 0.97 и запустите игру снова.");
            }

            if (Process.GetProcessesByName("Divinity2").Length > 0)
                Add(r, Sev.Warn, "Игра уже запущена", g.Exe, "Файлы мода заняты запущенной игрой.", "Закройте игру перед переустановкой мода.");
            P(1.0, "Готово");
        }

        static bool CheckFile(CheckResult r, string root, F f, bool quick)
        {
            string p = Path.Combine(root, f.Rel);
            if (!File.Exists(p)) {
                if (Directory.Exists(p)) { Add(r, Sev.Error, "На месте файла - папка", p, f.What + ".", "Удалите папку и переустановите мод."); return false; }
                if (f.Required) Add(r, Sev.Error, "Нет файла: " + Path.GetFileName(f.Rel), p, "Отсутствует " + f.What + ".", "Распакуйте архив мода в папку игры (рядом с bin и Data) с заменой файлов.");
                else Add(r, Sev.Info, "Нет файла: " + Path.GetFileName(f.Rel), p, "Отсутствует " + f.What + " - мод работает и без него.", "");
                return false;
            }
            if (!CanRead(p)) { Add(r, Sev.Error, "Файл недоступен для чтения", p, f.What + ": заблокирован другой программой или нет прав.", "Закройте игру и антивирусную проверку, проверьте права на папку."); return false; }
            long len = new FileInfo(p).Length;
            if (len == 0) { Add(r, Sev.Error, "Пустой файл: " + Path.GetFileName(f.Rel), p, f.What + ": размер 0 байт - файл повреждён.", "Распакуйте архив мода заново."); return false; }
            if (!quick && f.Sha != null) {
                string h = Sha256(p);
                if (!h.Equals(f.Sha, StringComparison.OrdinalIgnoreCase)) {
                    Add(r, Sev.Warn, "Файл отличается от выпуска Reborn 2.0: " + Path.GetFileName(f.Rel), p, f.What + ". Это другая сборка мода (старая версия или тестовая) либо повреждённый файл.", "Если не ставили другую сборку специально - распакуйте архив мода 2.0 заново.");
                    return true;
                }
            }
            return true;
        }

        static bool CanRead(string p)
        {
            try { using (var fs = new FileStream(p, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) { if (fs.Length > 0) fs.ReadByte(); } return true; }
            catch { return false; }
        }

        static string Sha256(string p)
        {
            using (var sha = SHA256.Create())
            using (var fs = new FileStream(p, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1 << 20))
                return BitConverter.ToString(sha.ComputeHash(fs)).Replace("-", "").ToLowerInvariant();
        }

        static bool Contains(string p, string ascii)
        {
            byte[] d = File.ReadAllBytes(p), k = Encoding.ASCII.GetBytes(ascii);
            for (int i = 0; i + k.Length <= d.Length; i++) { int j = 0; while (j < k.Length && d[i + j] == k[j]) j++; if (j == k.Length) return true; }
            return false;
        }

        static string ReadGameVersion(string exe, CheckResult r)
        {
            byte[] d;
            try { d = File.ReadAllBytes(exe); }
            catch (Exception e) { Add(r, Sev.Error, "Исполняемый файл игры недоступен", exe, e.Message, "Закройте игру, проверьте права и антивирус."); return null; }
            byte[] k = Encoding.ASCII.GetBytes("v1.4.700.");
            for (int i = 0; i + k.Length + 4 < d.Length; i++) {
                int j = 0; while (j < k.Length && d[i + j] == k[j]) j++;
                if (j < k.Length) continue;
                int e = i + k.Length; while (e < d.Length && d[e] >= '0' && d[e] <= '9') e++;
                return Encoding.ASCII.GetString(d, i, e - i);
            }
            return null;
        }

        static Encoding IniEnc { get { try { return Encoding.GetEncoding(1251); } catch { return Encoding.Default; } } }

        static HashSet<string> IniSections(string p)
        {
            var s = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var l in File.ReadAllLines(p, IniEnc)) { var t = l.Trim(); if (t.StartsWith("[") && t.EndsWith("]")) s.Add(t.Substring(1, t.Length - 2)); }
            return s;
        }

        static string IniGet(string p, string sec, string key)
        {
            string cur = null;
            foreach (var l in File.ReadAllLines(p, IniEnc)) {
                var t = l.Trim();
                if (t.StartsWith("[")) { cur = t.Trim('[', ']'); continue; }
                if (t.StartsWith(";") || cur == null || !cur.Equals(sec, StringComparison.OrdinalIgnoreCase)) continue;
                int eq = t.IndexOf('='); if (eq < 0) continue;
                if (t.Substring(0, eq).Trim().Equals(key, StringComparison.OrdinalIgnoreCase)) { var v = t.Substring(eq + 1); int c = v.IndexOf(';'); return (c >= 0 ? v.Substring(0, c) : v).Trim(); }
            }
            return null;
        }

        static Dictionary<string, string> DxvkConf(string p)
        {
            var d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var l in File.ReadAllLines(p)) {
                var t = l.Trim(); if (t.Length == 0 || t[0] == '#' || t[0] == '[') continue;
                int eq = t.IndexOf('='); if (eq < 0) continue;
                d[t.Substring(0, eq).Trim()] = t.Substring(eq + 1).Trim().Trim('"');
            }
            return d;
        }

        public static string Report(GameInstall g, CheckResult r)
        {
            var sb = new StringBuilder();
            sb.AppendLine("Divinity II Reborn - проверка установки, " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
            sb.AppendLine("Папка игры: " + (g != null ? g.Root : "?"));
            sb.AppendLine("Итог: " + Ui.SevName(r.Overall) + "  (ошибок " + r.Count(Sev.Error) + ", предупреждений " + r.Count(Sev.Warn) + ")");
            foreach (var i in r.Items.OrderByDescending(i => (int)i.Severity)) {
                sb.AppendLine();
                sb.AppendLine("[" + Ui.SevName(i.Severity) + "] " + i.Title);
                if (!string.IsNullOrEmpty(i.Path)) sb.AppendLine("  путь: " + i.Path);
                if (!string.IsNullOrEmpty(i.Description)) sb.AppendLine("  " + i.Description);
                if (!string.IsNullOrEmpty(i.Fix)) sb.AppendLine("  что сделать: " + i.Fix);
            }
            return sb.ToString();
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // UI: borderless, owner-drawn, DPI-scaled.
    // ------------------------------------------------------------------------------------------------------------
    static class Ui
    {
        public static readonly Color Bg = Color.FromArgb(12, 12, 15), Panel = Color.FromArgb(22, 22, 27), PanelHi = Color.FromArgb(30, 30, 37),
            Line = Color.FromArgb(44, 44, 52), Text = Color.FromArgb(236, 233, 226), Dim = Color.FromArgb(150, 148, 142), Faint = Color.FromArgb(96, 95, 92),
            Accent = Color.FromArgb(214, 160, 82), AccentHi = Color.FromArgb(236, 186, 108), Ok = Color.FromArgb(96, 196, 126), Warn = Color.FromArgb(232, 178, 64), Err = Color.FromArgb(230, 88, 80), Info = Color.FromArgb(110, 150, 220);
        public static string SevName(Sev s) { return s == Sev.Ok ? "УСПЕШНО" : s == Sev.Warn ? "ПРЕДУПРЕЖДЕНИЕ" : s == Sev.Error ? "ОШИБКА" : "ИНФО"; }
        public static Color SevColor(Sev s) { return s == Sev.Ok ? Ok : s == Sev.Warn ? Warn : s == Sev.Error ? Err : Info; }
        public static GraphicsPath Round(RectangleF r, float rad)
        {
            var p = new GraphicsPath(); float d = rad * 2;
            if (rad <= 0) { p.AddRectangle(r); return p; }
            p.AddArc(r.X, r.Y, d, d, 180, 90); p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90); p.AddArc(r.X, r.Bottom - d, d, d, 90, 90); p.CloseFigure(); return p;
        }
        public static float Ease(float t) { t = Math.Max(0, Math.Min(1, t)); return 1 - (1 - t) * (1 - t) * (1 - t); }
        public static Color Mix(Color a, Color b, float t) { t = Math.Max(0, Math.Min(1, t)); return Color.FromArgb((int)(a.A + (b.A - a.A) * t), (int)(a.R + (b.R - a.R) * t), (int)(a.G + (b.G - a.G) * t), (int)(a.B + (b.B - a.B) * t)); }
        public static Color A(Color c, float a) { return Color.FromArgb((int)(Math.Max(0, Math.Min(1, a)) * c.A), c); }
        public static string FontFamilyName(params string[] names)
        {
            using (var ifc = new InstalledFontCollection())
                foreach (var n in names) if (ifc.Families.Any(f => f.Name.Equals(n, StringComparison.OrdinalIgnoreCase))) return n;
            return "Segoe UI";
        }
    }

    sealed class Btn
    {
        public RectangleF R; public string Text; public bool Primary, Enabled = true, Visible = true; public float Hover; public bool Down; public int Layer; public Action Click;   // Layer: -1 window chrome, 0 main, 1 check, 2 ReShade
    }

    sealed class LauncherForm : Form
    {
        readonly GameInstall game;
        readonly bool dryRun;
        readonly string dryLog;
        readonly float S;                               // DPI scale
        readonly Image bg;
        readonly bool bgFramed;                         // images\background.jpg has a painted frame and logo
        readonly string fTitle, fBody;
        readonly System.Windows.Forms.Timer anim = new System.Windows.Forms.Timer { Interval = 15 };
        readonly Stopwatch clock = Stopwatch.StartNew();

        // countdown
        const double CountdownSec = 5.0;
        enum Cd { Waiting, Running, Cancelled, Fired, Blocked }
        Cd cd = Cd.Waiting; double cdStart; Point? lastMouse; float cdFade = 1;
        // launch guard: 0 idle, 1 launching/launched
        int launchFlag;
        string statusLine; Color statusColor = Ui.Dim;

        // quick status at start-up
        CheckResult quick; bool quickRunning = true;

        // check view
        bool checkOpen; double checkOpenT; bool checkRunning; double checkProgress; string checkStep = "";
        CheckResult full; CancellationTokenSource cts; float scroll, scrollTarget, contentH;
        readonly List<Btn> buttons = new List<Btn>();
        Btn play, check, devBtn, closeCheck, copyReport, winClose, winMin;
        // ReShade helper
        Btn rsLink, rsAction, rsClose; readonly Btn[] rsChips = new Btn[4];
        bool dpMode, dpBusy; DpState dp; string dpStep = "", dpError, dpNote;
        bool rsOpen, rsBusy; double rsOpenT; RsState rs; string rsStep = "", rsError, rsNote; int rsSel = 1;

        public LauncherForm(GameInstall g, bool dry, string dryLogPath)
        {
            game = g; dryRun = dry; dryLog = dryLogPath;
            FormBorderStyle = FormBorderStyle.None; StartPosition = FormStartPosition.CenterScreen;
            DoubleBuffered = true; KeyPreview = true; BackColor = Ui.Bg; Text = "Divinity II Reborn";
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            using (var gr = CreateGraphics()) S = gr.DpiX / 96f;
            var wa = Screen.FromPoint(Cursor.Position).WorkingArea;
            // 960x600 logical; shrink to fit small screens
            float fit = Math.Min(1f, Math.Min(wa.Width * 0.92f / (960 * S), wa.Height * 0.92f / (600 * S)));
            S *= fit;
            ClientSize = new Size((int)(960 * S), (int)(600 * S));
            fTitle = Ui.FontFamilyName("Segoe UI Variable Display", "Segoe UI");
            fBody = Ui.FontFamilyName("Segoe UI Variable Text", "Segoe UI");
            try {
                if (g != null) {
                    string ico = Path.Combine(g.Root, @"images\game.ico");
                    if (File.Exists(ico)) Icon = new Icon(ico);
                    // 1) the game's wide key art from the Steam library cache (same art, no logo), 2) the original menu art
                    string hero = null;
                    try { using (var k = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam")) if (k != null && k.GetValue("SteamPath") is string) hero = Path.Combine(((string)k.GetValue("SteamPath")).Replace('/', '\\'), @"appcache\librarycache\219780\library_hero.jpg"); } catch { }
                    string bgp = Path.Combine(g.Root, @"images\background.jpg");
                    string pick = hero != null && File.Exists(hero) ? hero : File.Exists(bgp) ? bgp : null;
                    if (pick != null) { using (var fs = File.OpenRead(pick)) bg = Image.FromStream(new MemoryStream(ReadAll(fs))); bgFramed = pick == bgp; }
                }
            } catch { bg = null; }

            play = AddBtn("ИГРАТЬ", true, () => { CancelCountdown(); Launch("кнопка ИГРАТЬ"); });
            check = AddBtn("ПРОВЕРИТЬ УСТАНОВКУ МОДА", false, () => { CancelCountdown(); OpenCheck(); });
            devBtn = AddBtn("Режим разработчика", false, () => { CancelCountdown(); Launch("режим разработчика", true); });
            devBtn.Visible = g != null && File.Exists(Path.Combine(Path.GetDirectoryName(g.Exe), "Divinity2-debug.exe"));
            closeCheck = AddBtn("ЗАКРЫТЬ", true, CloseCheck); closeCheck.Visible = false;
            copyReport = AddBtn("КОПИРОВАТЬ ОТЧЁТ", false, CopyReport); copyReport.Visible = false;
            winClose = AddBtn("✕", false, () => { CancelCountdown(); Close(); });
            winMin = AddBtn("–", false, () => { CancelCountdown(); WindowState = FormWindowState.Minimized; });
            winClose.Layer = winMin.Layer = -1; closeCheck.Layer = copyReport.Layer = 1;
            rsLink = AddBtn("", false, () => { CancelCountdown(); if (dpBusy || (dp != null && !dp.Ready)) OpenDp(); else OpenRs(); }); rsLink.Visible = g != null;
            rsAction = AddBtn("Установить ReShade", true, RsAction); rsAction.Layer = 2; rsAction.Visible = false;
            rsClose = AddBtn("Закрыть", false, CloseRs); rsClose.Layer = 2; rsClose.Visible = false;
            for (int i = 0; i < 4; i++) { int k = i; rsChips[i] = AddBtn(ReShadeHelper.PresetNames[i], false, () => PickPreset(k)); rsChips[i].Layer = 2; rsChips[i].Visible = false; }
            Layout_();

            anim.Tick += (s, e) => Tick();
            Shown += (s, e) => { anim.Start(); StartQuickCheck(); RefreshRs(); StartMusic(); Activate(); };
            FormClosing += (s, e) => { if (cts != null) cts.Cancel(); anim.Stop(); StopMusic(); };
            if (g == null) { statusLine = "Папка игры не найдена - положите лаунчер в папку Divinity II."; statusColor = Ui.Err; }
        }

        // ---------------- music: the original menu's images\background.wav, played much quieter ----------------
        const float MusicVolume = 0.25f;                // ~ -12 dB against the original menu
        System.Media.SoundPlayer music; volatile bool musicStopped;
        void StartMusic()
        {
            if (game == null) return;
            string wav = Path.Combine(game.Root, "images", "background.wav");
            if (!File.Exists(wav)) return;
            Task.Run(() => {
                try {
                    byte[] d = File.ReadAllBytes(wav);
                    // only plain 16-bit PCM is rescaled; anything else is skipped rather than played loud
                    if (d.Length < 44 || Encoding.ASCII.GetString(d, 0, 4) != "RIFF" || Encoding.ASCII.GetString(d, 8, 4) != "WAVE") return;
                    int pos = 12, data = -1, dataLen = 0, ch = 2, rate = 44100, bits = 0, fmt = 0;
                    while (pos + 8 <= d.Length) {
                        string id = Encoding.ASCII.GetString(d, pos, 4); int len = BitConverter.ToInt32(d, pos + 4);
                        if (id == "fmt ") { fmt = BitConverter.ToInt16(d, pos + 8); ch = BitConverter.ToInt16(d, pos + 10); rate = BitConverter.ToInt32(d, pos + 12); bits = BitConverter.ToInt16(d, pos + 22); }
                        else if (id == "data") { data = pos + 8; dataLen = Math.Min(len, d.Length - data); break; }
                        pos += 8 + len + (len & 1);
                    }
                    if (fmt != 1 || bits != 16 || data < 0) return;
                    int fade = rate * ch * 2;           // 2 s fade-in
                    for (int i = 0, n = 0; i + 1 < dataLen; i += 2, n++) {
                        float gain = MusicVolume * (n < fade ? (float)n / fade : 1f);
                        short v = (short)(BitConverter.ToInt16(d, data + i) * gain);
                        d[data + i] = (byte)v; d[data + i + 1] = (byte)(v >> 8);
                    }
                    if (musicStopped) return;
                    var sp = new System.Media.SoundPlayer(new MemoryStream(d));
                    sp.Load();
                    lock (this) { if (musicStopped) return; music = sp; sp.Play(); }
                } catch { }                             // music is decoration - never block the launcher
            });
        }
        void StopMusic() { lock (this) { musicStopped = true; if (music != null) { try { music.Stop(); } catch { } } } }

        static byte[] ReadAll(Stream s) { var m = new MemoryStream(); s.CopyTo(m); return m.ToArray(); }
        Btn AddBtn(string t, bool primary, Action a) { var b = new Btn { Text = t, Primary = primary, Click = a }; buttons.Add(b); return b; }
        float X(float v) { return v * S; }
        double Now { get { return clock.Elapsed.TotalSeconds; } }

        void Layout_()
        {
            float w = ClientSize.Width / S;
            play.R = new RectangleF(X(w / 2 - 170), X(266), X(340), X(68));
            check.R = new RectangleF(X(w / 2 - 170), X(418), X(340), X(44));
            closeCheck.R = new RectangleF(X(w - 40 - 160), X(600 - 40 - 46), X(160), X(46));
            copyReport.R = new RectangleF(X(w - 40 - 160 - 16 - 200), X(600 - 40 - 46), X(200), X(46));
            winClose.R = new RectangleF(X(w - 46), X(0), X(46), X(34));
            winMin.R = new RectangleF(X(w - 92), X(0), X(46), X(34));
            devBtn.R = new RectangleF(X(w - 40 - 170), X(600 - 26 - 4), X(170), X(26));
            rsLink.R = new RectangleF(X(w / 2 - 170), X(348), X(340), X(58));
            rsAction.R = new RectangleF(X(w - 40 - 230), X(600 - 40 - 46), X(230), X(46));
            rsClose.R = new RectangleF(X(w - 40 - 230 - 16 - 150), X(600 - 40 - 46), X(150), X(46));
            float cw = (w - 80 - 56 - 30) / 4f;
            for (int i = 0; i < 4; i++) rsChips[i].R = new RectangleF(X(68 + i * (cw + 10)), X(46 + 296), X(cw), X(40));
        }

        // ---------------- countdown ----------------
        void CancelCountdown()
        {
            if (cd == Cd.Running || cd == Cd.Waiting) { cd = Cd.Cancelled; Invalidate(); }
        }

        void Tick()
        {
            double t = Now;
            // countdown starts once the intro animation has finished (menu fully shown)
            // ...and only when the quick install check found no errors: a broken install must not start the game by itself
            if (cd == Cd.Waiting && t > 0.9 && !checkOpen && !rsOpen && (game == null || !quickRunning)) {
                if (game == null || quick == null || quick.Overall == Sev.Error) cd = Cd.Blocked;
                else { cd = Cd.Running; cdStart = t; }
            }
            if (cd == Cd.Running) {
                if (checkOpen || rsOpen || !Visible || WindowState == FormWindowState.Minimized) cd = Cd.Cancelled;
                else if (t - cdStart >= CountdownSec) { cd = Cd.Fired; Launch("автозапуск"); }
            }
            if (cd == Cd.Cancelled) cdFade = Math.Max(0, cdFade - 0.06f);

            var mp = PointToClient(Cursor.Position);
            bool inside = ClientRectangle.Contains(mp);
            foreach (var b in buttons) {
                bool h = b.Visible && b.Enabled && inside && b.R.Contains(mp) && (b.Layer == -1 || b.Layer == CurLayer());
                b.Hover += ((h ? 1 : 0) - b.Hover) * 0.2f;
            }
            Cursor = buttons.Any(b => b.Visible && b.Enabled && b.R.Contains(mp) && (b.Layer == -1 || b.Layer == CurLayer())) ? Cursors.Hand : Cursors.Default;
            scroll += (scrollTarget - scroll) * 0.25f;
            Invalidate();
        }

        int CurLayer() { return rsOpen ? 2 : checkOpen ? 1 : 0; }

        // ---------------- launch ----------------
        void Launch(string why, bool dev = false)
        {
            if (Interlocked.CompareExchange(ref launchFlag, 1, 0) != 0) return;     // second press / timer + click
            play.Enabled = false; check.Enabled = false; devBtn.Enabled = false;
            string err = null;
            string exe = game == null ? null : dev ? Path.Combine(Path.GetDirectoryName(game.Exe), "Divinity2-debug.exe") : game.Exe;
            try {
                if (game == null) err = "Папка игры не найдена.\n\nПоложите лаунчер в папку Divinity II: Developer's Cut (рядом с bin и Data).";
                else if (!File.Exists(exe)) err = "Не найден исполняемый файл игры:\n" + exe + "\n\nПроверьте целостность файлов в Steam (Свойства > Установленные файлы).";
                else if (!Directory.Exists(game.WorkDir)) err = "Не найдена рабочая папка игры:\n" + game.WorkDir;
                else if (Process.GetProcessesByName("Divinity2").Length + Process.GetProcessesByName("Divinity2-debug").Length > 0) err = "Divinity II уже запущена.";
                else {
                    try { using (File.Open(exe, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) { } }
                    catch (Exception e) { err = "Нет доступа к файлу игры:\n" + exe + "\n\n" + e.Message; }
                }
                if (err == null) {
                    if (dryRun) {
                        File.AppendAllText(dryLog, DateTime.Now.ToString("HH:mm:ss.fff") + " LAUNCH (" + why + ") exe=" + exe + " cwd=" + game.WorkDir + " args=[" + game.Args + "] t=" + Now.ToString("0.00") + "\r\n");
                        statusLine = "Тест: игра была бы запущена (" + why + ")."; statusColor = Ui.Ok;
                        BeginCloseSoon(); return;
                    }
                    var psi = new ProcessStartInfo(exe, game.Args) { WorkingDirectory = game.WorkDir, UseShellExecute = false };
                    var p = Process.Start(psi);
                    if (p == null) err = "Windows не запустила процесс игры.";
                    else {
                        statusLine = "Запуск Divinity II..."; statusColor = Ui.Accent;
                        WatchStart(p); return;
                    }
                }
            } catch (System.ComponentModel.Win32Exception e) { err = "Не удалось запустить игру:\n" + exe + "\n\n" + e.Message + " (код " + e.NativeErrorCode + ")"; }
            catch (Exception e) { err = "Не удалось запустить игру:\n" + e.Message; }
            Fail(err);
        }

        void Fail(string err)
        {
            statusLine = "Игра не запущена."; statusColor = Ui.Err;
            MessageBox.Show(this, err, "Divinity II Reborn", MessageBoxButtons.OK, MessageBoxIcon.Error);
            Interlocked.Exchange(ref launchFlag, 0);
            play.Enabled = true; check.Enabled = true; devBtn.Enabled = true;
        }

        // close once the game is clearly running; report an immediate crash instead of vanishing
        void WatchStart(Process p)
        {
            Task.Run(() => {
                bool exited = false; int code = 0;
                try { exited = p.WaitForExit(2500); if (exited) code = p.ExitCode; } catch { }
                BeginInvoke((Action)(() => {
                    if (exited && code != 0) Fail("Игра закрылась сразу после запуска (код выхода 0x" + code.ToString("X8") + ").\n\nЗапустите «Проверить установку мода» и посмотрите bin\\Div2Fix.log.");
                    else Close();
                }));
            });
        }

        void BeginCloseSoon() { var t = new System.Windows.Forms.Timer { Interval = 700 }; t.Tick += (s, e) => { t.Stop(); Close(); }; t.Start(); }

        // ---------------- checks ----------------
        void StartQuickCheck()
        {
            var g = game;
            Task.Run(() => ModChecker.Run(g, true, CancellationToken.None, null)).ContinueWith(t => {
                if (IsDisposed) return;
                try { BeginInvoke((Action)(() => { quick = t.Result; quickRunning = false; })); } catch { }
            });
        }

        void OpenCheck()
        {
            if (checkOpen) return;
            checkOpen = true; checkOpenT = Now; checkRunning = true; full = null; checkProgress = 0; checkStep = "Подготовка"; scroll = scrollTarget = 0;
            closeCheck.Visible = true; copyReport.Visible = true; copyReport.Enabled = false;
            cts = new CancellationTokenSource(); var token = cts.Token; var g = game;
            Task.Run(() => ModChecker.Run(g, false, token, (x, s) => {
                try { BeginInvoke((Action)(() => { checkProgress = x; checkStep = s; })); } catch { }
            }), token).ContinueWith(t => {
                if (t.IsCanceled || IsDisposed) return;
                try {
                    BeginInvoke((Action)(() => {
                        if (token.IsCancellationRequested) return;
                        full = t.IsFaulted ? new CheckResult { Crashed = true, Items = { new Finding(Sev.Error, "Проверка завершилась с ошибкой", g != null ? g.Root : "", t.Exception.GetBaseException().Message, "Запустите проверку ещё раз.") } } : t.Result;
                        quick = full; checkRunning = false; copyReport.Enabled = true;
                    }));
                } catch { }
            });
        }

        void CloseCheck()
        {
            if (cts != null) cts.Cancel();
            checkOpen = false; checkRunning = false; closeCheck.Visible = false; copyReport.Visible = false;
            // countdown stays cancelled - it never restarts by itself
        }

        // ---------------- ReShade ----------------
        void RefreshRs()
        {
            RefreshDp();
            var g = game;
            Task.Run(() => ReShadeHelper.Detect(g)).ContinueWith(t => {
                if (IsDisposed || t.IsFaulted) return;
                try {
                    BeginInvoke((Action)(() => {
                        rs = t.Result;
                        int i = Array.FindIndex(ReShadeHelper.PresetFiles, f => f.Equals(rs.PresetFile, StringComparison.OrdinalIgnoreCase));
                        if (i >= 0 && !rsBusy) rsSel = i;
                    }));
                } catch { }
            });
        }

        void OpenRs()
        {
            if (rsOpen || checkOpen || game == null) return;
            rsOpen = true; dpMode = false; rsOpenT = Now; rsNote = null; if (!rsBusy) rsError = null;
            rsAction.Visible = rsClose.Visible = true; foreach (var c in rsChips) c.Visible = true;
            RefreshRs();
        }

        void CloseRs()
        {
            rsOpen = false; rsAction.Visible = rsClose.Visible = false; foreach (var c in rsChips) c.Visible = false;   // an install keeps running
        }

        void RsAction()
        {
            if (dpMode) { DpAction(); return; }
            if (rsBusy) return;
            if (rs != null && rs.Ready) { CloseRs(); return; }
            rsBusy = true; rsError = null; rsNote = null; rsStep = "Подготовка";
            var g = game; int preset = rsSel;
            Task.Run(() => ReShadeHelper.Install(g, preset, st => { try { BeginInvoke((Action)(() => rsStep = st)); } catch { } }))
                .ContinueWith(t => {
                    if (IsDisposed) return;
                    try {
                        BeginInvoke((Action)(() => {
                            rsBusy = false;
                            if (t.IsFaulted) { var ex = t.Exception.GetBaseException(); rsError = ex is RsError ? ex.Message : "Ошибка установки ReShade: " + ex.Message; }
                            else rsNote = "ReShade установлен. В игре меню ReShade открывается клавишей Home.";
                            RefreshRs();
                        }));
                    } catch { }
                });
        }

        void PickPreset(int k)
        {
            rsSel = k; rsError = null; rsNote = null;
            if (rsBusy || rs == null || !rs.IniExists || !rs.Presets) return;    // remembered and applied by the install
            try { ReShadeHelper.SetPreset(game, k); rsNote = "Пресет «" + ReShadeHelper.PresetNames[k] + "» включится при следующем запуске игры."; }
            catch (Exception e) { rsError = "Не удалось записать ReShade.ini: " + e.Message; }
            RefreshRs();
        }

        string PresetName(RsState s)
        {
            int i = s == null ? -1 : Array.FindIndex(ReShadeHelper.PresetFiles, f => f.Equals(s.PresetFile, StringComparison.OrdinalIgnoreCase));
            return i >= 0 ? ReShadeHelper.PresetNames[i] : s != null && s.PresetFile != null ? s.PresetFile : "не выбран";
        }

        // ---------------- Div2Patch (the ReShade panel's frame and buttons) ----------------
        void RefreshDp()
        {
            var g = game;
            Task.Run(() => Div2PatchHelper.Detect(g)).ContinueWith(t => {
                if (IsDisposed || t.IsFaulted) return;
                try { BeginInvoke((Action)(() => dp = t.Result)); } catch { }
            });
        }

        void OpenDp()
        {
            if (rsOpen || checkOpen || game == null) return;
            rsOpen = true; dpMode = true; rsOpenT = Now; dpNote = null; if (!dpBusy) dpError = null;
            rsAction.Visible = rsClose.Visible = true;
            RefreshDp();
        }

        void DpAction()
        {
            if (dpBusy) return;
            if (dp != null && dp.Ready) { CloseRs(); return; }
            dpBusy = true; dpError = null; dpNote = null; dpStep = "Подготовка";
            var g = game;
            Task.Run(() => Div2PatchHelper.Install(g, st => { try { BeginInvoke((Action)(() => dpStep = st)); } catch { } }))
                .ContinueWith(t => {
                    if (IsDisposed) return;
                    try {
                        BeginInvoke((Action)(() => {
                            dpBusy = false;
                            if (t.IsFaulted) { var ex = t.Exception.GetBaseException(); dpError = ex is RsError ? ex.Message : "Ошибка установки Div2Patch: " + ex.Message; }
                            else dpNote = "Div2Patch 0.97 установлен. Теперь можно играть.";
                            RefreshDp(); StartQuickCheck();
                        }));
                    } catch { }
                });
        }

        void PaintDp(Graphics g, RectangleF panel, float x0, float iw, float a, float dy, double t)
        {
            using (var f = F(fTitle, 22, FontStyle.Bold)) DrawText(g, "Div2Patch", f, Ui.A(Ui.Text, a), new RectangleF(x0, panel.Y + X(20), X(400), X(32)));
            using (var f = F(fBody, 12.5f)) DrawText(g, "Обязательный фанатский патч от BlanknameES: мод рассчитан на версию игры, которую он ставит.", f, Ui.A(Ui.Dim, a), new RectangleF(x0, panel.Y + X(54), iw, X(20)));
            var s = dp;
            var rows = new[] {
                Tuple.Create(s != null && s.Dll, "Div2Patch в папке bin", s == null ? "проверяю" : s.Dll ? "на месте" : "не установлен"),
                Tuple.Create(s != null && s.Exe, "Игра версии v1.4.700.56", s == null ? "проверяю" : s.Exe ? "подходит" : "нужна установка"),
            };
            float y = panel.Y + X(100);
            using (var fT = F(fBody, 14)) using (var fD = F(fBody, 12.5f))
            foreach (var row in rows) {
                using (var b = new SolidBrush(Ui.A(row.Item1 ? Ui.Ok : Ui.Faint, a))) g.FillEllipse(b, x0, y + X(9), X(9), X(9));
                DrawText(g, row.Item2, fT, Ui.A(Ui.Text, a), new RectangleF(x0 + X(20), y, X(360), X(28)), StringAlignment.Near, StringAlignment.Center);
                DrawText(g, row.Item3, fD, Ui.A(row.Item1 ? Ui.Dim : Ui.Mix(Ui.Dim, Ui.AccentHi, 0.5f), a), new RectangleF(x0 + X(380), y, iw - X(380), X(28)), StringAlignment.Far, StringAlignment.Center);
                y += X(30);
            }
            var msg = new RectangleF(x0, panel.Y + X(230), iw, X(120));
            if (dpBusy) {
                using (var p = new Pen(Ui.A(Ui.Accent, a), X(3)) { StartCap = LineCap.Round, EndCap = LineCap.Round }) g.DrawArc(p, msg.X, msg.Y + X(2), X(20), X(20), (float)(t * 360 % 360), 260);
                using (var f = F(fBody, 14)) DrawText(g, dpStep, f, Ui.A(Ui.Text, a), new RectangleF(msg.X + X(32), msg.Y, msg.Width - X(32), X(24)), StringAlignment.Near, StringAlignment.Center);
            } else if (dpError != null) {
                using (var f = F(fBody, 13)) DrawText(g, dpError, f, Ui.A(Ui.Err, a), msg, wrap: true);
            } else if (dpNote != null) {
                using (var f = F(fBody, 13)) DrawText(g, dpNote, f, Ui.A(Ui.Ok, a), msg, wrap: true);
            } else if (s != null && !s.Ready) {
                using (var f = F(fBody, 12.5f)) DrawText(g, "Лаунчер скачает Div2Patch 0.97 с GitHub автора, сверит контрольную сумму и установит. Он заменит Divinity2.exe и globalswitches.xml, прежние файлы сохранятся в _Div2Reborn\\Div2Patch_backup. Игра должна быть закрыта.", f, Ui.A(Ui.Faint, a), msg, wrap: true);
            }
            rsAction.Text = dpBusy ? "Устанавливаю…" : s != null && s.Ready ? "Готово" : "Установить Div2Patch";
            rsAction.Enabled = !dpBusy && s != null;
            PaintBtn(g, rsClose, a, dy);
            PaintBtn(g, rsAction, a, dy);
        }

        // ReShade is a key part of the mod: a full button between ИГРАТЬ and the install check, with its state inside
        void PaintRsLink(Graphics g, float a, float dy)
        {
            if (!rsLink.Visible || a <= 0.01f) return;
            var r = rsLink.R; r.Offset(0, dy);
            if (rsLink.Down) r.Inflate(-X(1.5f), -X(1.5f));
            float h = rsLink.Hover; double t = Now;
            bool needDp = dpBusy || (dp != null && !dp.Ready);
            bool ready = rs != null && rs.Ready && !rsBusy && !needDp;
            string sub; Color subC, dot;
            if (dpBusy) { sub = dpStep; subC = Ui.AccentHi; dot = Ui.Accent; }
            else if (needDp) { sub = "Обязателен для мода. Установить в один щелчок"; subC = Ui.AccentHi; dot = Ui.Warn; }
            else if (rsBusy) { sub = rsStep; subC = Ui.AccentHi; dot = Ui.Accent; }
            else if (rs == null) { sub = "Проверяю"; subC = Ui.Faint; dot = Ui.Faint; }
            else if (ready) { sub = "Готов, пресет «" + PresetName(rs) + "»"; subC = Ui.Dim; dot = Ui.Ok; }
            else { sub = "Не установлен. Установить в один щелчок"; subC = Ui.AccentHi; dot = Ui.Warn; }
            Color edge = ready ? Ui.Mix(Ui.Accent, Ui.Line, 0.35f) : Ui.AccentHi;
            using (var path = Ui.Round(r, X(9))) {
                if (!ready && h < 0.01f) using (var gp = Ui.Round(RectangleF.Inflate(r, X(3), X(3)), X(12))) using (var gb = new SolidBrush(Ui.A(Ui.Accent, a * (0.06f + 0.05f * (float)Math.Sin(t * 3))))) g.FillPath(gb, gp);
                using (var br = new SolidBrush(Ui.A(Ui.Mix(Color.FromArgb(34, 30, 26), Color.FromArgb(48, 40, 32), h), a * 0.95f))) g.FillPath(br, path);
                using (var pn = new Pen(Ui.A(Ui.Mix(edge, Ui.AccentHi, h), a), X(1.3f))) g.DrawPath(pn, path);
            }
            using (var f = F(fTitle, 16, FontStyle.Bold)) DrawSpaced(g, needDp ? "DIV2PATCH" : "RESHADE", f, Ui.A(Ui.Mix(Ui.Accent, Ui.AccentHi, 0.5f + 0.5f * h), a), r.X + r.Width / 2, r.Y + X(9), X(4));
            using (var f = F(fBody, 12.5f)) DrawText(g, sub, f, Ui.A(Ui.Mix(subC, Ui.Text, h * 0.4f), a), new RectangleF(r.X + X(30), r.Y + X(32), r.Width - X(60), X(18)), StringAlignment.Center, StringAlignment.Center);
            float pulse = rsBusy || dpBusy || needDp || (rs != null && !ready) ? 0.6f + 0.4f * (float)Math.Sin(t * 5) : 1;
            using (var b = new SolidBrush(Ui.A(dot, a * pulse))) g.FillEllipse(b, r.X + X(18), r.Y + r.Height / 2 - X(4), X(8), X(8));
            using (var f = F(fBody, 20)) DrawText(g, "›", f, Ui.A(Ui.Mix(Ui.Faint, Ui.AccentHi, h), a), new RectangleF(r.Right - X(34), r.Y, X(24), r.Height), StringAlignment.Center, StringAlignment.Center);
        }

        void PaintRs(Graphics g, float W, float H, double t)
        {
            float a = Ui.Ease((float)((t - rsOpenT) / 0.35)), dy = (1 - a) * X(20);
            using (var b = new SolidBrush(Ui.A(Color.FromArgb(200, 8, 8, 10), a))) g.FillRectangle(b, 0, X(34), W, H - X(34));
            var panel = new RectangleF(X(40), X(46) + dy, W - X(80), H - X(46) - X(110));
            using (var path = Ui.Round(panel, X(12))) {
                using (var b = new SolidBrush(Ui.A(Ui.Panel, a * 0.97f))) g.FillPath(b, path);
                using (var p = new Pen(Ui.A(Ui.Line, a))) g.DrawPath(p, path);
            }
            float x0 = panel.X + X(28), iw = panel.Width - X(56);
            if (dpMode) { PaintDp(g, panel, x0, iw, a, dy, t); return; }
            using (var f = F(fTitle, 22, FontStyle.Bold)) DrawText(g, "ReShade", f, Ui.A(Ui.Text, a), new RectangleF(x0, panel.Y + X(20), X(400), X(32)));
            using (var f = F(fBody, 12.5f)) DrawText(g, "Резкость и цвет поверх сглаживания мода. Лаунчер сам скачает и настроит всё нужное.", f, Ui.A(Ui.Dim, a), new RectangleF(x0, panel.Y + X(54), iw, X(20)));

            var s = rs;
            var rows = new List<Tuple<bool, string, string>>();
            if (s == null) rows.Add(Tuple.Create(false, "Проверяю ReShade", ""));
            else {
                if (s.ConflictFile != null) rows.Add(Tuple.Create(false, "Найден ReShade в режиме DirectX 9", Path.GetFileName(s.ConflictFile) + " мешает DXVK"));
                rows.Add(Tuple.Create(s.Layer, "ReShade для Vulkan", s.Layer ? "версия " + (s.Version ?? "?") : "не установлен"));
                rows.Add(Tuple.Create(s.App, "Подключён к Divinity2.exe", s.App ? "да" : "нет"));
                rows.Add(Tuple.Create(s.Effects, "Эффекты Standard и SweetFX", s.Effects ? "на месте" : "не хватает: " + string.Join(", ", s.MissingFx)));
                rows.Add(Tuple.Create(s.Presets, "Пресеты мода в папке игры", s.Presets ? "4 из 4" : "не скопированы"));
            }
            float y = panel.Y + X(90);
            using (var fT = F(fBody, 14)) using (var fD = F(fBody, 12.5f))
            foreach (var row in rows) {
                Color dc = row.Item1 ? Ui.Ok : (s != null && s.ConflictFile != null && row.Item2.StartsWith("Найден") ? Ui.Err : Ui.Faint);
                using (var b = new SolidBrush(Ui.A(dc, a))) g.FillEllipse(b, x0, y + X(9), X(9), X(9));
                DrawText(g, row.Item2, fT, Ui.A(Ui.Text, a), new RectangleF(x0 + X(20), y, X(360), X(28)), StringAlignment.Near, StringAlignment.Center);
                DrawText(g, row.Item3, fD, Ui.A(row.Item1 ? Ui.Dim : Ui.Mix(Ui.Dim, Ui.AccentHi, 0.5f), a), new RectangleF(x0 + X(380), y, iw - X(380), X(28)), StringAlignment.Far, StringAlignment.Center);
                y += X(30);
            }

            using (var f = F(fBody, 13)) DrawText(g, "Пресет", f, Ui.A(Ui.Dim, a), new RectangleF(x0, panel.Y + X(268), X(300), X(22)));
            for (int i = 0; i < 4; i++) { rsChips[i].Primary = i == rsSel; rsChips[i].Enabled = !rsBusy; PaintBtn(g, rsChips[i], a, dy); }

            var msg = new RectangleF(x0, panel.Y + X(350), iw, X(80));
            if (rsBusy) {
                using (var p = new Pen(Ui.A(Ui.Accent, a), X(3)) { StartCap = LineCap.Round, EndCap = LineCap.Round }) g.DrawArc(p, msg.X, msg.Y + X(2), X(20), X(20), (float)(t * 360 % 360), 260);
                using (var f = F(fBody, 14)) DrawText(g, rsStep, f, Ui.A(Ui.Text, a), new RectangleF(msg.X + X(32), msg.Y, msg.Width - X(32), X(24)), StringAlignment.Near, StringAlignment.Center);
            } else if (rsError != null) {
                using (var f = F(fBody, 13)) DrawText(g, rsError, f, Ui.A(Ui.Err, a), msg, wrap: true);
            } else if (rsNote != null) {
                using (var f = F(fBody, 13)) DrawText(g, rsNote, f, Ui.A(Ui.Ok, a), msg, wrap: true);
            } else if (s != null && !s.Ready) {
                using (var f = F(fBody, 12.5f)) DrawText(g, "Лаунчер скачает ReShade с reshade.me и эффекты с GitHub. Windows один раз спросит права администратора: так ReShade подключается к Vulkan. Ваши файлы не удаляются, ReShade.ini сохраняется в ReShade.ini.div2bak.", f, Ui.A(Ui.Faint, a), msg, wrap: true);
            } else if (s != null) {
                using (var f = F(fBody, 12.5f)) DrawText(g, "В игре меню ReShade открывается клавишей Home. Сглаживание SMAA в ReShade не включайте: оно уже есть в моде.", f, Ui.A(Ui.Faint, a), msg, wrap: true);
            }

            rsAction.Text = rsBusy ? "Устанавливаю…" : s != null && s.Ready ? "Готово" : "Установить ReShade";
            rsAction.Enabled = !rsBusy && s != null;
            PaintBtn(g, rsClose, a, dy);
            PaintBtn(g, rsAction, a, dy);
        }

        void CopyReport()
        {
            if (full == null) return;
            try { Clipboard.SetText(ModChecker.Report(game, full)); statusLine = "Отчёт скопирован в буфер обмена."; statusColor = Ui.Ok; } catch { }
        }

        // ---------------- input ----------------
        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            CancelCountdown();
            foreach (var b in buttons) if (Hit(b, e.Location)) { b.Down = true; Capture = true; return; }
            if (e.Button == MouseButtons.Left && e.Y < X(60)) { ReleaseCapture(); SendMessage(Handle, 0xA1, (IntPtr)2, IntPtr.Zero); }   // drag window
        }
        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            foreach (var b in buttons) { bool was = b.Down; b.Down = false; if (was && Hit(b, e.Location) && b.Click != null) b.Click(); }
        }
        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            // moving the mouse noticeably over the window counts as interaction
            if (lastMouse.HasValue) { var d = Math.Abs(e.X - lastMouse.Value.X) + Math.Abs(e.Y - lastMouse.Value.Y); if (d > X(24)) CancelCountdown(); else return; }
            lastMouse = e.Location;
        }
        protected override void OnMouseWheel(MouseEventArgs e)
        {
            base.OnMouseWheel(e); CancelCountdown();
            if (checkOpen) scrollTarget = Math.Max(0, Math.Min(Math.Max(0, contentH), scrollTarget - e.Delta * X(0.6f)));
        }
        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            CancelCountdown();
            if (keyData == Keys.Escape) { if (rsOpen) CloseRs(); else if (checkOpen) CloseCheck(); else Close(); return true; }
            if (rsOpen && (keyData == Keys.Enter || keyData == Keys.Space)) return true;
            if (keyData == Keys.Enter || keyData == Keys.Space) { if (checkOpen) { if (!checkRunning) CloseCheck(); } else if (play.Enabled) play.Click(); return true; }
            return base.ProcessCmdKey(ref msg, keyData);
        }
        bool Hit(Btn b, Point p)
        {
            if (!b.Visible || !b.Enabled || !b.R.Contains(p)) return false;
            if (b == winClose || b == winMin) return true;
            return b.Layer == -1 || b.Layer == CurLayer();
        }
        [DllImport("user32.dll")] static extern bool ReleaseCapture();
        [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, IntPtr l);

        protected override CreateParams CreateParams
        {
            get { var cp = base.CreateParams; cp.Style |= 0x00020000 /*WS_MINIMIZEBOX*/; cp.ClassStyle |= 0x00020000 /*CS_DROPSHADOW*/; return cp; }
        }

        // ---------------- painting ----------------
        Font F(string fam, float pt, FontStyle st = FontStyle.Regular) { return new Font(fam, pt * S, st, GraphicsUnit.Pixel); }

        void DrawText(Graphics g, string s, Font f, Color c, RectangleF r, StringAlignment h = StringAlignment.Near, StringAlignment v = StringAlignment.Near, bool wrap = false)
        {
            using (var sf = new StringFormat { Alignment = h, LineAlignment = v, Trimming = StringTrimming.EllipsisCharacter })
            using (var b = new SolidBrush(c)) {
                if (!wrap) sf.FormatFlags |= StringFormatFlags.NoWrap;
                g.DrawString(s, f, b, r, sf);
            }
        }

        // letter-spaced caps
        void DrawSpaced(Graphics g, string s, Font f, Color c, float cx, float y, float spacing)
        {
            float sp = g.MeasureString("a a", f, PointF.Empty, StringFormat.GenericTypographic).Width - g.MeasureString("aa", f, PointF.Empty, StringFormat.GenericTypographic).Width;
            var sizes = s.Select(ch => ch == ' ' ? sp : g.MeasureString(ch.ToString(), f, PointF.Empty, StringFormat.GenericTypographic).Width).ToArray();
            float w = sizes.Sum() + spacing * (s.Length - 1), x = cx - w / 2;
            using (var b = new SolidBrush(c))
                for (int i = 0; i < s.Length; i++) { g.DrawString(s[i].ToString(), f, b, x, y, StringFormat.GenericTypographic); x += sizes[i] + spacing; }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics; g.SmoothingMode = SmoothingMode.AntiAlias; g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit; g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            float W = ClientSize.Width, H = ClientSize.Height; double t = Now;
            float intro = Ui.Ease((float)(t / 0.8));

            // background art, darkened, with a vertical fade
            if (bg != null) {
                RectangleF src = bgFramed ? new RectangleF(bg.Width * 0.03f, bg.Height * 0.04f, bg.Width * 0.94f, bg.Height * 0.92f) : new RectangleF(0, 0, bg.Width, bg.Height);
                float sc = Math.Max(W / src.Width, H / src.Height) * (1.04f - 0.04f * intro);
                float bw = src.Width * sc, bh = src.Height * sc;
                float bx = bgFramed ? (W - bw) / 2 : -Math.Min(W * 0.02f, bw - W);   // wide art: keep the heroine in view
                g.DrawImage(bg, new RectangleF(bx, (H - bh) / 2, bw, bh), src, GraphicsUnit.Pixel);
            }
            using (var br = new LinearGradientBrush(new RectangleF(0, 0, W, H), Color.FromArgb(bg != null ? 222 : 255, Ui.Bg), Color.FromArgb(250, Ui.Bg), 90f)) g.FillRectangle(br, 0, 0, W, H);
            using (var path = new GraphicsPath()) {
                path.AddEllipse(-W * 0.2f, -H * 0.6f, W * 1.4f, H * 1.3f);
                using (var pb = new PathGradientBrush(path) { CenterColor = Color.FromArgb(40, Ui.Accent), SurroundColors = new[] { Color.FromArgb(0, Ui.Accent) } }) g.FillPath(pb, path);
            }
            using (var p = new Pen(Ui.Line)) g.DrawRectangle(p, 0, 0, W - 1, H - 1);

            // window buttons
            foreach (var b in new[] { winMin, winClose }) {
                if (b.Hover > 0.01f) using (var br = new SolidBrush(Ui.A(b == winClose ? Ui.Err : Ui.PanelHi, b.Hover))) g.FillRectangle(br, b.R);
                using (var f = F(fBody, 13)) DrawText(g, b.Text, f, Ui.Mix(Ui.Dim, Ui.Text, b.Hover), b.R, StringAlignment.Center, StringAlignment.Center);
            }
            using (var f = F(fBody, 11.5f)) DrawText(g, "DIVINITY II REBORN · ЛАУНЧЕР", f, Ui.Faint, new RectangleF(X(20), 0, X(400), X(34)), StringAlignment.Near, StringAlignment.Center);

            float mainA = (checkOpen || rsOpen) ? 1 - Ui.Ease((float)((t - (rsOpen ? rsOpenT : checkOpenT)) / 0.25)) : 1;
            if (mainA > 0.01f) PaintMain(g, W, H, t, intro * mainA);
            if (checkOpen) PaintCheck(g, W, H, t);
            if (rsOpen) PaintRs(g, W, H, t);
        }

        void PaintMain(Graphics g, float W, float H, double t, float a)
        {
            Func<float, float> stag = d => Ui.Ease((float)((t - d) / 0.6)) * a;   // staggered appearance
            float cx = W / 2;

            // title
            float a1 = stag(0.05f), dy = (1 - a1) * X(14);
            using (var f = F(fTitle, 62, FontStyle.Bold)) DrawSpaced(g, "DIVINITY II", f, Ui.A(Ui.Text, a1), cx, X(92) + dy, X(10));
            float a2 = stag(0.18f);
            using (var p = new Pen(Ui.A(Ui.Accent, a2 * 0.8f), X(1))) { g.DrawLine(p, cx - X(150) * a2, X(184), cx - X(96), X(184)); g.DrawLine(p, cx + X(96), X(184), cx + X(150) * a2, X(184)); }
            using (var f = F(fBody, 14, FontStyle.Bold)) DrawSpaced(g, "DEVELOPER'S CUT", f, Ui.A(Ui.Accent, a2), cx, X(175) + (1 - a2) * X(10), X(5));
            float a3 = stag(0.28f);
            string mod = game != null && game.ModVersion != null ? game.ModVersion : "Divinity II Reborn";
            using (var f = F(fBody, 13.5f)) DrawText(g, mod + "  ·  новые навыки, ребаланс, геймпад, улучшенный интерфейс", f, Ui.A(Ui.Dim, a3), new RectangleF(0, X(206), W, X(24)), StringAlignment.Center);

            // buttons
            float a4 = stag(0.38f), a5 = stag(0.46f);
            PaintBtn(g, play, a4, (1 - a4) * X(16));
            PaintBtn(g, check, a5, (1 - a5) * X(16));
            PaintRsLink(g, a5, (1 - a5) * X(16));

            // footer
            float a6 = stag(0.55f);
            float fy = H - X(76);
            using (var p = new Pen(Ui.A(Ui.Line, a6))) g.DrawLine(p, X(40), fy, W - X(40), fy);

            // install status (left)
            Sev? st = quickRunning ? (Sev?)null : quick != null ? quick.Overall : (Sev?)null;
            Color dc = st.HasValue ? Ui.SevColor(st.Value) : Ui.Faint;
            float pulse = st.HasValue ? 1 : 0.5f + 0.5f * (float)Math.Sin(t * 6);
            using (var b = new SolidBrush(Ui.A(dc, a6 * pulse))) g.FillEllipse(b, X(40), fy + X(29), X(10), X(10));
            using (var b = new SolidBrush(Ui.A(dc, a6 * 0.18f))) g.FillEllipse(b, X(36), fy + X(25), X(18), X(18));
            string stText = game == null ? "Папка игры не найдена" : quickRunning ? "Проверка установки..." : st == Sev.Ok ? "Мод установлен правильно" : st == Sev.Warn ? "Мод установлен, есть предупреждения (" + quick.Count(Sev.Warn) + ")" : "Ошибки установки мода (" + (quick != null ? quick.Count(Sev.Error) : 0) + ") - нажмите «Проверить»";
            using (var f = F(fBody, 13)) DrawText(g, stText, f, Ui.A(Ui.Text, a6 * 0.9f), new RectangleF(X(60), fy + X(20), X(360), X(28)), StringAlignment.Near, StringAlignment.Center);

            // countdown (centre-right)
            if (cd == Cd.Running || (cd == Cd.Cancelled && cdFade > 0.01f) || cd == Cd.Fired) {
                double left = cd == Cd.Running ? Math.Max(0, CountdownSec - (Now - cdStart)) : 0;
                int n = (int)Math.Ceiling(left);
                float ca = a6 * (cd == Cd.Cancelled ? cdFade : 1);
                float bx = W - X(40) - X(250), by = fy + X(14);
                string label = cd == Cd.Cancelled ? "Автозапуск отменён" : cd == Cd.Fired ? "Запуск..." : "Автоматический запуск через";
                using (var f = F(fBody, 12.5f)) DrawText(g, label, f, Ui.A(Ui.Dim, ca), new RectangleF(bx, by, X(200), X(22)), StringAlignment.Near, StringAlignment.Center);
                if (cd == Cd.Running) {
                    float frac = (float)(left - Math.Floor(left));
                    float pop = 1 + 0.25f * Ui.Ease(frac > 0.85f ? (frac - 0.85f) / 0.15f : 0);
                    using (var f = F(fTitle, 30 * pop, FontStyle.Bold)) DrawText(g, n.ToString(), f, Ui.A(Ui.Accent, ca), new RectangleF(W - X(40) - X(50), fy + X(4), X(50), X(48)), StringAlignment.Far, StringAlignment.Center);
                }
                var bar = new RectangleF(bx, by + X(28), X(196), X(4));
                using (var b = new SolidBrush(Ui.A(Ui.Line, ca))) using (var pth = Ui.Round(bar, X(2))) g.FillPath(b, pth);
                float prog = cd == Cd.Running ? (float)(left / CountdownSec) : cd == Cd.Fired ? 0 : 0;
                if (prog > 0) using (var b = new SolidBrush(Ui.A(Ui.Accent, ca))) using (var pth = Ui.Round(new RectangleF(bar.X, bar.Y, Math.Max(X(4), bar.Width * prog), bar.Height), X(2))) g.FillPath(b, pth);
            } else if (cd == Cd.Blocked) {
                using (var f = F(fBody, 12.5f)) DrawText(g, "Автозапуск выключен: мод установлен с ошибками", f, Ui.A(Ui.Err, a6), new RectangleF(W - X(40) - X(380), fy + X(20), X(380), X(28)), StringAlignment.Far, StringAlignment.Center);
            } else if (statusLine == null) {
                using (var f = F(fBody, 12.5f)) DrawText(g, "Enter - играть   ·   Esc - выход", f, Ui.A(Ui.Faint, a6), new RectangleF(W - X(40) - X(300), fy + X(20), X(300), X(28)), StringAlignment.Far, StringAlignment.Center);
            }
            if (statusLine != null)
                using (var f = F(fBody, 12.5f)) DrawText(g, statusLine, f, Ui.A(statusColor, a6), new RectangleF(0, fy - X(34), W, X(24)), StringAlignment.Center, StringAlignment.Center);

            if (devBtn.Visible) using (var f = F(fBody, 11.5f, devBtn.Hover > 0.5f ? FontStyle.Underline : FontStyle.Regular))
                DrawText(g, "⚙  " + devBtn.Text, f, Ui.A(devBtn.Enabled ? Ui.Mix(Ui.Faint, Ui.Accent, devBtn.Hover) : Ui.Line, a6), devBtn.R, StringAlignment.Far, StringAlignment.Center);
            using (var f = F(fBody, 11)) DrawText(g, "Лаунчер 2.1  ·  Divinity2.exe" + (game != null && game.FromIni ? " (installation.ini)" : ""), f, Ui.A(Ui.Faint, a6), new RectangleF(X(40), H - X(26), W - X(80), X(18)), StringAlignment.Near, StringAlignment.Center);
        }

        void PaintBtn(Graphics g, Btn b, float a, float dy)
        {
            if (a <= 0.01f) return;
            var r = b.R; r.Offset(0, dy);
            float h = b.Hover, press = b.Down ? 1 : 0;
            if (press > 0) r.Inflate(-X(1.5f), -X(1.5f));
            float rad = X(b.Primary ? 10 : 8);
            using (var path = Ui.Round(r, rad)) {
                if (b.Primary) {
                    if (h > 0.01f) for (int i = 3; i >= 1; i--) using (var gp = Ui.Round(RectangleF.Inflate(r, X(4 * i), X(4 * i)), rad + X(4 * i))) using (var gb = new SolidBrush(Ui.A(Ui.Accent, a * h * 0.07f))) g.FillPath(gb, gp);
                    Color c1 = b.Enabled ? Ui.Mix(Ui.Accent, Ui.AccentHi, h) : Ui.Faint, c2 = b.Enabled ? Ui.Mix(Color.FromArgb(176, 122, 56), Ui.Accent, h) : Ui.Line;
                    using (var br = new LinearGradientBrush(r, Ui.A(c1, a), Ui.A(c2, a), 90f)) g.FillPath(br, path);
                    using (var f = F(fTitle, r.Height < X(60) ? 14 : 22, FontStyle.Bold)) DrawSpaced(g, b.Enabled ? b.Text : "ЗАПУСК...", f, Ui.A(Color.FromArgb(24, 18, 10), a), r.X + r.Width / 2, r.Y + (r.Height - f.Height) / 2 + X(1), r.Height < X(60) ? X(2) : X(5));
                } else {
                    using (var br = new SolidBrush(Ui.A(Ui.Mix(Ui.Panel, Ui.PanelHi, h), a * 0.92f))) g.FillPath(br, path);
                    using (var p = new Pen(Ui.A(Ui.Mix(Ui.Line, Ui.Accent, h * 0.8f), a), X(1))) g.DrawPath(p, path);
                    using (var f = F(fBody, 13.5f, FontStyle.Bold)) DrawSpaced(g, b.Text, f, Ui.A(b.Enabled ? Ui.Mix(Ui.Dim, Ui.Text, 0.6f + 0.4f * h) : Ui.Faint, a), r.X + r.Width / 2, r.Y + (r.Height - f.Height) / 2, X(1.5f));
                }
            }
        }

        void PaintCheck(Graphics g, float W, float H, double t)
        {
            float a = Ui.Ease((float)((t - checkOpenT) / 0.35)), dy = (1 - a) * X(20);
            using (var b = new SolidBrush(Ui.A(Color.FromArgb(200, 8, 8, 10), a))) g.FillRectangle(b, 0, X(34), W, H - X(34));
            var panel = new RectangleF(X(40), X(46) + dy, W - X(80), H - X(46) - X(110));
            using (var path = Ui.Round(panel, X(12))) {
                using (var b = new SolidBrush(Ui.A(Ui.Panel, a * 0.97f))) g.FillPath(b, path);
                using (var p = new Pen(Ui.A(Ui.Line, a))) g.DrawPath(p, path);
            }
            using (var f = F(fTitle, 22, FontStyle.Bold)) DrawText(g, "Проверка установки мода", f, Ui.A(Ui.Text, a), new RectangleF(panel.X + X(28), panel.Y + X(20), X(500), X(32)));
            using (var f = F(fBody, 12.5f)) DrawText(g, game != null ? game.Root : "папка игры не найдена", f, Ui.A(Ui.Faint, a), new RectangleF(panel.X + X(28), panel.Y + X(54), panel.Width - X(56), X(20)));

            if (checkRunning || full == null) {
                var bar = new RectangleF(panel.X + X(28), panel.Y + panel.Height / 2, panel.Width - X(56), X(6));
                using (var b = new SolidBrush(Ui.A(Ui.Line, a))) using (var pth = Ui.Round(bar, X(3))) g.FillPath(b, pth);
                using (var b = new SolidBrush(Ui.A(Ui.Accent, a))) using (var pth = Ui.Round(new RectangleF(bar.X, bar.Y, Math.Max(X(6), (float)(bar.Width * checkProgress)), bar.Height), X(3))) g.FillPath(b, pth);
                // spinner
                float sx = bar.X, sy = bar.Y - X(46);
                using (var p = new Pen(Ui.A(Ui.Accent, a), X(3)) { StartCap = LineCap.Round, EndCap = LineCap.Round }) g.DrawArc(p, sx, sy, X(22), X(22), (float)(t * 360 % 360), 260);
                using (var f = F(fBody, 14)) DrawText(g, "Проверяю: " + checkStep, f, Ui.A(Ui.Text, a), new RectangleF(sx + X(36), sy, bar.Width - X(36), X(24)), StringAlignment.Near, StringAlignment.Center);
                using (var f = F(fBody, 12)) DrawText(g, "Файлы только читаются - проверка ничего не меняет и не удаляет.", f, Ui.A(Ui.Faint, a), new RectangleF(bar.X, bar.Bottom + X(14), bar.Width, X(20)));
            } else {
                var ov = full.Overall;
                var badge = new RectangleF(panel.Right - X(28) - X(210), panel.Y + X(22), X(210), X(34));
                using (var pth = Ui.Round(badge, X(17))) using (var b = new SolidBrush(Ui.A(Ui.SevColor(ov), a * 0.16f))) g.FillPath(b, pth);
                using (var f = F(fBody, 13, FontStyle.Bold)) DrawText(g, Ui.SevName(ov), f, Ui.A(Ui.SevColor(ov), a), badge, StringAlignment.Center, StringAlignment.Center);
                string sum = ov == Sev.Ok ? "Мод установлен правильно." : ov == Sev.Warn ? "Мод работает, но есть необязательные проблемы." : "Отсутствуют необходимые файлы или компоненты.";
                sum += "   Ошибок: " + full.Count(Sev.Error) + "   Предупреждений: " + full.Count(Sev.Warn) + "   Проверено без ошибок: " + full.Count(Sev.Ok);
                using (var f = F(fBody, 13)) DrawText(g, sum, f, Ui.A(Ui.Dim, a), new RectangleF(panel.X + X(28), panel.Y + X(80), panel.Width - X(56), X(22)));

                var list = new RectangleF(panel.X + X(16), panel.Y + X(112), panel.Width - X(32), panel.Height - X(124));
                var state = g.Save();
                g.SetClip(list);
                float y = list.Y - scroll, startY = y;
                var items = full.Items.OrderByDescending(i => i.Severity == Sev.Error ? 3 : i.Severity == Sev.Warn ? 2 : i.Severity == Sev.Info ? 1 : 0).ToList();
                // problems get full cards; passed checks are compact rows
                using (var fT = F(fBody, 14, FontStyle.Bold)) using (var fP = F(fBody, 11.5f)) using (var fD = F(fBody, 12.5f))
                foreach (var it in items) {
                    bool problem = it.Severity == Sev.Error || it.Severity == Sev.Warn || it.Severity == Sev.Info;
                    float tw = list.Width - X(40);
                    float hDesc = problem && !string.IsNullOrEmpty(it.Description) ? g.MeasureString(it.Description, fD, (int)tw).Height : 0;
                    float hFix = problem && !string.IsNullOrEmpty(it.Fix) ? g.MeasureString("Как исправить: " + it.Fix, fD, (int)tw).Height : 0;
                    float hh = problem ? X(14) + X(22) + X(18) + hDesc + (hFix > 0 ? hFix + X(4) : 0) + X(12) : X(34);
                    var card = new RectangleF(list.X, y, list.Width, hh);
                    if (card.Bottom > list.Y && card.Y < list.Bottom) {
                        var col = Ui.SevColor(it.Severity);
                        using (var pth = Ui.Round(card, X(8))) using (var b = new SolidBrush(Ui.A(problem ? Ui.PanelHi : Ui.Panel, a))) g.FillPath(b, pth);
                        using (var b = new SolidBrush(Ui.A(col, a))) g.FillRectangle(b, card.X, card.Y + X(8), X(3), card.Height - X(16));
                        float tx = card.X + X(20), ty = card.Y + (problem ? X(12) : X(7));
                        string tag = Ui.SevName(it.Severity);
                        float tagW = g.MeasureString(tag, fP).Width + X(10);
                        using (var pth = Ui.Round(new RectangleF(card.Right - X(14) - tagW, ty + X(1), tagW, X(20)), X(10))) using (var b = new SolidBrush(Ui.A(col, a * 0.15f))) g.FillPath(b, pth);
                        DrawText(g, tag, fP, Ui.A(col, a), new RectangleF(card.Right - X(14) - tagW, ty + X(1), tagW, X(20)), StringAlignment.Center, StringAlignment.Center);
                        DrawText(g, it.Title, problem ? fT : fD, Ui.A(problem ? Ui.Text : Ui.Dim, a), new RectangleF(tx, ty, tw - tagW - X(10), X(22)));
                        if (problem) {
                            float yy = ty + X(23);
                            DrawText(g, it.Path ?? "", fP, Ui.A(Ui.Faint, a), new RectangleF(tx, yy, tw, X(18))); yy += X(18);
                            if (hDesc > 0) { DrawText(g, it.Description, fD, Ui.A(Ui.Dim, a), new RectangleF(tx, yy, tw, hDesc + 2), wrap: true); yy += hDesc; }
                            if (hFix > 0) { yy += X(4); DrawText(g, "Как исправить: " + it.Fix, fD, Ui.A(Ui.Mix(Ui.Text, Ui.Accent, 0.35f), a), new RectangleF(tx, yy, tw, hFix + 2), wrap: true); }
                        }
                    }
                    y += hh + X(8);
                }
                contentH = (y - startY) - list.Height;
                g.Restore(state);
                if (contentH > 0) {   // scrollbar
                    float th = list.Height * list.Height / (list.Height + contentH), tp = list.Y + (list.Height - th) * (scroll / contentH);
                    using (var b = new SolidBrush(Ui.A(Ui.Line, a))) using (var pth = Ui.Round(new RectangleF(list.Right + X(5), tp, X(4), th), X(2))) g.FillPath(b, pth);
                }
            }
            closeCheck.R.Y = H - X(40) - X(46); copyReport.R.Y = closeCheck.R.Y;
            PaintBtn(g, copyReport, a, dy);
            PaintBtn(g, closeCheck, a, dy);
            using (var f = F(fBody, 12)) DrawText(g, "Колесо мыши - прокрутка   ·   Esc - закрыть", f, Ui.A(Ui.Faint, a), new RectangleF(X(40), closeCheck.R.Y, X(400), X(46)), StringAlignment.Near, StringAlignment.Center);
        }
    }

    static class Program
    {
        [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

        [STAThread]
        static int Main(string[] args)
        {
            string root = null, report = null, dryLog = null; bool dry = false;
            for (int i = 0; i < args.Length; i++) {
                if (args[i] == "--game" && i + 1 < args.Length) root = args[++i];
                else if (args[i] == "--report" && i + 1 < args.Length) report = args[++i];
                else if (args[i] == "--dry-run" && i + 1 < args.Length) { dry = true; dryLog = args[++i]; }
                // test hooks for the ReShade helper (write a line to <out> and exit)
                else if (args[i] == "--dp-install" && i + 1 < args.Length) {   // test hook: installs Div2Patch into --game (test copy)
                    string outp = args[i + 1];
                    try { Div2PatchHelper.Install(GameInstall.Find(root), m => File.AppendAllText(outp, m + Environment.NewLine)); File.AppendAllText(outp, "OK" + Environment.NewLine); }
                    catch (Exception e) { File.AppendAllText(outp, "ERR " + e.Message + Environment.NewLine); }
                    return 0;
                }
                else if (args[i] == "--rs-verify" && i + 2 < args.Length) { File.WriteAllText(args[i + 2], ReShadeHelper.Probe(args[i + 1])); return 0; }
                else if (args[i] == "--rs-finish" && i + 1 < args.Length) {   // effects + presets only, on --game (test copy)
                    string outp = args[i + 1];
                    try { ReShadeHelper.FinishSetup(GameInstall.Find(root), 3, m => File.AppendAllText(outp, m + Environment.NewLine)); File.AppendAllText(outp, "OK" + Environment.NewLine); }
                    catch (Exception e) { File.AppendAllText(outp, "ERR " + e.Message + Environment.NewLine); }
                    return 0;
                }
                else if (args[i] == "--rs-detect" && i + 1 < args.Length) {
                    var s = ReShadeHelper.Detect(GameInstall.Find(root));
                    File.WriteAllText(args[i + 1], "layer=" + s.Layer + " version=" + s.Version + " app=" + s.App + " ini=" + s.IniExists + " preset=" + s.PresetFile + " effects=" + s.Effects + " missing=" + string.Join("|", s.MissingFx) + " presets=" + s.Presets + " conflict=" + s.ConflictFile + " ready=" + s.Ready);
                    return 0;
                }
            }
            var g = GameInstall.Find(root);
            if (report != null) {   // headless check for tests: writes the report, exit code 0/1/2 = ok/warn/error
                var r = ModChecker.Run(g, false, CancellationToken.None, null);
                File.WriteAllText(report, ModChecker.Report(g, r), new UTF8Encoding(true));
                return r.Overall == Sev.Ok ? 0 : r.Overall == Sev.Warn ? 1 : 2;
            }
            bool created;
            using (var m = new Mutex(true, "Local\\Div2RebornLauncher", out created)) {
                if (!created) return 0;     // already open
                try { SetProcessDPIAware(); } catch { }
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.ThreadException += (s, e) => MessageBox.Show(e.Exception.Message, "Divinity II Reborn - ошибка лаунчера", MessageBoxButtons.OK, MessageBoxIcon.Error);
                Application.Run(new LauncherForm(g, dry, dryLog));
            }
            return 0;
        }
    }
}
