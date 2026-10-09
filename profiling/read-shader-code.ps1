param([Parameter(Mandatory)][int]$AttachPid, [Parameter(Mandatory)][string]$Address, [Parameter(Mandatory)][int]$Words, [Parameter(Mandatory)][string]$OutFile)
$ErrorActionPreference = 'Stop'
$game = Get-Process -Id $AttachPid
if ($game.Path -ne 'D:\ps5\gt7\app.exe') { throw 'Expected GT7 process' }
if ($Words -le 0 -or $Words -gt 1048576) { throw 'Invalid capture size' }
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ShaderRead {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, int size, out IntPtr read);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
}
'@
$process = [ShaderRead]::OpenProcess(0x10, $false, $AttachPid)
if ($process -eq [IntPtr]::Zero) { throw 'Could not open process for reading' }
try {
  $ptr = [IntPtr][long][Convert]::ToUInt64($Address.Replace('0x',''),16)
  $first = [byte[]]::new($Words*4)
  $second = [byte[]]::new($Words*4)
  $count = [IntPtr]::Zero
  foreach ($buffer in @($first,$second)) {
    if (-not [ShaderRead]::ReadProcessMemory($process,$ptr,$buffer,$buffer.Length,[ref]$count) -or $count.ToInt64() -ne $buffer.Length) { throw 'Short process-memory read' }
  }
  if ([Convert]::ToBase64String($first) -ne [Convert]::ToBase64String($second)) { throw 'Code changed during capture' }
  [IO.File]::WriteAllBytes($OutFile,$first)
  Get-FileHash -LiteralPath $OutFile
} finally { [void][ShaderRead]::CloseHandle($process) }
