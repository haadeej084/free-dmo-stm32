# OpenDMOfw - time a print job from the spooler's side, without touching the printer.
#
# The USB bulk-OUT pipe is flow-controlled by the printer: when its buffer is
# full the host waits. So the time a RAW job spends in the Windows queue is,
# to within the driver's render time, the time the printer took to print it.
# That lets two driver modes (ESC h text vs ESC i graphics, ESC T speed) be
# compared on a genuine unit with nothing sent to the printer - the read-only
# probe cannot see speed, and polling ESC A mid-raster would splice bytes into
# the job.
#
#   powershell -File tools\time_print_job.ps1 -Printer "DYMO LabelWriter 550 (Kopie 1)" -Label "Tekst"
#
# Waits (up to -WaitSec) for a job to appear, then samples every 50 ms until
# it is gone and prints: appear -> first 'Printing' -> gone, in ms, plus the
# spooled size. Append -Out to log the line.
param(
    [string]$Printer = "DYMO LabelWriter 550 (Kopie 1)",
    [string]$Label   = "",
    [int]$WaitSec    = 180,
    [string]$Out     = ""
)
$sw = [Diagnostics.Stopwatch]::StartNew()
$job = $null
while ($sw.Elapsed.TotalSeconds -lt $WaitSec) {
    $job = Get-PrintJob -PrinterName $Printer -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($job) { break }
    Start-Sleep -Milliseconds 50
}
if (-not $job) { "no job appeared within $WaitSec s"; exit 1 }
$t0 = $sw.Elapsed.TotalMilliseconds
$id = $job.Id; $size = $job.Size; $tPrinting = -1; $states = @()
"job $id appeared (size $size B)"
while ($true) {
    $j = Get-PrintJob -PrinterName $Printer -ID $id -ErrorAction SilentlyContinue
    if (-not $j) { break }
    $st = "$($j.JobStatus)"
    if ($states.Count -eq 0 -or $states[-1] -ne $st) { $states += $st; "  {0,7:N0} ms  {1}" -f ($sw.Elapsed.TotalMilliseconds - $t0), $st }
    if ($tPrinting -lt 0 -and $st -match 'Printing') { $tPrinting = $sw.Elapsed.TotalMilliseconds }
    if ($j.Size -gt $size) { $size = $j.Size }
    Start-Sleep -Milliseconds 50
}
$t1 = $sw.Elapsed.TotalMilliseconds
$line = "{0}  job {1}: total {2:N0} ms (printing phase {3:N0} ms), spooled {4:N0} B  [{5}]" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $id, ($t1 - $t0), ($(if ($tPrinting -ge 0) { $t1 - $tPrinting } else { -1 })), $size, $Label
$line
if ($Out) { Add-Content -Path $Out -Value $line -Encoding utf8 }
