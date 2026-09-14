using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using Microsoft.Win32;
using dnlib.DotNet;

namespace DymoPatch
{
    public static class Program
    {
        // Nominal (full-roll) label counts per SKU, used by insert/reset. Unknown SKU -> 220.
        public static readonly Dictionary<string, int> Nominal = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase)
        {
            { "S0904980", 220 },   // 104x159mm biggest 5XL
            { "S0722430", 220 },   // 54x101mm
            { "30387",    100 },   // Internet Postage, biggest 550 (2-1/4 x 10 in)
            { "30256",    300 },   // 59x102mm biggest 550 shipping
            { "11351",   1500 },   // 11x54
            { "99017",    220 },   // 12x50
            { "S0722530",1000 },   // 13x25
            { "11354",   1000 },   // 57x32
            { "S0947420",1150 },   // 102x59
            { "S0947410",2100 },   // 89x28 record
        };

        static string FlagPath => Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), Patcher.FlagFileName);

        public static int Main(string[] args)
        {
            CleanupStaleOldFiles();
            if (args.Length == 0) { PatchAll(); return 0; }
            try
            {
                switch (args[0].ToLowerInvariant())
                {
                    case "patch": PatchAll(); break;
                    case "restore": RestoreAll(); break;
                    case "set": SetFlag(args[1], int.Parse(args[2])); break;
                    case "insert": InsertRoll(args.Length > 1 ? args[1] : "S0904980"); break;
                    case "reset": ResetCounter(); break;
                    case "off": OffFlag(); break;
                    case "show": Show(); break;
                    case "list": ListSkus(); break;
                    case "watch": Watch(args.Length > 1 ? int.Parse(args[1]) : 500); break;
                    case "autostart": Autostart(args.Length > 1 ? args[1].ToLowerInvariant() : "status"); break;
                    case "updates": UpdatesBlock(args.Length > 1 ? args[1].ToLowerInvariant() : "status"); break;
                    case "--tray": case "tray": TrayApp.Run(); return 0;
                    default:
                        Console.WriteLine("usage: dymo [patch|restore|set <SKU> <count>|insert [SKU]|reset|off|show|list|watch [ms]|autostart [on|off|status]|updates [on|off|status]|--tray]");
                        return 1;
                }
            }
            catch (Exception ex) { Console.WriteLine("error: " + ex.ToString()); return 2; }
            return 0;
        }

        // ------------------------------------------------------------------ discovery
        public static List<string> FindDlls()
        {
            var root = Path.Combine(Path.GetTempPath(), ".net");
            var list = new List<string>();
            if (!Directory.Exists(root)) return list;
            foreach (var f in Directory.EnumerateFiles(root, "DYMO.LabelAPI.dll", SearchOption.AllDirectories))
                if (!f.EndsWith(".orig") && !f.EndsWith(".bak"))
                    list.Add(f);
            return list;
        }

        static void CleanupStaleOldFiles()
        {
            try
            {
                foreach (var d in FindDlls())
                    foreach (var old in Directory.GetFiles(Path.GetDirectoryName(d), Path.GetFileName(d) + ".old_*"))
                        try { File.Delete(old); } catch { }
            }
            catch { }
        }

        // ------------------------------------------------------------------ patch / restore
        public static void PatchAll()
        {
            var dlls = FindDlls();
            if (dlls.Count == 0) { Console.WriteLine("[!] no DYMO.LabelAPI.dll found under %TEMP%\\.net"); return; }
            string activeSku = Patcher.DefaultSku;
            if (ReadFlag(out string flagSku, out _)) activeSku = flagSku;
            var skus = new List<string> { activeSku };

            foreach (var live in dlls)
            {
                try { PatchDll(live, skus); }
                catch (Exception ex) { Console.WriteLine("[!] " + live + " :\n" + ex); }
            }
            Console.WriteLine("Done. Restart DYMO Connect to apply the patch.");
            Console.WriteLine("Roll switch:  dymo set <SKU> <count>   |   dymo off   (flag: " + FlagPath + ")");
        }

        public static void PatchDll(string live, List<string> skus)
        {
            string orig = live + ".orig";
            if (!File.Exists(orig)) File.Copy(live, orig); // first time: keep pristine reference

            string dir = Path.GetDirectoryName(live);
            string workIn = Path.Combine(dir, "." + Path.GetFileName(live) + ".in");
            string workOut = Path.Combine(dir, "." + Path.GetFileName(live) + ".out");

            File.Copy(orig, workIn, true);
            var data = Patcher.CatalogPatch(File.ReadAllBytes(workIn), skus);   // A: Region->Global (keep real paper)
            foreach (var sku in skus)                                            // D: un-exclude the SKU's real paper
            {
                string paper = Patcher.GetSkuPaper(data, sku);
                if (!string.IsNullOrEmpty(paper)) data = Patcher.ExcludedPapersBlank(data, paper);
            }
            File.WriteAllBytes(workIn, data);

            var mod = ModuleDefMD.Load(workIn);
            Patcher.IlInject(mod);          // B: IL injection
            Patcher.PatchValidateResult(mod); // C: ValidateResult flip
            mod.Write(workOut);
            mod.Dispose();

            SwapFile(live, workOut);
            try { File.Delete(workIn); } catch { }
            try { File.Delete(workOut); } catch { }
            Console.WriteLine("[patched] " + live);
        }

        public static void RestoreAll()
        {
            foreach (var live in FindDlls())
            {
                string orig = live + ".orig";
                if (!File.Exists(orig)) { Console.WriteLine("[skip] no .orig for " + live); continue; }
                SwapFile(live, orig);
                Console.WriteLine("[restored] " + live);
            }
        }

        static void SwapFile(string live, string src)
        {
            string old = live + ".old_" + DateTime.Now.Ticks;
            if (File.Exists(live)) File.Move(live, old);
            File.Copy(src, live);
            try { File.Delete(old); } catch { /* may be mapped; cleaned next run */ }
        }

        // ------------------------------------------------------------------ flag file
        public static bool ReadFlag(out string sku, out int count)
        {
            sku = null; count = 0;
            try
            {
                if (!File.Exists(FlagPath)) return false;
                var parts = File.ReadAllText(FlagPath).Trim().Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length >= 2 && int.TryParse(parts[1], out count)) { sku = parts[0]; return true; }
            }
            catch { }
            return false;
        }

        public static void SetFlag(string sku, int count)
        {
            File.WriteAllText(FlagPath, sku + " " + count);
            Console.WriteLine("[ok] flag set -> " + sku + " / " + count + "  (" + FlagPath + ")");
            RepatchForActive();
        }
        public static void InsertRoll(string sku)
        {
            int n = Nominal.TryGetValue(sku, out var c) ? c : 220;
            SetFlag(sku, n);
        }
        public static void ResetCounter()
        {
            if (!ReadFlag(out string sku, out _)) { Console.WriteLine("[!] no active roll (flag file absent)"); return; }
            int n = Nominal.TryGetValue(sku, out var c) ? c : 220;
            SetFlag(sku, n);
        }
        public static void OffFlag()
        {
            if (File.Exists(FlagPath)) File.Delete(FlagPath);
            Console.WriteLine("[ok] flag cleared -> authentic pass-through");
        }

        // Re-patch so the catalog/exclusion are ready for the newly-active SKU. The IL injection
        // reads the flag file at runtime, so the count takes effect on the next poll; the catalog
        // (Region + un-excluded paper) is updated here.
        static void RepatchForActive()
        {
            try { PatchAll(); } catch (Exception ex) { Console.WriteLine("[!] re-patch: " + ex.Message); }
        }

        // ------------------------------------------------------------------ show / list
        public static void Show()
        {
            var dlls = FindDlls();
            Console.WriteLine("DLLs under %TEMP%\\.net:");
            foreach (var d in dlls)
                Console.WriteLine("  " + (Patcher.IsPatched(File.ReadAllBytes(d)) ? "[patched]  " : "[original] ") + d);
            if (dlls.Count == 0) Console.WriteLine("  (none)");
            if (ReadFlag(out string sku, out int count))
                Console.WriteLine("flag: ON -> " + sku + " / " + count);
            else
                Console.WriteLine("flag: OFF (authentic pass-through)");
        }

        public static void ListSkus()
        {
            Console.WriteLine("Known rolls (SKU -> nominal count):");
            foreach (var kv in Nominal.OrderBy(k => k.Key))
                Console.WriteLine("  " + kv.Key + " = " + kv.Value);
        }

        // ------------------------------------------------------------------ watch (persistence)
        public static void Watch(int intervalMs)
        {
            Console.WriteLine("[watch] re-applying patch every " + intervalMs + " ms. Ctrl+C to stop.");
            while (true)
            {
                try
                {
                    string activeSku = Patcher.DefaultSku;
                    if (ReadFlag(out string flagSku, out _)) activeSku = flagSku;
                    var skus = new List<string> { activeSku };

                    foreach (var live in FindDlls())
                    {
                        if (Patcher.IsPatched(File.ReadAllBytes(live))) continue; // already good
                        if (!File.Exists(live + ".orig")) File.Copy(live, live + ".orig");
                        PatchDll(live, skus);
                        Console.WriteLine("[watch] re-patched " + live);
                    }
                }
                catch { /* transient (file locked mid-extract); retry next tick */ }
                System.Threading.Thread.Sleep(intervalMs);
            }
        }

        // ------------------------------------------------------------------ autostart
        public static void Autostart(string mode)
        {
            const string keyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
            const string valueName = "dymo_roll_patch";
            string exe = Process.GetCurrentProcess().MainModule.FileName;
            using (var k = Registry.CurrentUser.OpenSubKey(keyPath, true))
            {
                if (k == null) { Console.WriteLine("[!] Run key not found"); return; }
                switch (mode)
                {
                    case "on":
                        k.SetValue(valueName, "\"" + exe + "\" --tray");
                        Console.WriteLine("[ok] autostart ON -> " + exe + " --tray");
                        break;
                    case "off":
                        k.DeleteValue(valueName, false);
                        Console.WriteLine(k.GetValue(valueName) == null ? "[ok] autostart removed" : "[i] autostart not present");
                        break;
                    default:
                        var v = k.GetValue(valueName);
                        Console.WriteLine("autostart: " + (v == null ? "OFF" : "ON -> " + v));
                        break;
                }
            }
        }

        // ------------------------------------------------------------------ update block (hosts)
        static readonly string[] UpdateDomains = { "dymoreleasecontent.blob.core.windows.net", "printdymolabel.azurewebsites.net" };
        const string HostsMarker = "# DYMO-UPDATE-BLOCK";
        public static void UpdatesBlock(string mode)
        {
            string hosts = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "drivers", "etc", "hosts");
            switch (mode)
            {
                case "on":
                    var lines = File.ReadAllLines(hosts).ToList();
                    if (lines.Any(l => l.Contains(HostsMarker))) { Console.WriteLine("[i] already blocked"); return; }
                    var block = new List<string> { HostsMarker };
                    foreach (var d in UpdateDomains) block.Add("127.0.0.1 " + d);
                    lines.AddRange(block);
                    WriteHosts(hosts, lines);
                    Console.WriteLine("[ok] DYMO update CDNs blocked via hosts file");
                    break;
                case "off":
                    var cur = File.ReadAllLines(hosts).Where(l => !l.Contains(HostsMarker) && !UpdateDomains.Any(d => l.TrimStart().StartsWith("127.0.0.1 " + d))).ToList();
                    WriteHosts(hosts, cur);
                    Console.WriteLine("[ok] DYMO update block removed");
                    break;
                default:
                    var has = File.Exists(hosts) && File.ReadAllLines(hosts).Any(l => l.Contains(HostsMarker));
                    Console.WriteLine("update block: " + (has ? "ON" : "OFF"));
                    break;
            }
        }
        static void WriteHosts(string hosts, List<string> lines)
        {
            var attrs = File.GetAttributes(hosts);
            try { File.SetAttributes(hosts, attrs & ~System.IO.FileAttributes.ReadOnly); } catch { }
            File.WriteAllLines(hosts, lines, new UTF8Encoding(false));
            try { File.SetAttributes(hosts, attrs); } catch { }
        }
    }
}
