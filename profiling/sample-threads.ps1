param([Parameter(Mandatory)][int]$AttachPid, [Parameter(Mandatory)][string]$OutFile, [int[]]$ThreadIds = @(), [int]$Passes = 2, [int]$IntervalMs = 400, [int]$MaxHits = 40, [string]$ExpectedPath = 'D:\ps5\gt7\app.exe')
$dir = 'D:\ps5\gt7'
$env:PATH = "C:\tools\mingw64\bin;$env:PATH"
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Smp {
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint access, bool inherit, uint tid);
    [DllImport("kernel32.dll")] public static extern uint SuspendThread(IntPtr h);
    [DllImport("kernel32.dll")] public static extern uint ResumeThread(IntPtr h);
    [DllImport("kernel32.dll")] public static extern bool GetThreadContext(IntPtr h, IntPtr ctx);
    [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr p, IntPtr addr, byte[] buf, int size, out IntPtr read);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] public static extern int GetThreadDescription(IntPtr h, out IntPtr name);
    [DllImport("kernel32.dll")] public static extern IntPtr LocalFree(IntPtr h);
    public static string ThreadName(IntPtr h) {
        IntPtr p; if (GetThreadDescription(h, out p) < 0 || p == IntPtr.Zero) return "";
        string s = Marshal.PtrToStringUni(p); LocalFree(p); return s;
    }
    public static ulong[] Sample(IntPtr proc, IntPtr th, ulong lo, ulong hi, int maxBytes, out ulong rsp, out ulong rip) {
        rsp = 0; rip = 0;
        IntPtr raw = Marshal.AllocHGlobal(1232 + 32);
        IntPtr ctx = (IntPtr)(((long)raw + 15) & ~15L);
        for (int i = 0; i < 1232; i++) Marshal.WriteByte(ctx, i, 0);
        Marshal.WriteInt32(ctx, 0x30, 0x100001);
        var result = new System.Collections.Generic.List<ulong>();
        if (SuspendThread(th) == 0xFFFFFFFF) { Marshal.FreeHGlobal(raw); return result.ToArray(); }
        try {
            if (GetThreadContext(th, ctx)) {
                rsp = (ulong)Marshal.ReadInt64(ctx, 0x98);
                rip = (ulong)Marshal.ReadInt64(ctx, 0xF8);
                byte[] buf = new byte[0x1000];
                int total = 0;
                while (total < maxBytes) {
                    IntPtr read;
                    if (!ReadProcessMemory(proc, (IntPtr)(long)(rsp + (ulong)total), buf, buf.Length, out read) || (long)read == 0) break;
                    int n = (int)read;
                    for (int i = 0; i + 8 <= n; i += 8) {
                        ulong v = BitConverter.ToUInt64(buf, i);
                        if (v >= lo && v < hi) result.Add(v);
                    }
                    total += buf.Length;
                    if (n < buf.Length) break;
                }
            }
        } finally { ResumeThread(th); Marshal.FreeHGlobal(raw); }
        return result.ToArray();
    }
}
'@

$p = Get-Process -Id $AttachPid -ErrorAction Stop
if ($p.Path -ne $ExpectedPath) { throw "Expected process executable $ExpectedPath" }
$p.Refresh()
$mods = @($p.Modules | ForEach-Object { [pscustomobject]@{ Name = $_.ModuleName; Lo = [uint64]$_.BaseAddress.ToInt64(); Hi = [uint64]$_.BaseAddress.ToInt64() + [uint64]$_.ModuleMemorySize } })
$lo = ($mods | Measure-Object Lo -Minimum).Minimum; $hi = ($mods | Measure-Object Hi -Maximum).Maximum
$ph = [Smp]::OpenProcess(0x0010 -bor 0x0400, $false, $p.Id)
$sb = New-Object System.Text.StringBuilder
for ($pass = 1; $pass -le $Passes; $pass++) {
  foreach ($t in $p.Threads) {
    if ($ThreadIds.Count -gt 0 -and $t.Id -notin $ThreadIds) { continue }
    $th = [Smp]::OpenThread(0x0002 -bor 0x0008 -bor 0x0040, $false, [uint32]$t.Id)
    if ($th -eq [IntPtr]::Zero) { continue }
    $name = [Smp]::ThreadName($th)
    $rsp = [uint64]0; $rip = [uint64]0
    $hits = [Smp]::Sample($ph, $th, [uint64]$lo, [uint64]$hi, 0x10000, [ref]$rsp, [ref]$rip)
    [void][Smp]::CloseHandle($th)
    $named = New-Object System.Collections.Generic.List[string]
    foreach ($v in $hits) {
      $m = $mods | Where-Object { $v -ge $_.Lo -and $v -lt $_.Hi } | Select-Object -First 1
      if ($m) { $named.Add(('{0}+{1:x}' -f $m.Name, ($v - $m.Lo))) }
      if ($named.Count -ge $MaxHits) { break }
    }
    $ripm = $mods | Where-Object { $rip -ge $_.Lo -and $rip -lt $_.Hi } | Select-Object -First 1
    $ripText = if ($ripm) { '{0}+{1:x}' -f $ripm.Name, ($rip - $ripm.Lo) } else { '{0:x}' -f $rip }
    [void]$sb.AppendLine(("{0}`t{1}`t{2}`trip={3}`t{4}" -f $pass, $t.Id, $name, $ripText, ($named -join ' ')))
  }
  Start-Sleep -Milliseconds $IntervalMs
}
[void][Smp]::CloseHandle($ph)
[IO.File]::WriteAllText($OutFile, $sb.ToString())
"wrote $OutFile ($($sb.Length) chars)"
