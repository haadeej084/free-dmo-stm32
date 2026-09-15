using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using dnlib.DotNet;
using dnlib.DotNet.Emit;

namespace DymoPatch
{
    /// Core patching, ported from the proven Python patcher:
    ///   A  catalog: SKU Region -> Global (keep the real paper).
    ///   B  IL inject: in UpdatePrinterStatus::MoveNext, after set_SkuNumber, if the
    ///      detected SKU is empty, force SkuNumber/count/eRollValidity from the flag file
    ///      (default S0904980/220 = the big 104x159mm label).
    ///   C  ValidateResult: flip the non-null return-false sites to true so a printer in an
    ///      Error/Counterfeit state still passes validation (needed by the REV.E firmware).
    ///   D  ExcludedPapers: blank the real paper's <PaperName> entries so it resolves on the
    ///      550 driver (otherwise S0904980 would not resolve -> "Leeg").
    /// All idempotent: always rebuild from the .orig copy.
    public static class Patcher
    {
        // Flag file read at runtime (per poll) to choose the spoofed roll. Absent -> defaults.
        public const string FlagFileName = "dymo_roll.flag";
        public const string DefaultSku = "S0904980";   // 104x159mm, biggest 5XL roll
        public const int    DefaultCount = 220;

        // The real paper of the default SKU (from SKUs.xml). Kept as-is; Patch D un-excludes it.
        public const string BigPaper = "1933086 LW DURABLE 104x159mm";

        // ------------------------------------------------------------------ catalog (A)
        // Set Region="Global" on each target SKU tag, KEEP its real PaperName. Length-preserving.
        public static byte[] CatalogPatch(byte[] dataIn, IEnumerable<string> skus)
        {
            var data = new List<byte>(dataIn);
            int s = IndexOf(data, "<SKUs>");
            if (s < 0) throw new Exception("<SKUs> not found in DLL");
            int e = IndexOf(data, "</SKUs>", s);
            if (e < 0) throw new Exception("</SKUs> not found in DLL");
            int endSkuTag = e + Encoding.ASCII.GetByteCount("</SKUs>");
            int origLen = endSkuTag - s;

            foreach (var sku in skus)
            {
                byte[] skuAttr = Encoding.ASCII.GetBytes("SKU=\"" + sku + "\"");
                int hit = IndexOf(data, skuAttr, s);
                if (hit < 0) continue;
                int tagBegin = LastIndexOf(data, "<DieCutSKU", 0, hit);
                if (tagBegin < 0 || tagBegin < s) continue;
                int tagEnd = IndexOfByte(data, (byte)'>', hit) + 1;

                var tag = new List<byte>(data.GetRange(tagBegin, tagEnd - tagBegin));
                tag = ReplaceAttr(tag, "Region", "Global");   // keep real PaperName
                data.RemoveRange(tagBegin, tagEnd - tagBegin);
                data.InsertRange(tagBegin, tag);
            }

            // restore total length of the <SKUs>...</SKUs> region (whitespace pad/trim)
            int e2 = IndexOf(data, "</SKUs>", s);
            int newLen = (e2 + Encoding.ASCII.GetByteCount("</SKUs>")) - s;
            int delta = origLen - newLen;
            if (delta > 0)
                for (int i = 0; i < delta; i++) data.Insert(e2, (byte)' ');
            else if (delta < 0)
            {
                int need = -delta, p = e2 - 1;
                while (need > 0 && p > s)
                {
                    byte b = data[p];
                    if (b == (byte)' ' || b == (byte)'\t' || b == (byte)'\r' || b == (byte)'\n') { data.RemoveAt(p); need--; }
                    p--;
                }
                if (need > 0) throw new Exception("could not trim enough whitespace to preserve length");
            }
            return data.ToArray();
        }

        // ------------------------------------------------------------------ ExcludedPapers (D)
        // Blank every <PaperName>{paper}</PaperName> exclusion entry so the paper is no longer
        // rejected by any driver. Length-preserving (same byte count). Returns the modified bytes.
        public static byte[] ExcludedPapersBlank(byte[] dataIn, string paper)
        {
            var data = new List<byte>(dataIn);
            byte[] elem  = Encoding.ASCII.GetBytes("<PaperName>" + paper + "</PaperName>");
            byte[] blank = Encoding.ASCII.GetBytes("<PaperName>" + new string(' ', paper.Length) + "</PaperName>");
            int idx = 0;
            while (true)
            {
                int i = IndexOf(data, elem, idx);
                if (i < 0) break;
                data.RemoveRange(i, elem.Length);
                data.InsertRange(i, blank);
                idx = i + elem.Length;
            }
            return data.ToArray();
        }

        // ------------------------------------------------------------------ IL inject (B)
        // Dynamic anchors (no hardcoded indices). The original set_SkuNumber is the callvirt fed
        // directly by get_InsertedSKU. Inject a straight-line block after it that, when the
        // detected SKU is empty, forces SkuNumber/count/eRollValidity from the flag file.
        // Every branch target is reached with an EMPTY stack (no max-stack mismatch).
        public static void IlInject(ModuleDefMD mod)
        {
            var t = FindType(mod, "LabelWriterRollDetectionPrinterCommunication");
            IMethod setSku = null, getSku = null, setRem = null;
            CilBody body = null;
            int insertAt = -1;
            Local locPrinter = null, locValidity = null;

            for (int i = 0; i < t.NestedTypes.Count && body == null; i++)
            {
                var nt = t.NestedTypes[i];
                string ntName = nt.Name;
                if (ntName == null || !ntName.Contains("UpdatePrinterStatus")) continue;
                foreach (var m in nt.Methods)
                {
                    if (m.Name.ToString() != "MoveNext" || !m.HasBody) continue;
                    var b = m.Body as CilBody;
                    if (b == null) continue;
                    var ins2 = b.Instructions;

                    // already patched? (marker ldstr present anywhere in the body)
                    bool already = false;
                    for (int k = 0; k < ins2.Count; k++)
                        if (ins2[k].OpCode.Code == Code.Ldstr && ins2[k].Operand is string st && st == FlagFileName)
                        { already = true; break; }
                    if (already) { body = b; break; } // nothing to do

                    int idxSetSku = -1;
                    for (int k = 1; k < ins2.Count; k++)
                    {
                        var cur = ins2[k];
                        var prev = ins2[k - 1];
                        if (cur.OpCode.Code == Code.Callvirt && cur.Operand is IMethod cmi && cmi.ToString().Contains("set_SkuNumber") &&
                            prev.OpCode.Code == Code.Callvirt && prev.Operand is IMethod pmi && pmi.ToString().Contains("get_InsertedSKU"))
                        { idxSetSku = k; break; }
                    }
                    if (idxSetSku < 0) continue;

                    int idxSetRem = -1;
                    for (int k = idxSetSku; k >= 0; k--)
                        if (ins2[k].Operand is IMethod rm && rm.ToString().Contains("set_LabelsRemaining")) { idxSetRem = k; break; }
                    int idxGetSku = -1;
                    for (int k = idxSetSku + 1; k < ins2.Count; k++)
                        if (ins2[k].Operand is IMethod gs && gs.ToString().Contains("get_SkuNumber")) { idxGetSku = k; break; }
                    if (idxSetRem < 0 || idxGetSku < 0) continue;

                    setSku = (IMethod)ins2[idxSetSku].Operand;
                    setRem = (IMethod)ins2[idxSetRem].Operand;
                    getSku = (IMethod)ins2[idxGetSku].Operand;
                    body = b;
                    insertAt = idxSetSku + 1;

                    for (int v = 0; v < b.Variables.Count; v++)
                    {
                        string tn = b.Variables[v].Type.ToString();
                        if (locPrinter == null && tn.Contains("LabelWriterRollDetectionPrinterCommunication")) locPrinter = b.Variables[v];
                        if (locValidity == null && tn.Contains("ERollValidity")) locValidity = b.Variables[v];
                    }
                    break;
                }
            }
            if (body == null) throw new Exception("UpdatePrinterStatus::MoveNext anchors not found");
            if (insertAt < 0) return; // already-patched method: leave as-is

            if (locPrinter == null) locPrinter = new Local(mod.CorLibTypes.Object);
            if (locValidity == null) throw new Exception("ERollValidity local not found in MoveNext");

            var ins = body.Instructions;
            Instruction cont = ins[insertAt]; // original instr right after set_SkuNumber (fall-through target)
            var ct = mod.CorLibTypes;

            // New locals for the flag-file read/parse (appended after the originals).
            var locPath    = new Local(ct.String);  body.Variables.Add(locPath);
            var locContent = new Local(ct.String);  body.Variables.Add(locContent);
            var locIdx     = new Local(ct.Int32);   body.Variables.Add(locIdx);
            var locSku     = new Local(ct.String);  body.Variables.Add(locSku);
            var locCnt     = new Local(ct.Int32);   body.Variables.Add(locCnt);

            // mscorlib method refs, imported fresh (robust across builds).
            var mGetFolderPath = mod.Import(typeof(Environment).GetMethod("GetFolderPath", new[] { typeof(Environment.SpecialFolder) }));
            var mPathCombine   = mod.Import(typeof(Path).GetMethod("Combine", new[] { typeof(string), typeof(string) }));
            var mFileExists    = mod.Import(typeof(File).GetMethod("Exists", new[] { typeof(string) }));
            var mReadAllText   = mod.Import(typeof(File).GetMethod("ReadAllText", new[] { typeof(string) }));
            var mTrim          = mod.Import(typeof(string).GetMethod("Trim", Type.EmptyTypes));
            var mIndexOf       = mod.Import(typeof(string).GetMethod("IndexOf", new[] { typeof(char) }));
            var mSub2          = mod.Import(typeof(string).GetMethod("Substring", new[] { typeof(int), typeof(int) }));
            var mSub1          = mod.Import(typeof(string).GetMethod("Substring", new[] { typeof(int) }));
            var mTryParse      = mod.Import(typeof(int).GetMethod("TryParse", new[] { typeof(string), typeof(int).MakeByRefType() }));
            var mIsNullOrEmpty = mod.Import(typeof(string).GetMethod("IsNullOrEmpty", new[] { typeof(string) }));

            int specialFolder = (int)Environment.SpecialFolder.LocalApplicationData;

            var L = new List<Instruction>();
            // path = Path.Combine(GetFolderPath(LocalApplicationData), flag)
            L.Add(Instruction.Create(OpCodes.Ldc_I4, specialFolder));
            L.Add(Instruction.Create(OpCodes.Call, mGetFolderPath));
            L.Add(Instruction.Create(OpCodes.Ldstr, FlagFileName));
            L.Add(Instruction.Create(OpCodes.Call, mPathCombine));
            L.Add(Instruction.Create(OpCodes.Stloc, locPath));
            // if !File.Exists(path) -> defaults
            L.Add(Instruction.Create(OpCodes.Ldloc, locPath));
            L.Add(Instruction.Create(OpCodes.Call, mFileExists));
            Instruction brHasFlag = Instruction.Create(OpCodes.Brtrue, (Instruction)null);
            L.Add(brHasFlag);
            // --- default branch: sku/count = baked-in defaults ---
            L.Add(Instruction.Create(OpCodes.Ldstr, DefaultSku));
            L.Add(Instruction.Create(OpCodes.Stloc, locSku));
            L.Add(Instruction.Create(OpCodes.Ldc_I4, DefaultCount));
            L.Add(Instruction.Create(OpCodes.Stloc, locCnt));
            Instruction brGuard = Instruction.Create(OpCodes.Br, (Instruction)null);
            L.Add(brGuard);
            // --- flag branch: parse "SKU count" from the file ---
            Instruction hasFlagLbl = L[L.Count - 1]; // placeholder; set below to first instr of this block
            L.Add(Instruction.Create(OpCodes.Ldloc, locPath));
            L.Add(Instruction.Create(OpCodes.Call, mReadAllText));
            L.Add(Instruction.Create(OpCodes.Call, mTrim));
            L.Add(Instruction.Create(OpCodes.Stloc, locContent));
            L.Add(Instruction.Create(OpCodes.Ldloc, locContent));
            L.Add(Instruction.Create(OpCodes.Ldc_I4, (int)' '));
            L.Add(Instruction.Create(OpCodes.Conv_I2));
            L.Add(Instruction.Create(OpCodes.Callvirt, mIndexOf));
            L.Add(Instruction.Create(OpCodes.Stloc, locIdx));
            // if idx < 1 -> malformed -> defaults
            L.Add(Instruction.Create(OpCodes.Ldloc, locIdx));
            L.Add(Instruction.Create(OpCodes.Ldc_I4_1));
            L.Add(Instruction.Create(OpCodes.Clt));
            Instruction brMalformed = Instruction.Create(OpCodes.Brtrue, (Instruction)null);
            L.Add(brMalformed);
            // sku = content.Substring(0, idx)
            L.Add(Instruction.Create(OpCodes.Ldloc, locContent));
            L.Add(Instruction.Create(OpCodes.Ldc_I4_0));
            L.Add(Instruction.Create(OpCodes.Ldloc, locIdx));
            L.Add(Instruction.Create(OpCodes.Call, mSub2));
            L.Add(Instruction.Create(OpCodes.Stloc, locSku));
            // cnt = TryParse(content.Substring(idx+1)) ? parsed : DefaultCount
            L.Add(Instruction.Create(OpCodes.Ldloc, locContent));
            L.Add(Instruction.Create(OpCodes.Ldloc, locIdx));
            L.Add(Instruction.Create(OpCodes.Ldc_I4_1));
            L.Add(Instruction.Create(OpCodes.Add));
            L.Add(Instruction.Create(OpCodes.Call, mSub1));
            L.Add(Instruction.Create(OpCodes.Ldloca, locCnt));
            L.Add(Instruction.Create(OpCodes.Call, mTryParse));      // leaves [success]
            Instruction brHaveCnt = Instruction.Create(OpCodes.Brtrue, (Instruction)null);
            L.Add(brHaveCnt);                                        // success: locCnt set -> guard (empty stack)
            L.Add(Instruction.Create(OpCodes.Ldc_I4, DefaultCount)); // fail: load default
            L.Add(Instruction.Create(OpCodes.Stloc, locCnt));        // store (empty stack)
            // --- guard: apply only when the detected SKU is empty ---
            Instruction guardLbl = L[L.Count - 2]; // first instr of the guard block (Ldloc printer below)
            L.Add(Instruction.Create(OpCodes.Ldloc, locPrinter));
            L.Add(Instruction.Create(OpCodes.Callvirt, getSku));
            L.Add(Instruction.Create(OpCodes.Call, mIsNullOrEmpty));
            L.Add(Instruction.Create(OpCodes.Brfalse, cont));        // not empty -> skip (empty stack at cont)
            // apply: force sku/count/validity
            L.Add(Instruction.Create(OpCodes.Ldloc, locPrinter));
            L.Add(Instruction.Create(OpCodes.Ldloc, locSku));
            L.Add(Instruction.Create(OpCodes.Callvirt, setSku));     // []
            L.Add(Instruction.Create(OpCodes.Ldc_I4_0));             // eRollValidity = Valid
            L.Add(Instruction.Create(OpCodes.Stloc, locValidity));   // []
            L.Add(Instruction.Create(OpCodes.Ldloc, locPrinter));
            L.Add(Instruction.Create(OpCodes.Ldloc, locCnt));
            L.Add(Instruction.Create(OpCodes.Callvirt, setRem));     // [] -> falls through to cont (empty stack)

            // wire forward branches (all targets reached with an empty stack):
            brHasFlag.Operand  = hasFlagLbl;   // File.Exists true  -> parse block
            brGuard.Operand    = guardLbl;     // defaults done     -> guard
            brMalformed.Operand= guardLbl;     // malformed         -> guard
            brHaveCnt.Operand  = guardLbl;     // TryParse success  -> guard

            for (int i = 0; i < L.Count; i++)
                body.Instructions.Insert(insertAt + i, L[i]);

            body.SimplifyBranches();

            if (Environment.GetEnvironmentVariable("DYMO_DUMPIL") == "1")
            {
                var insd = body.Instructions;
                System.Console.WriteLine("=== DUMP MoveNext: MaxStack=" + body.MaxStack + " instrs=" + insd.Count + " ===");
                for (int i = 0; i < insd.Count; i++)
                {
                    var it = insd[i];
                    string on = it.OpCode.Name;
                    if (on.StartsWith("br") || on == "leave" || on == "ret")
                        System.Console.WriteLine("  " + i + " " + on + " -> " + (it.Operand is Instruction ti ? insd.IndexOf(ti) : "?"));
                }
            }
        }

        // ------------------------------------------------------------------ ValidateResult (C)
        // Flip the non-null return-false sites to true; keep the first (null guard).
        public static void PatchValidateResult(ModuleDefMD mod)
        {
            var t = FindType(mod, "LabelWriterRollDetectionPrinterCommunication");
            foreach (var m in t.Methods)
            {
                if (m.Name.ToString() != "ValidateResult" || !m.HasBody) continue;
                var b = m.Body as CilBody;
                if (b == null) continue;
                var ins = b.Instructions;
                var sites = new List<int>();
                for (int i = 0; i + 1 < ins.Count; i++)
                    if (ins[i].OpCode.Name == "ldc.i4.0" && ins[i + 1].OpCode.Name == "ret")
                        sites.Add(i);
                if (sites.Count >= 2)
                    for (int i = 1; i < sites.Count; i++)
                        ins[sites[i]].OpCode = OpCodes.Ldc_I4_1;
                return;
            }
            throw new Exception("ValidateResult not found");
        }

        // ------------------------------------------------------------------ helpers
        public static TypeDef FindType(ModuleDefMD mod, string name)
        {
            for (int i = 0; i < mod.Types.Count; i++)
            {
                string tn = mod.Types[i].Name;
                if (tn != null && tn == name) return mod.Types[i];
            }
            throw new Exception("type " + name + " not found");
        }

        public static bool IsPatched(byte[] data)
        {
            byte[] m = Encoding.Unicode.GetBytes(FlagFileName);
            return IndexOf(data, m) >= 0;
        }

        // Read the PaperName attribute of a SKU tag from the catalog bytes (null if not found).
        public static string GetSkuPaper(byte[] data, string sku)
        {
            int s = IndexOf(data, "<SKUs>");
            int e = IndexOf(data, "</SKUs>", s);
            if (s < 0 || e < 0) return null;
            string region = Encoding.ASCII.GetString(data, s, e - s + Encoding.ASCII.GetByteCount("</SKUs>"));
            var m = System.Text.RegularExpressions.Regex.Match(
                region, @"<DieCutSKU[^>]*SKU=""" + System.Text.RegularExpressions.Regex.Escape(sku) + @"""[^>]*>");
            if (!m.Success) return null;
            var pm = System.Text.RegularExpressions.Regex.Match(m.Value, @"PaperName=""([^""]*)""");
            return pm.Success ? pm.Groups[1].Value : null;
        }

        public static int IndexOf(byte[] data, string ascii, int start = 0) => IndexOf(data, Encoding.ASCII.GetBytes(ascii), start);
        public static int IndexOf(byte[] data, byte[] pat, int start = 0)
        {
            if (pat.Length == 0) return -1;
            for (int i = start; i + pat.Length <= data.Length; i++)
            {
                bool ok = true;
                for (int j = 0; j < pat.Length; j++) if (data[i + j] != pat[j]) { ok = false; break; }
                if (ok) return i;
            }
            return -1;
        }
        public static int IndexOf(List<byte> data, string ascii, int start = 0) => IndexOf(data, Encoding.ASCII.GetBytes(ascii), start);
        public static int IndexOf(List<byte> data, byte[] pat, int start = 0)
        {
            if (pat.Length == 0) return -1;
            for (int i = start; i + pat.Length <= data.Count; i++)
            {
                bool ok = true;
                for (int j = 0; j < pat.Length; j++) if (data[i + j] != pat[j]) { ok = false; break; }
                if (ok) return i;
            }
            return -1;
        }
        public static int LastIndexOf(List<byte> data, string ascii, int start, int endExclusive)
        {
            byte[] pat = Encoding.ASCII.GetBytes(ascii);
            for (int i = endExclusive - pat.Length; i >= start; i--)
            {
                bool ok = true;
                for (int j = 0; j < pat.Length; j++) if (data[i + j] != pat[j]) { ok = false; break; }
                if (ok) return i;
            }
            return -1;
        }
        public static int IndexOfByte(List<byte> data, byte b, int start = 0)
        {
            for (int i = start; i < data.Count; i++) if (data[i] == b) return i;
            return -1;
        }

        static string GetAttr(List<byte> tag, string name)
        {
            string s = Encoding.ASCII.GetString(tag.ToArray());
            var m = System.Text.RegularExpressions.Regex.Match(s, name + "=\"([^\"]*)\"");
            return m.Success ? m.Groups[1].Value : null;
        }
        static List<byte> ReplaceAttr(List<byte> tag, string name, string value)
        {
            string s = Encoding.ASCII.GetString(tag.ToArray());
            var m = System.Text.RegularExpressions.Regex.Match(s, name + "=\"[^\"]*\"");
            if (!m.Success) return tag;
            string rep = name + "=\"" + value + "\"";
            s = s.Substring(0, m.Index) + rep + s.Substring(m.Index + m.Length);
            return new List<byte>(Encoding.ASCII.GetBytes(s));
        }
    }
}
