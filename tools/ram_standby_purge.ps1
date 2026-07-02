# ram_standby_purge.ps1 -- light first-line RAM hygiene for the 3GB omega-vps.
# Purges the STANDBY + low-priority-standby page lists (discardable cached pages)
# back to the free list WITHOUT evicting any active working set. This is the same
# mechanism as Sysinternals RAMMap "-Et" (NtSetSystemInformation /
# SystemMemoryListInformation / MemoryPurgeStandbyList). No process restart, no
# reboot -> live-safe on paper OR live (touches cache only, never open orders).
#
# Scope/limits (be honest): at PEAK thrash the standby list is already near-empty
# (~50MB), so this reclaims little THEN. Its job is periodic hygiene -- reclaim aged
# file-cache/standby BEFORE the box starves -- complementing OmegaRamRelief (which
# restarts the JVM as last resort) and not replacing a RAM bump. Fires every 30 min.
$ErrorActionPreference = 'Continue'
$log = 'C:\Omega\logs\ram_standby_purge.log'
function Log($m){ "$([DateTime]::UtcNow.ToString('yyyy-MM-dd HH:mm:ss')) $m" | Add-Content -Path $log }

$before = [math]::Round((Get-Counter '\Memory\Available MBytes').CounterSamples.CookedValue)
$pin    = [math]::Round((Get-Counter '\Memory\Pages Input/sec').CounterSamples.CookedValue)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class MemNt {
    [DllImport("ntdll.dll")]
    public static extern uint NtSetSystemInformation(int InfoClass, IntPtr Info, int Length);
    [DllImport("advapi32.dll", SetLastError=true)]
    public static extern bool OpenProcessToken(IntPtr h, uint acc, out IntPtr tok);
    [DllImport("advapi32.dll", SetLastError=true)]
    public static extern bool LookupPrivilegeValue(string host, string name, out long luid);
    [DllImport("advapi32.dll", SetLastError=true)]
    public static extern bool AdjustTokenPrivileges(IntPtr tok, bool dis, ref TP newp, int len, IntPtr prev, IntPtr rl);
    [DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
    [StructLayout(LayoutKind.Sequential)] public struct TP { public int Count; public long Luid; public int Attr; }
    static bool EnablePriv(string priv){
        IntPtr tok; if(!OpenProcessToken(GetCurrentProcess(), 0x20, out tok)) return false;
        long luid; if(!LookupPrivilegeValue(null, priv, out luid)) return false;
        TP tp = new TP(); tp.Count=1; tp.Luid=luid; tp.Attr=0x2;
        return AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
    }
    public static uint Purge(int cmd){
        EnablePriv("SeProfileSingleProcessPrivilege");
        IntPtr p = Marshal.AllocHGlobal(4); Marshal.WriteInt32(p, cmd);
        uint r = NtSetSystemInformation(0x50, p, 4); // 0x50 = SystemMemoryListInformation
        Marshal.FreeHGlobal(p); return r;
    }
}
"@

$rc4 = [MemNt]::Purge(4)  # MemoryPurgeStandbyList
$rc5 = [MemNt]::Purge(5)  # MemoryPurgeLowPriorityStandbyList
Start-Sleep -Milliseconds 600
$after = [math]::Round((Get-Counter '\Memory\Available MBytes').CounterSamples.CookedValue)
Log ("purge rc(standby)=$rc4 rc(lowprio)=$rc5 availMB $before -> $after reclaimed=" + ($after-$before) + " pagesIn=$pin")
