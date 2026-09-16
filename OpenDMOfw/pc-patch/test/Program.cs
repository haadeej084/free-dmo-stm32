// Offline tests for the DYMO.LabelAPI.dll patcher.
//
// The real DLL cannot be redistributed, so these tests build a SYNTHETIC
// assembly with the same shapes the patcher anchors on
// (LabelWriterRollDetectionPrinterCommunication, a nested UpdatePrinterStatus
// state machine with MoveNext, set_SkuNumber fed by get_InsertedSKU, a
// set_LabelsRemaining call and an ERollValidity local) and run the patcher
// against it.
//
// The assertions that matter are about BRANCH WIRING. Two label captures used
// to read back L[L.Count-1] / L[L.Count-2] at a point where those were not the
// instruction the comment claimed, which silently made the flag file a no-op:
//   - File.Exists == true jumped to the unconditional Br, skipping the parse
//     block, leaving the SKU local null;
//   - a SUCCESSFUL int.TryParse jumped to "load default count", discarding the
//     parsed value.
// Both produced a patched DLL that loaded and ran fine - only the feature was
// gone. Hence: assert the targets, not just that patching succeeds.
//
// Run:  dotnet run --project pc-patch/test    (exit 0 = all pass)

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using dnlib.DotNet;
using dnlib.DotNet.Emit;

namespace DmoPatch.Tests
{
    public static class Program
    {
        static int _fails;

        static void Check(bool ok, string what)
        {
            Console.WriteLine((ok ? "ok   " : "FAIL ") + what);
            if (!ok) _fails++;
        }

        // ---------------------------------------------------------------- fixture
        // Builds a module shaped like the parts of DYMO.LabelAPI.dll the patcher
        // touches, then serialises it. Nothing here needs to be runnable - only
        // structurally faithful. It is reloaded as a ModuleDefMD so the tests go
        // through exactly the production entry points.
        static byte[] BuildFakeModuleBytes()
        {
            var mod = new ModuleDefUser("Fake.dll", Guid.NewGuid(),
                new AssemblyRefUser(new AssemblyNameInfo(typeof(object).Assembly.GetName().FullName)));
            mod.Kind = ModuleKind.Dll;
            var asm = new AssemblyDefUser("Fake", new Version(1, 0, 0, 0));
            asm.Modules.Add(mod);

            var ct = mod.CorLibTypes;

            // enum ERollValidity { Valid = 0 }  - only the type NAME is matched on.
            var validity = new TypeDefUser("DmoFake", "ERollValidity",
                mod.CorLibTypes.GetTypeRef("System", "Enum"));
            validity.Attributes = dnlib.DotNet.TypeAttributes.Public | dnlib.DotNet.TypeAttributes.Sealed;
            validity.Fields.Add(new FieldDefUser("value__",
                new FieldSig(ct.Int32),
                dnlib.DotNet.FieldAttributes.Public | dnlib.DotNet.FieldAttributes.SpecialName |
                dnlib.DotNet.FieldAttributes.RTSpecialName));
            mod.Types.Add(validity);

            var printerType = new TypeDefUser("DmoFake", "LabelWriterRollDetectionPrinterCommunication",
                mod.CorLibTypes.Object.TypeDefOrRef);
            printerType.Attributes = dnlib.DotNet.TypeAttributes.Public;
            mod.Types.Add(printerType);

            MethodDefUser Add(string name, MethodSig sig)
            {
                var m = new MethodDefUser(name, sig,
                    dnlib.DotNet.MethodImplAttributes.IL,
                    dnlib.DotNet.MethodAttributes.Public | dnlib.DotNet.MethodAttributes.Virtual |
                    dnlib.DotNet.MethodAttributes.NewSlot);
                m.Body = new CilBody();
                printerType.Methods.Add(m);
                return m;
            }

            var setSku = Add("set_SkuNumber", MethodSig.CreateInstance(ct.Void, ct.String));
            setSku.Body.Instructions.Add(Instruction.Create(OpCodes.Ret));
            var getSku = Add("get_SkuNumber", MethodSig.CreateInstance(ct.String));
            getSku.Body.Instructions.Add(Instruction.Create(OpCodes.Ldnull));
            getSku.Body.Instructions.Add(Instruction.Create(OpCodes.Ret));
            var getIns = Add("get_InsertedSKU", MethodSig.CreateInstance(ct.String));
            getIns.Body.Instructions.Add(Instruction.Create(OpCodes.Ldnull));
            getIns.Body.Instructions.Add(Instruction.Create(OpCodes.Ret));
            var setRem = Add("set_LabelsRemaining", MethodSig.CreateInstance(ct.Void, ct.Int32));
            setRem.Body.Instructions.Add(Instruction.Create(OpCodes.Ret));

            // ValidateResult with three "ldc.i4.0; ret" sites (patch C flips all but the first).
            var validate = new MethodDefUser("ValidateResult",
                MethodSig.CreateInstance(ct.Boolean),
                dnlib.DotNet.MethodImplAttributes.IL, dnlib.DotNet.MethodAttributes.Public);
            validate.Body = new CilBody();
            for (int i = 0; i < 3; i++)
            {
                validate.Body.Instructions.Add(Instruction.Create(OpCodes.Ldc_I4_0));
                validate.Body.Instructions.Add(Instruction.Create(OpCodes.Ret));
            }
            printerType.Methods.Add(validate);

            // Nested compiler-generated state machine with MoveNext.
            var nested = new TypeDefUser("<UpdatePrinterStatus>d__7", mod.CorLibTypes.Object.TypeDefOrRef);
            nested.Attributes = dnlib.DotNet.TypeAttributes.NestedPrivate;
            printerType.NestedTypes.Add(nested);

            var moveNext = new MethodDefUser("MoveNext", MethodSig.CreateInstance(ct.Void),
                dnlib.DotNet.MethodImplAttributes.IL, dnlib.DotNet.MethodAttributes.Public);
            var body = new CilBody { InitLocals = true };
            var locPrinter = new Local(printerType.ToTypeSig());
            var locValidity = new Local(validity.ToTypeSig());
            body.Variables.Add(locPrinter);
            body.Variables.Add(locValidity);

            var ins = body.Instructions;
            // set_LabelsRemaining must appear at or before the anchor (searched backwards).
            ins.Add(Instruction.Create(OpCodes.Ldloc, locPrinter));
            ins.Add(Instruction.Create(OpCodes.Ldc_I4, 5));
            ins.Add(Instruction.Create(OpCodes.Callvirt, setRem));
            // The anchor pair: callvirt get_InsertedSKU immediately followed by callvirt set_SkuNumber.
            ins.Add(Instruction.Create(OpCodes.Ldloc, locPrinter));
            ins.Add(Instruction.Create(OpCodes.Ldloc, locPrinter));
            ins.Add(Instruction.Create(OpCodes.Callvirt, getIns));
            ins.Add(Instruction.Create(OpCodes.Callvirt, setSku));
            // get_SkuNumber must appear after the anchor.
            ins.Add(Instruction.Create(OpCodes.Ldloc, locPrinter));
            ins.Add(Instruction.Create(OpCodes.Callvirt, getSku));
            ins.Add(Instruction.Create(OpCodes.Pop));
            ins.Add(Instruction.Create(OpCodes.Ret));
            moveNext.Body = body;
            nested.Methods.Add(moveNext);

            using (var ms = new MemoryStream())
            {
                mod.Write(ms);
                return ms.ToArray();
            }
        }

        static ModuleDefMD LoadFake(out MethodDef moveNext)
        {
            var mod = ModuleDefMD.Load(BuildFakeModuleBytes());
            var printer = Patcher.FindType(mod, "LabelWriterRollDetectionPrinterCommunication");
            moveNext = printer.NestedTypes
                              .SelectMany(nt => nt.Methods)
                              .First(m => m.Name == "MoveNext");
            return mod;
        }

        // ---------------------------------------------------------------- helpers
        static bool Calls(Instruction i, string name) =>
            i.Operand is IMethod m && m.ToString().IndexOf(name, StringComparison.Ordinal) >= 0;

        static int IndexOfCall(IList<Instruction> ins, string name)
        {
            for (int k = 0; k < ins.Count; k++) if (Calls(ins[k], name)) return k;
            return -1;
        }

        // ---------------------------------------------------------------- tests
        static void TestIlInjectBranchWiring()
        {
            Console.WriteLine("== IL injection: branch wiring ==");
            var mod = LoadFake(out var moveNext);
            Patcher.IlInject(mod);

            var ins = moveNext.Body.Instructions;

            Check(ins.Any(i => i.OpCode.Code == Code.Ldstr &&
                               (i.Operand as string) == Patcher.FlagFileName),
                  "flag-file name is injected (marker for IsPatched)");

            int iExists = IndexOfCall(ins, "Exists");
            Check(iExists >= 0, "File.Exists call present");
            var brHasFlag = ins[iExists + 1];
            Check(brHasFlag.OpCode.Code == Code.Brtrue || brHasFlag.OpCode.Code == Code.Brtrue_S,
                  "File.Exists is followed by a conditional branch");

            // THE BUG: this used to target the unconditional Br, skipping the parse block.
            var t1 = brHasFlag.Operand as Instruction;
            Check(t1 != null, "File.Exists branch has an instruction target");
            Check(t1 != null && t1.OpCode.FlowControl != FlowControl.Branch,
                  "File.Exists branch does not target another unconditional branch");
            int i1 = ins.IndexOf(t1);
            Check(i1 >= 0 && Calls(ins[i1 + 1], "ReadAllText"),
                  "File.Exists==true lands on the block that reads the flag file");

            // The three forward branches into the guard must share one target.
            int iParse = IndexOfCall(ins, "TryParse");
            Check(iParse >= 0, "int.TryParse call present");
            var brHaveCnt = ins[iParse + 1];
            Check(brHaveCnt.OpCode.Code == Code.Brtrue || brHaveCnt.OpCode.Code == Code.Brtrue_S,
                  "TryParse is followed by a conditional branch");
            var t2 = brHaveCnt.Operand as Instruction;

            var uncond = ins.Where(i => i.OpCode.Code == Code.Br || i.OpCode.Code == Code.Br_S).ToList();
            Check(uncond.Count == 1, "exactly one unconditional branch was injected");
            var t3 = uncond.Count == 1 ? uncond[0].Operand as Instruction : null;

            Check(t2 != null && t2 == t3,
                  "TryParse-success and defaults-done branch to the same guard label");

            // THE BUG: TryParse success used to land on "ldc.i4 DefaultCount; stloc cnt",
            // throwing away the count that had just been parsed.
            int i2 = t2 == null ? -1 : ins.IndexOf(t2);
            Check(i2 >= 0 && !(ins[i2].OpCode.Code == Code.Ldc_I4 &&
                               ins[i2].Operand is int v && v == Patcher.DefaultCount),
                  "guard label is not the 'load default count' instruction");
            Check(i2 >= 0 && Calls(ins[i2 + 1], "get_SkuNumber"),
                  "guard label starts the 'is the detected SKU empty?' test");
            Check(i2 >= 0 && Calls(ins[i2 + 2], "IsNullOrEmpty"),
                  "guard tests the detected SKU with string.IsNullOrEmpty");

            // Every branch target must still be inside the method.
            bool allResolve = ins.Where(i => i.Operand is Instruction)
                                 .All(i => ins.IndexOf((Instruction)i.Operand) >= 0);
            Check(allResolve, "every branch target resolves inside the method body");

            // A second pass must be a no-op (the tool re-patches every 500 ms).
            int before = ins.Count;
            Patcher.IlInject(mod);
            Check(moveNext.Body.Instructions.Count == before, "re-injection is idempotent");

            // The patched module must still be writable (catches malformed bodies).
            bool wrote = true;
            try { using (var ms = new MemoryStream()) mod.Write(ms); }
            catch (Exception ex) { wrote = false; Console.WriteLine("     write failed: " + ex.Message); }
            Check(wrote, "patched module still serialises");
        }

        static void TestValidateResult()
        {
            Console.WriteLine("\n== ValidateResult flip ==");
            var mod = LoadFake(out _);
            Patcher.PatchValidateResult(mod);
            var v = Patcher.FindType(mod, "LabelWriterRollDetectionPrinterCommunication")
                           .Methods.First(m => m.Name == "ValidateResult");
            var ins = v.Body.Instructions;
            var loads = ins.Where((x, i) => i + 1 < ins.Count && ins[i + 1].OpCode.Code == Code.Ret).ToList();
            Check(loads.Count == 3, "three return sites found");
            Check(loads[0].OpCode.Code == Code.Ldc_I4_0, "first return (null guard) still returns false");
            Check(loads[1].OpCode.Code == Code.Ldc_I4_1 && loads[2].OpCode.Code == Code.Ldc_I4_1,
                  "later returns flipped to true");
        }

        static void TestCatalogLengthPreserved()
        {
            Console.WriteLine("\n== catalog / exclusion byte edits ==");
            // Length preservation is load-bearing: these blobs sit inside the DLL,
            // so the edit must not move anything after them.
            string xml =
                "PADPAD<SKUs>\n" +
                "  <DieCutSKU SKU=\"S0904980\" Region=\"US\" PaperName=\"1933086 LW DURABLE 104x159mm\" />\n" +
                "  <DieCutSKU SKU=\"30387\" Region=\"US\" PaperName=\"Other\" />\n" +
                "</SKUs>TAIL";
            byte[] input = Encoding.ASCII.GetBytes(xml);

            byte[] outp = Patcher.CatalogPatch(input, new[] { "S0904980" });
            Check(outp.Length == input.Length, "CatalogPatch preserves total length");
            string s = Encoding.ASCII.GetString(outp);
            Check(s.Contains("Region=\"Global\""), "target SKU region set to Global");
            Check(s.Contains("PaperName=\"1933086 LW DURABLE 104x159mm\""), "real paper name kept");
            Check(s.StartsWith("PADPAD") && s.EndsWith("TAIL"), "surrounding bytes untouched");
            Check(Patcher.GetSkuPaper(outp, "S0904980") == "1933086 LW DURABLE 104x159mm",
                  "GetSkuPaper reads the paper back");

            byte[] excl = Encoding.ASCII.GetBytes(
                "<ExcludedPapers><PaperName>1933086 LW DURABLE 104x159mm</PaperName></ExcludedPapers>");
            byte[] blanked = Patcher.ExcludedPapersBlank(excl, "1933086 LW DURABLE 104x159mm");
            Check(blanked.Length == excl.Length, "ExcludedPapersBlank preserves length");
            Check(Encoding.ASCII.GetString(blanked).Contains("<PaperName>   "),
                  "exclusion entry blanked out");
        }

        public static int Main()
        {
            TestIlInjectBranchWiring();
            TestValidateResult();
            TestCatalogLengthPreserved();
            Console.WriteLine();
            Console.WriteLine(_fails == 0 ? "ALL PATCHER TESTS PASSED" : _fails + " test(s) FAILED");
            return _fails == 0 ? 0 : 1;
        }
    }
}
