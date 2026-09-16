using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace DmoPatch
{
    /// System-tray switch: re-applies the patch continuously (watch thread) and offers
    /// Insert new roll / Reset counter / presets / Custom / off / startup / block-updates.
    public static class TrayApp
    {
        // The project is a console Exe so the CLI verbs can print. In tray mode
        // that console is just a stray window (autostart opens one on every
        // logon), so detach from it. Launched from an existing cmd, FreeConsole
        // only detaches us - the user's window stays.
        [DllImport("kernel32.dll")] static extern bool FreeConsole();

        public static void Run()
        {
            try { FreeConsole(); } catch { }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);

            // patch now, then keep patched in the background
            try { Program.PatchAll(); } catch (Exception ex) { MessageBox.Show("patch: " + ex.Message); }
            var watcher = new Thread(() => Program.Watch(500)) { IsBackground = true, Name = "dmo-watch" };
            watcher.Start();

            using (var ni = new NotifyIcon())
            {
                ni.Icon = LoadAppIcon() ?? SystemIcons.Application;
                ni.Text = "D.MO roll switch";
                ni.Visible = true;
                ni.ContextMenuStrip = BuildMenu(ni);
                Application.Run();
            }
        }

        /// Loads the app icon embedded in the assembly (dmo.ico); null on any failure.
        static Icon LoadAppIcon()
        {
            try
            {
                var asm = typeof(TrayApp).Assembly;
                foreach (var name in asm.GetManifestResourceNames())
                {
                    if (name.EndsWith(".ico", StringComparison.OrdinalIgnoreCase))
                        using (var s = asm.GetManifestResourceStream(name))
                            return new Icon(s);
                }
            }
            catch { }
            return null;
        }

        static ContextMenuStrip BuildMenu(NotifyIcon ni)
        {
            var m = new ContextMenuStrip();

            // --- Insert new roll (known rolls at full nominal count) ---
            var insert = new ToolStripMenuItem("Insert new roll…");
            foreach (var kv in Program.Nominal.OrderBy(k => k.Key))
            {
                var it = new ToolStripMenuItem(kv.Key + "  (" + kv.Value + ")", null,
                    (s, e) => { Program.InsertRoll(kv.Key); RefreshStatus(ni); });
                insert.DropDownItems.Add(it);
            }
            var custom = new ToolStripMenuItem("Custom…", null, (s, e) => CustomInput());
            insert.DropDownItems.Add(new ToolStripSeparator());
            insert.DropDownItems.Add(custom);
            m.Items.Add(insert);

            // --- Reset counter (bump current roll back to full nominal) ---
            m.Items.Add(new ToolStripMenuItem("Reset counter", null,
                (s, e) => { Program.ResetCounter(); RefreshStatus(ni); }));

            // --- presets ---
            m.Items.Add(new ToolStripSeparator());
            m.Items.Add(new ToolStripMenuItem("Biggest roll — 30387", null,
                (s, e) => { Program.InsertRoll("30387"); RefreshStatus(ni); }));
            m.Items.Add(new ToolStripMenuItem("Unlimited — 9999 labels", null,
                (s, e) => { if (Program.ReadFlag(out var sku, out _)) Program.SetFlag(sku, 9999); else Program.SetFlag("S0904980", 9999); RefreshStatus(ni); }));

            // --- off ---
            m.Items.Add(new ToolStripSeparator());
            m.Items.Add(new ToolStripMenuItem("Authentic (off)", null,
                (s, e) => { Program.OffFlag(); RefreshStatus(ni); }));

            // --- maintenance ---
            m.Items.Add(new ToolStripSeparator());
            m.Items.Add(new ToolStripMenuItem("Re-apply patch now", null,
                (s, e) => { try { Program.PatchAll(); MessageBox.Show("Patch re-applied."); } catch (Exception ex) { MessageBox.Show(ex.Message); } }));
            m.Items.Add(new ToolStripMenuItem("Add to Windows startup…", null,
                (s, e) => { Program.Autostart("on"); MessageBox.Show("Startup entry added."); }));
            m.Items.Add(new ToolStripMenuItem("Remove from Windows startup", null,
                (s, e) => { Program.Autostart("off"); }));
            m.Items.Add(new ToolStripMenuItem("Block D.MO Connect updates…", null,
                (s, e) => { try { Program.UpdatesBlock("on"); } catch (Exception ex) { MessageBox.Show("Needs admin rights:\n" + ex.Message); } }));

            m.Items.Add(new ToolStripSeparator());
            m.Items.Add(new ToolStripMenuItem("Exit", null, (s, e) => Application.Exit()));

            RefreshStatus(ni);
            return m;
        }

        static void RefreshStatus(NotifyIcon ni)
        {
            if (Program.ReadFlag(out string sku, out int count))
                ni.Text = "D.MO roll: " + sku + " / " + count;
            else
                ni.Text = "D.MO roll: authentic (off)";
        }

        static void CustomInput()
        {
            using (var f = new InputForm("SKU:", "Count:"))
            {
                if (f.ShowDialog() == DialogResult.OK && int.TryParse(f.Value2, out int count))
                {
                    Program.SetFlag(f.Value1, count);
                }
            }
        }

        // Minimal two-field input dialog (no extra references needed).
        class InputForm : Form
        {
            public string Value1; public string Value2;
            TextBox t1, t2;
            public InputForm(string l1, string l2)
            {
                Text = "D.MO roll";
                FormBorderStyle = FormBorderStyle.FixedDialog;
                MaximizeBox = false; MinimizeBox = false;
                StartPosition = FormStartPosition.CenterScreen;
                ClientSize = new Size(260, 120);
                var p1 = new Label { Text = l1, Location = new Point(12, 12), AutoSize = true };
                t1 = new TextBox { Location = new Point(80, 9), Width = 160, Text = "S0904980" };
                var p2 = new Label { Text = l2, Location = new Point(12, 44), AutoSize = true };
                t2 = new TextBox { Location = new Point(80, 41), Width = 160, Text = "220" };
                var ok = new Button { Text = "OK", DialogResult = DialogResult.OK, Location = new Point(95, 78), Width = 70 };
                var cancel = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, Location = new Point(170, 78), Width = 70 };
                Controls.AddRange(new Control[] { p1, t1, p2, t2, ok, cancel });
                AcceptButton = ok; CancelButton = cancel;
                ok.Click += (s, e) => { Value1 = t1.Text.Trim(); Value2 = t2.Text.Trim(); };
            }
        }
    }
}
