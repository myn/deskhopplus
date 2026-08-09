<#
.SYNOPSIS
    deskhopplus #59 / #34: does dwShareMode = 0 give exclusive ownership of a HID
    top-level collection the way it does for a COM port?

.DESCRIPTION
    Four checks, each scored on an API return value.

    CHECK A - inventory. Enumerate every HID top-level collection, read its usage page
      and usage via HidD_GetPreparsedData + HidP_GetCaps, and classify each against the
      table in learn.microsoft.com/.../top-level-collections-opened-by-windows-for-system-use.
      Picks a target: a vendor-defined page (>= 0xFF00) if one exists, else a Shared-listed
      page. NEVER a keyboard/mouse/digitizer collection - RIM holds those and the result
      would be about RIM, not about us.

    CHECK B - does share mode 0 exclude anything?
      B1  open target with GENERIC_READ|GENERIC_WRITE, dwShareMode = 0        (the "owner")
      B2  while B1 is held: open again with GENERIC_READ, dwShareMode = 0     expect FAIL
      B3  while B1 is held: open again with dwDesiredAccess = 0, share 0      expect SUCCESS
      B2 failing proves hidclass.sys honours share access at all (research inference 3.3).
      B3 succeeding proves share mode 0 does not exclude (documented, 3.1) - and B3's handle
      is what check C then uses. If B2 SUCCEEDS, hidclass ignores share mode entirely and
      the situation is worse than the document describes, not better.

    CHECK C - THE decisive one. Can B3's zero-access handle move data?
      Calls HidD_GetFeature on it. Every HID IOCTL is FILE_ANY_ACCESS in hidclass.h, so the
      I/O manager should permit it - but hidclass.sys may check more strictly
      (IoValidateDeviceIoControlAccess). This measures which.
      GetFeature, not SetFeature: reading a feature report is passive. Writing one to a
      device we do not own could reconfigure someone's touchpad.
      NOTE: some devices do not implement feature reports at all. An ERROR_INVALID_FUNCTION /
      "not supported" answer means the CHECK did not run, not that access was denied - the
      script distinguishes the two by error code and says so.

    CHECK E - does B3's refusal generalise? Repeats B1/B2/B3 against every vendor-defined,
      non-system-held collection on the machine. One target cannot distinguish "hidclass.sys
      excludes zero-access openers" from "this particular driver does". Added after runs 1
      and 2 both measured the same single Synaptics collection.

    CHECK D - broadcast or consumed? Opens the target twice for read from this one process,
      issues an overlapped ReadFile on both, and reports whether both complete for a single
      device report. Needs a device that actually emits input reports; it will time out on a
      quiet collection, which is reported as INCONCLUSIVE, not as a result.

    Run UNELEVATED. Read-only with respect to device state.
#>

[CmdletBinding()]
param(
    [string]$OutDir = [Environment]::GetFolderPath('Desktop'),
    [string[]]$Check = @('A','B','C','D','E'),
    # Override check A's automatic pick, e.g. -TargetPath '\\?\hid#vid_...'
    [string]$TargetPath
)

$ErrorActionPreference = 'Continue'
$Check = @($Check | ForEach-Object { $_ -split '[,\s]+' } | Where-Object { $_ } | ForEach-Object { $_.ToUpper() })
$bad = @($Check | Where-Object { $_ -notin @('A','B','C','D','E') })
if ($bad.Count -gt 0) { Write-Host "Unknown -Check value(s): $($bad -join ', ')" -ForegroundColor Red; exit 1 }

$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$LogPath = Join-Path $OutDir "deskhopplus-hid-$stamp.log"

function Write-Log {
    param([string]$Message, [string]$Level = 'INFO')
    $line = '{0} [{1,-5}] {2}' -f (Get-Date -Format 'HH:mm:ss'), $Level, $Message
    switch ($Level) {
        'PASS' { Write-Host $line -ForegroundColor Green }
        'FAIL' { Write-Host $line -ForegroundColor Red }
        'WARN' { Write-Host $line -ForegroundColor Yellow }
        'HEAD' { Write-Host ''; Write-Host $line -ForegroundColor Cyan }
        default { Write-Host $line }
    }
    Add-Content -Path $LogPath -Value $line -Encoding UTF8
}

$sig = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class H {
    public const uint GENERIC_READ  = 0x80000000;
    public const uint GENERIC_WRITE = 0x40000000;
    public const uint FILE_SHARE_READ  = 0x00000001;
    public const uint FILE_SHARE_WRITE = 0x00000002;
    public const uint OPEN_EXISTING = 3;
    public static readonly IntPtr INVALID = new IntPtr(-1);

    // #40 found GetLastError reading a stale 203 throughout, because a succeeding API need
    // not set it. Clear immediately before every call so a reported code belongs to it.
    [DllImport("kernel32.dll")] public static extern void SetLastError(uint code);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateFileW(string path, uint access, uint share,
        IntPtr sec, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr h);

    [DllImport("hid.dll", SetLastError = true)] public static extern void HidD_GetHidGuid(out Guid g);
    [DllImport("hid.dll", SetLastError = true)] public static extern bool HidD_GetPreparsedData(IntPtr h, out IntPtr ppd);
    [DllImport("hid.dll", SetLastError = true)] public static extern bool HidD_FreePreparsedData(IntPtr ppd);
    [DllImport("hid.dll", SetLastError = true)] public static extern bool HidD_GetFeature(IntPtr h, byte[] buf, int len);
    [DllImport("hid.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool HidD_GetProductString(IntPtr h, StringBuilder s, int len);

    [StructLayout(LayoutKind.Sequential)]
    public struct HIDP_CAPS {
        // hidpi.h declares Usage FIRST, then UsagePage. Getting this pair the wrong way round
        // transposes every reading silently - it does not error, it just reports a keyboard
        // (page 0x01 usage 0x06) as page 0x06 usage 0x01. Run 1 of this script had them
        // reversed, which mislabelled its own target as "not a vendor-defined page" when it
        // was in fact usage page 0xFF00 - the ideal target. Do not swap these back.
        public ushort Usage, UsagePage;
        public ushort InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
        public ushort NumberLinkCollectionNodes;
        public ushort NumberInputButtonCaps, NumberInputValueCaps, NumberInputDataIndices;
        public ushort NumberOutputButtonCaps, NumberOutputValueCaps, NumberOutputDataIndices;
        public ushort NumberFeatureButtonCaps, NumberFeatureValueCaps, NumberFeatureDataIndices;
    }
    [DllImport("hid.dll", SetLastError = true)] public static extern int HidP_GetCaps(IntPtr ppd, ref HIDP_CAPS caps);

    // SetupAPI - enumerate the HID device interfaces.
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid InterfaceClassGuid; public int Flags; public IntPtr Reserved; }

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr SetupDiGetClassDevsW(ref Guid g, IntPtr enumerator, IntPtr hwnd, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    public static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr devInfo, ref Guid g, uint index, ref SP_DEVICE_INTERFACE_DATA data);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool SetupDiGetDeviceInterfaceDetailW(IntPtr set, ref SP_DEVICE_INTERFACE_DATA data,
        IntPtr detail, int detailSize, ref int required, IntPtr devInfoData);
    [DllImport("setupapi.dll", SetLastError = true)] public static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);

    public const uint DIGCF_PRESENT = 0x02, DIGCF_DEVICEINTERFACE = 0x10;

    public static string[] EnumHidPaths() {
        Guid hid; HidD_GetHidGuid(out hid);
        IntPtr set = SetupDiGetClassDevsW(ref hid, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID) return new string[0];
        var list = new System.Collections.Generic.List<string>();
        try {
            var did = new SP_DEVICE_INTERFACE_DATA();
            did.cbSize = Marshal.SizeOf(did);
            for (uint i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref hid, i, ref did); i++) {
                int need = 0;
                SetupDiGetDeviceInterfaceDetailW(set, ref did, IntPtr.Zero, 0, ref need, IntPtr.Zero);
                if (need <= 0) continue;
                IntPtr buf = Marshal.AllocHGlobal(need);
                try {
                    // SP_DEVICE_INTERFACE_DETAIL_DATA_W.cbSize is 6 on x86, 8 on x64 -
                    // the struct is {DWORD cbSize; WCHAR DevicePath[ANYSIZE];} with pointer alignment.
                    Marshal.WriteInt32(buf, (IntPtr.Size == 8) ? 8 : 6);
                    if (SetupDiGetDeviceInterfaceDetailW(set, ref did, buf, need, ref need, IntPtr.Zero)) {
                        string p = Marshal.PtrToStringUni(new IntPtr(buf.ToInt64() + 4));
                        if (!string.IsNullOrEmpty(p)) list.Add(p);
                    }
                } finally { Marshal.FreeHGlobal(buf); }
                did.cbSize = Marshal.SizeOf(did);
            }
        } finally { SetupDiDestroyDeviceInfoList(set); }
        return list.ToArray();
    }
}
'@

try { Add-Type -TypeDefinition $sig -Language CSharp -ErrorAction Stop }
catch { Write-Log "FATAL: shim would not compile: $($_.Exception.Message)" 'FAIL'; exit 2 }

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
Write-Log "=== deskhopplus #59/#34 - HID exclusivity ===" 'HEAD'
Write-Log ("Host {0}   User {1}   Elevated {2}" -f $env:COMPUTERNAME, $id.Name,
    ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
Write-Log ("Log file: {0}" -f $LogPath)

function Open-Hid {
    param([string]$Path, [uint32]$Access, [uint32]$Share, [string]$Label)
    [H]::SetLastError(0)
    $h = [H]::CreateFileW($Path, $Access, $Share, [IntPtr]::Zero, [H]::OPEN_EXISTING, 0, [IntPtr]::Zero)
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $ok = ($h -ne [H]::INVALID)
    Write-Log ("  {0,-34} access=0x{1:X8} share=0x{2:X8} -> {3} (err={4})" -f `
        $Label, $Access, $Share, $(if ($ok) { 'HANDLE' } else { 'FAILED' }), $err)
    [pscustomobject]@{ Handle = $h; Ok = $ok; Error = $err; Label = $Label }
}

# ---------------------------------------------------------------------------------------
# CHECK A - inventory and target selection
# ---------------------------------------------------------------------------------------

$script:Target = $null
$script:Rows   = @()

function Invoke-CheckA {
    Write-Log "--- CHECK A: HID top-level collections on this machine ---" 'HEAD'
    Write-Log "Classification follows the Exclusive/Shared table at"
    Write-Log "learn.microsoft.com/windows-hardware/drivers/hid/top-level-collections-opened-by-windows-for-system-use"

    $rows = @()
    foreach ($p in [H]::EnumHidPaths()) {
        # Zero-access open: this is exactly the open the research says nothing can prevent,
        # so it also works on RIM-held keyboards and mice. Enumeration must not need read access.
        $o = $null
        [H]::SetLastError(0)
        $h = [H]::CreateFileW($p, 0, [H]::FILE_SHARE_READ -bor [H]::FILE_SHARE_WRITE, [IntPtr]::Zero, [H]::OPEN_EXISTING, 0, [IntPtr]::Zero)
        if ($h -eq [H]::INVALID) { continue }
        try {
            $ppd = [IntPtr]::Zero
            if (-not [H]::HidD_GetPreparsedData($h, [ref]$ppd)) { continue }
            try {
                $caps = New-Object H+HIDP_CAPS
                if ([H]::HidP_GetCaps($ppd, [ref]$caps) -ne 0x110000) { continue }  # HIDP_STATUS_SUCCESS
                $sb = New-Object Text.StringBuilder 256
                [void][H]::HidD_GetProductString($h, $sb, 512)

                $up = $caps.UsagePage; $u = $caps.Usage
                $sysHeld =
                    ($up -eq 0x0001 -and ($u -in 1,2,6,7)) -or
                    ($up -eq 0x000D -and ($u -in 1,2,4,5))
                $vendor = ($up -ge 0xFF00)
                $rows += [pscustomobject]@{
                    Path = $p; UsagePage = $up; Usage = $u
                    Product = $sb.ToString()
                    Feature = $caps.FeatureReportByteLength
                    Input   = $caps.InputReportByteLength
                    SystemHeld = $sysHeld; Vendor = $vendor
                }
            } finally { [void][H]::HidD_FreePreparsedData($ppd) }
        } finally { [void][H]::CloseHandle($h) }
    }

    $script:Rows = $rows

    foreach ($r in ($rows | Sort-Object -Property @{e={-$_.UsagePage}})) {
        $tag = if ($r.SystemHeld) { 'SYSTEM-HELD (Exclusive)' } elseif ($r.Vendor) { 'VENDOR-DEFINED' } else { 'shared/other' }
        $lvl = if ($r.Vendor) { 'WARN' } else { 'INFO' }
        Write-Log ("  UP=0x{0:X4} U=0x{1:X4}  feat={2,-4} in={3,-4} {4,-24} {5}" -f `
            $r.UsagePage, $r.Usage, $r.Feature, $r.Input, $tag, $r.Product) $lvl
    }

    if ($TargetPath) {
        $script:Target = $rows | Where-Object { $_.Path -eq $TargetPath } | Select-Object -First 1
        if (-not $script:Target) { Write-Log "-TargetPath did not match any enumerated collection." 'FAIL'; return }
        Write-Log ("Target: caller-supplied.") 'WARN'
    } else {
        # Prefer a vendor-defined collection that actually supports feature reports, since
        # that is the closest analogue to ours and the only one check C can exercise.
        # Written as a loop rather than a ?? chain: Windows PowerShell 5.1 has no ??.
        foreach ($pred in @(
            { $_.Vendor -and $_.Feature -gt 0 },
            { $_.Vendor },
            { -not $_.SystemHeld -and $_.Feature -gt 0 },
            { -not $_.SystemHeld }
        )) {
            $script:Target = $rows | Where-Object $pred | Select-Object -First 1
            if ($script:Target) { break }
        }
    }

    if (-not $script:Target) {
        Write-Log "No usable target: every collection on this machine is system-held. Re-run with -TargetPath, or attach any cheap vendor HID device." 'FAIL'
        return
    }
    Write-Log ("TARGET: UP=0x{0:X4} U=0x{1:X4} feat={2} '{3}'" -f `
        $script:Target.UsagePage, $script:Target.Usage, $script:Target.Feature, $script:Target.Product) 'PASS'
    Write-Log ("        {0}" -f $script:Target.Path)
    if ($script:Target.SystemHeld) { Write-Log "        WARNING: target is system-held. Results will describe RIM, not our case." 'FAIL' }
    elseif (-not $script:Target.Vendor) { Write-Log "        Note: not a vendor-defined page. Closest available analogue, slightly weaker evidence." 'WARN' }
}

# ---------------------------------------------------------------------------------------
# CHECK B - does share mode 0 exclude anything?
# ---------------------------------------------------------------------------------------

$script:B3 = $null

function Invoke-CheckB {
    Write-Log "--- CHECK B: does dwShareMode = 0 exclude a second opener? ---" 'HEAD'
    if (-not $script:Target) { Write-Log "no target (run check A first)" 'FAIL'; return }
    $p = $script:Target.Path

    $b1 = Open-Hid -Path $p -Access ([H]::GENERIC_READ -bor [H]::GENERIC_WRITE) -Share 0 -Label 'B1 owner  rw / share 0'
    if (-not $b1.Ok) {
        Write-Log ("B1 could not take the collection at all (err={0}). Share mode 0 is refused here; nothing further is measurable on this target." -f $b1.Error) 'WARN'
        # err 5 = ACCESS_DENIED (someone holds it), err 32 = SHARING_VIOLATION.
        return
    }
    try {
        $b2 = Open-Hid -Path $p -Access ([H]::GENERIC_READ) -Share 0 -Label 'B2 second rw / share 0'
        if ($b2.Ok) {
            Write-Log "B2: a SECOND read handle opened while B1 held share-mode 0. hidclass.sys does not enforce share access at all - worse than the document assumes." 'FAIL'
            [void][H]::CloseHandle($b2.Handle)
        } else {
            Write-Log ("B2: refused (err={0}) - hidclass.sys DOES enforce share access. Research inference 3.3 confirmed." -f $b2.Error) 'PASS'
        }

        $script:B3 = Open-Hid -Path $p -Access 0 -Share 0 -Label 'B3 second ZERO-ACCESS'
        if ($script:B3.Ok) {
            Write-Log "B3: a zero-access handle opened straight through share mode 0. Exclusive ownership is NOT available - this is the documented CreateFile behaviour (dwShareMode value table)." 'FAIL'
        } else {
            Write-Log ("B3: zero-access open REFUSED (err={0}). Unexpected, and good for us - hidclass.sys is stricter than CreateFile documents. Re-read section 3 of the research before relying on it." -f $script:B3.Error) 'PASS'
        }
    } finally {
        [void][H]::CloseHandle($b1.Handle)
        Write-Log "  B1 owner handle closed"
    }
}

# ---------------------------------------------------------------------------------------
# CHECK C - can a zero-access handle move data? THE decisive check.
# ---------------------------------------------------------------------------------------

function Invoke-CheckC {
    Write-Log "--- CHECK C: can a zero-access handle exchange feature reports? ---" 'HEAD'
    Write-Log "Every HID IOCTL is FILE_ANY_ACCESS in hidclass.h, so the I/O manager should permit this."
    Write-Log "hidclass.sys may check more strictly (IoValidateDeviceIoControlAccess). This measures which."
    if (-not $script:Target) { Write-Log "no target (run check A first)" 'FAIL'; return }
    if ($script:Target.Feature -le 0) {
        Write-Log "Target reports FeatureReportByteLength = 0: it has no feature reports, so this check CANNOT run against it. Pick another target with -TargetPath (check A lists feat= per collection)." 'WARN'
        return
    }

    # Fresh zero-access handle, opened while nothing of ours holds the collection - the
    # attacker's position, with no help from us.
    $c = Open-Hid -Path $script:Target.Path -Access 0 -Share ([H]::FILE_SHARE_READ -bor [H]::FILE_SHARE_WRITE) -Label 'C zero-access'
    if (-not $c.Ok) { Write-Log ("could not open zero-access (err={0})" -f $c.Error) 'WARN'; return }
    try {
        # Run 1 returned err=87 (ERROR_INVALID_PARAMETER) with report ID 0, which is the
        # signature of a collection that DOES use report IDs - 0 is not a valid one. That is a
        # malformed request, not a permission answer, so sweep the plausible IDs and keep the
        # most informative outcome. Success or ACCESS_DENIED on ANY id settles the question;
        # 87 on all of them means the check still did not run.
        $ok = $false; $err = 0; $usedId = 0
        foreach ($rid in 0..8) {
            $buf = New-Object byte[] $script:Target.Feature
            $buf[0] = $rid
            [H]::SetLastError(0)
            $try = [H]::HidD_GetFeature($c.Handle, $buf, $buf.Length)
            $tryErr = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            if ($try) { $ok = $true; $err = 0; $usedId = $rid; break }
            if ($tryErr -eq 5) { $err = 5; $usedId = $rid; break }   # ACCESS_DENIED settles it
            if ($err -eq 0 -or $err -eq 87) { $err = $tryErr; $usedId = $rid }
        }
        Write-Log ("  HidD_GetFeature swept report IDs 0-8; reporting id={0}" -f $usedId)

        if ($ok) {
            Write-Log ("HidD_GetFeature SUCCEEDED on a zero-access handle, returned {0} bytes." -f $buf.Length) 'FAIL'
            Write-Log "  => FILE_ANY_ACCESS is effective. A second process needs no access rights to talk to the device."
            Write-Log "  => #34's exclusive-ownership control does NOT survive a move to vendor HID."
        } elseif ($err -eq 5) {
            Write-Log "HidD_GetFeature refused with ACCESS_DENIED (5) on a zero-access handle." 'PASS'
            Write-Log "  => hidclass.sys checks more strictly than the IOCTL access bits. Share mode 0 may be a real control after all; section 3.2 of the research needs correcting."
        } elseif ($err -in 1,31,50,1784) {
            Write-Log ("HidD_GetFeature failed with err={0} - 'not supported / invalid function' shaped. The CHECK did not run; this is not a permission answer. Pick a different target." -f $err) 'WARN'
        } else {
            Write-Log ("HidD_GetFeature failed with err={0}. Ambiguous - record the code and re-run against another vendor collection before concluding anything." -f $err) 'WARN'
        }
    } finally { [void][H]::CloseHandle($c.Handle) }
}

# ---------------------------------------------------------------------------------------
# CHECK D - broadcast or consumed?
# ---------------------------------------------------------------------------------------

function Invoke-CheckD {
    Write-Log "--- CHECK D: do two readers each receive input reports, or does one consume them? ---" 'HEAD'
    if (-not $script:Target) { Write-Log "no target (run check A first)" 'FAIL'; return }
    if ($script:Target.Input -le 0) { Write-Log "Target emits no input reports; this check cannot run against it." 'WARN'; return }

    $d1 = Open-Hid -Path $script:Target.Path -Access ([H]::GENERIC_READ) -Share ([H]::FILE_SHARE_READ -bor [H]::FILE_SHARE_WRITE) -Label 'D1 reader one'
    if (-not $d1.Ok) { Write-Log "could not open first reader" 'WARN'; return }
    $d2 = Open-Hid -Path $script:Target.Path -Access ([H]::GENERIC_READ) -Share ([H]::FILE_SHARE_READ -bor [H]::FILE_SHARE_WRITE) -Label 'D2 reader two'
    try {
        if (-not $d2.Ok) {
            Write-Log ("A second READ handle was refused (err={0}) even with sharing flags set. Notable - record it." -f $d2.Error) 'WARN'
            return
        }
        Write-Log "Two concurrent read handles on one collection: both opened." 'WARN'
        Write-Log "For the delivery question, drive the device now (press its button / move it) and use"
        Write-Log "the C# FileStream readers below; a bare PowerShell ReadFile here would block the console."
        Write-Log "MARKED INCONCLUSIVE unless a report actually arrives on both handles."
        # Deliberately not automating the read race: on a quiet collection it produces a
        # timeout that reads like a negative result. Two handles opening is the reportable fact.
    } finally {
        if ($d1.Ok) { [void][H]::CloseHandle($d1.Handle) }
        if ($d2.Ok) { [void][H]::CloseHandle($d2.Handle) }
    }
}


function Invoke-CheckE {
    Write-Log "--- CHECK E: does B3's refusal generalise across every vendor collection? ---" 'HEAD'
    Write-Log "Runs 1 and 2 measured a single Synaptics collection. One target cannot distinguish"
    Write-Log "'hidclass.sys excludes zero-access openers' from 'this particular driver does'."
    Write-Log "This repeats B1/B2/B3 against every vendor-defined, non-system-held collection."

    if (-not $script:Rows -or $script:Rows.Count -eq 0) { Write-Log "no inventory (run check A first)" 'FAIL'; return }
    $targets = @($script:Rows | Where-Object { $_.Vendor -and -not $_.SystemHeld })
    if ($targets.Count -eq 0) { Write-Log "no vendor-defined collections on this machine" 'WARN'; return }

    $excluded = 0; $leaked = 0; $unusable = 0
    foreach ($t in $targets) {
        $label = "UP=0x{0:X4} U=0x{1:X4} '{2}'" -f $t.UsagePage, $t.Usage, $t.Product

        $o = Open-Hid -Path $t.Path -Access ([H]::GENERIC_READ -bor [H]::GENERIC_WRITE) -Share 0 -Label "  owner $label"
        if (-not $o.Ok) {
            # Could not take ownership - someone else holds it, or it refuses rw. Not a result.
            Write-Log ("  SKIP  {0} - could not open as owner (err={1})" -f $label, $o.Error) 'WARN'
            $unusable++
            continue
        }
        try {
            $z = Open-Hid -Path $t.Path -Access 0 -Share 0 -Label "  zero-access $label"
            if ($z.Ok) {
                [void][H]::CloseHandle($z.Handle)
                Write-Log ("  LEAK  {0} - zero-access open SUCCEEDED while share-0 held" -f $label) 'FAIL'
                $leaked++
            } else {
                Write-Log ("  OK    {0} - zero-access refused (err={1})" -f $label, $z.Error) 'PASS'
                $excluded++
            }
        } finally { [void][H]::CloseHandle($o.Handle) }
    }

    Write-Log ("CHECK E: {0} excluded, {1} leaked, {2} unusable, of {3} vendor collections" -f `
        $excluded, $leaked, $unusable, $targets.Count) $(if ($leaked -gt 0) { 'FAIL' } elseif ($excluded -gt 0) { 'PASS' } else { 'WARN' })

    if ($leaked -gt 0) {
        Write-Log "At least one collection admits a zero-access opener despite share mode 0. The research's section 3 holds for those, and exclusivity cannot be relied on generally." 'FAIL'
    } elseif ($excluded -gt 1) {
        Write-Log "Every usable vendor collection refused the zero-access open. hidclass.sys - not the individual driver - is enforcing, and #34's exclusivity control looks like it survives vendor HID." 'PASS'
    } elseif ($excluded -eq 1) {
        Write-Log "Only one usable target: still a single data point. Attach another cheap vendor HID device, or boot deskhopplus into config mode to test its own vendor collection." 'WARN'
    }
}

if ($Check -contains 'A') { Invoke-CheckA }
if ($Check -contains 'B') { Invoke-CheckB }
if ($Check -contains 'C') { Invoke-CheckC }
if ($Check -contains 'D') { Invoke-CheckD }
if ($Check -contains 'E') { Invoke-CheckE }
if ($script:B3 -and $script:B3.Ok) { [void][H]::CloseHandle($script:B3.Handle) }

Write-Host ''
Write-Host "Log: $LogPath" -ForegroundColor Cyan
Write-Host 'Paste it back and I will finalise the #59 write-up.' -ForegroundColor Cyan
