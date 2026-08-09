<#
.SYNOPSIS
    Follow-up for deskhopplus issue #40. Settles the three things the first run left open.

.DESCRIPTION
    Run 1 produced: check 1 FAIL, check 2 PASS, check 3 PASS. Two of those need firming up
    before they go in the issue as facts.

    PART A - the cursor matrix.
      Run 1 compared "notepad focused" (worked) against "elevated cmd focused" (failed).
      Two variables changed at once - elevation AND window type - so elevation is not yet
      proven to be the cause, and the integrity check that run 1 printed was unsound
      (process-object mandatory policy is NO_WRITE_UP, so a same-user elevated process is
      readable from medium IL - "readable" never meant "not elevated").

      This part reads the real integrity level and elevation flag out of each foreground
      process's token, and tests four conditions:
        A1 our own console      (medium IL control)
        A2 cmd.exe unelevated   (same window type as the test, medium IL - isolates window type)
        A3 cmd.exe elevated     (the actual question)
        A4 our own console again (does the block clear when focus leaves, or is it sticky?)

      Score on the API RETURN VALUE, not the final cursor position: a blocked call leaves the
      cursor exactly where it was, but so does nothing else - whereas a human nudging the mouse
      mid-test also produces "not at target", from a call that actually succeeded.

    PART B - is the logon task real?
      Run 1 showed schtasks.exe denied but Register-ScheduledTask succeeded. Registration
      succeeding is not the same as a task that exists, is enabled, and has a working logon
      trigger. This registers it, dumps the full XML and state, then deletes it.

    PART C - which DLP/EDR agent is actually on this box?
      Run 1 found Sense, WinDefend and MDCoreSvc all STOPPED, which means Microsoft Endpoint
      DLP is almost certainly not the agent in play - the research's Purview assumption may
      be aimed at the wrong product. This inventories third-party EDR/DLP agents so the
      clipboard question can be re-asked against whatever is actually enforcing.

    PART D - how does the existing KVM software get away with it?
      MKRoamer works between these same two machines. Either it never needs SetCursorPos on the
      Windows side, or it runs above medium integrity. Its token integrity distinguishes the two.

    PART E - does the same integrity boundary block the CLIPBOARD?
      Check 1 proved UIPI blocks cursor placement. The clipboard relay crosses the same boundary,
      so it needs its own answer. Three conditions: medium writes (control), a HIGH-IL writer that
      exits, and a HIGH-IL writer that keeps owning the clipboard while holding the foreground.
      Reports the clipboard owner's integrity level, so a pass is provably a cross-boundary read.
      Overwrites the clipboard several times.

    PART F - can Trellix device control even touch our device?
      From docs/research/trellix-dlp-endpoint-constraints.md (issue #57): plug-and-play device
      rules are enforced by the optional 'hdlpdbk' driver, which #40's inventory did not list.
      Checks whether it is absent, present-but-disabled, or running, and reads the device-class
      filter registry for the classes a CDC+HID composite device travels. Read-only.

    PART G - does Trellix intercept clipboard reads of CLASSIFIED content?
      #40's clipboard pass used unlabeled content, and a Trellix clipboard rule can specify
      Classification = "is any data (ALL)" with a Block action, so unlabeled content is not
      exempt by design. Repeats the read with labeled content, separates an API read from a real
      paste gesture, and looks for the local artefacts that would let the helper tell "Trellix
      blocked it" from "our bug". Overwrites the clipboard.

    PART H - is the local Trellix policy cache readable structure or opaque?
      Part G found the agent caches policy world-readable under C:\ProgramData\McAfee\DLP, at paths
      no Trellix document mentions. Measures entropy and extracts printable runs from SCM.opg and
      friends: if it is parseable it names the rules actually in force, answering the clipboard-rule
      and device-rule questions without waiting on ePO. Read-only. Any strings recovered are
      corporate policy content, so the log is sensitive.

    Run UNELEVATED, same as before. Approve the UAC prompts when they appear.
#>

[CmdletBinding()]
param(
    [string]$OutDir = [Environment]::GetFolderPath('Desktop'),
    # NOTE: deliberately no ValidateSet. powershell.exe -File passes every argument as a
    # literal string, so "-Part A,D" arrives as the single string "A,D" and a ValidateSet
    # rejects it. Split and validate by hand so both "-Part A,D" and "-Part A,D" (real array,
    # when dot-sourced or run via -Command) work identically.
    [string[]]$Part = @('A', 'B', 'C', 'D', 'E', 'F', 'G', 'H')
)

$ErrorActionPreference = 'Continue'

$Part = @($Part | ForEach-Object { $_ -split '[,\s]+' } | Where-Object { $_ } | ForEach-Object { $_.ToUpper() })
$bad = @($Part | Where-Object { $_ -notin @('A', 'B', 'C', 'D', 'E', 'F', 'G', 'H') })
if ($bad.Count -gt 0) {
    Write-Host "Unknown -Part value(s): $($bad -join ', '). Valid parts are A through H (e.g. -Part A,D)." -ForegroundColor Red
    exit 1
}

$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$LogPath = Join-Path $OutDir "deskhopplus-confirm-$stamp.log"

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

public static class W2 {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }

    [DllImport("user32.dll", SetLastError = true)] public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", SetLastError = true)] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();

    // Clipboard (Part E)
    [DllImport("user32.dll", SetLastError = true)] public static extern bool OpenClipboard(IntPtr hWndNewOwner);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool CloseClipboard();
    [DllImport("user32.dll", SetLastError = true)] public static extern IntPtr GetClipboardData(uint uFormat);
    [DllImport("user32.dll", SetLastError = true)] public static extern uint EnumClipboardFormats(uint format);
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern int GetClipboardFormatNameW(uint format, StringBuilder lpsz, int cch);
    [DllImport("user32.dll", SetLastError = true)] public static extern IntPtr GetClipboardOwner();
    [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr GlobalLock(IntPtr hMem);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool GlobalUnlock(IntPtr hMem);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern UIntPtr GlobalSize(IntPtr hMem);

    // #40 found GetLastError reading a stale 203 throughout, because a succeeding API need not
    // set it. Clear it immediately before each call so a reported code belongs to that call.
    [DllImport("kernel32.dll")] public static extern void SetLastError(uint code);

    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr h);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr proc, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool GetTokenInformation(IntPtr token, int cls, IntPtr buf, uint len, out uint ret);
    [DllImport("advapi32.dll", SetLastError = true)] static extern IntPtr GetSidSubAuthority(IntPtr sid, uint idx);
    [DllImport("advapi32.dll", SetLastError = true)] static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);

    const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
    const uint TOKEN_QUERY = 0x0008;
    const int  TokenElevation = 20;
    const int  TokenIntegrityLevel = 25;

    // Reads the real integrity level + elevation flag from the target process token.
    // Everything here is query-only and needs no privileges beyond the caller's own.
    public static string DescribeProcess(int pid) {
        IntPtr h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
        if (h == IntPtr.Zero) return "OpenProcess denied (err=" + Marshal.GetLastWin32Error() + ")";
        try {
            IntPtr tok;
            if (!OpenProcessToken(h, TOKEN_QUERY, out tok)) return "OpenProcessToken denied (err=" + Marshal.GetLastWin32Error() + ")";
            try {
                uint len = 0;
                GetTokenInformation(tok, TokenIntegrityLevel, IntPtr.Zero, 0, out len);
                if (len == 0) return "integrity query failed (err=" + Marshal.GetLastWin32Error() + ")";
                IntPtr buf = Marshal.AllocHGlobal((int)len);
                try {
                    if (!GetTokenInformation(tok, TokenIntegrityLevel, buf, len, out len))
                        return "integrity query failed (err=" + Marshal.GetLastWin32Error() + ")";
                    IntPtr sid = Marshal.ReadIntPtr(buf);            // TOKEN_MANDATORY_LABEL.Label.Sid
                    int count = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
                    uint rid = (uint)Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(count - 1)));
                    string il = rid >= 0x4000 ? "SYSTEM" : rid >= 0x3000 ? "HIGH" : rid >= 0x2000 ? "MEDIUM" : rid >= 0x1000 ? "LOW" : "UNTRUSTED";

                    string elev = "unknown";
                    IntPtr ebuf = Marshal.AllocHGlobal(4);
                    try {
                        uint elen;
                        if (GetTokenInformation(tok, TokenElevation, ebuf, 4, out elen))
                            elev = (Marshal.ReadInt32(ebuf) != 0) ? "True" : "False";
                    } finally { Marshal.FreeHGlobal(ebuf); }

                    return il + " (rid=0x" + rid.ToString("X") + ", elevated=" + elev + ")";
                } finally { Marshal.FreeHGlobal(buf); }
            } finally { CloseHandle(tok); }
        } finally { CloseHandle(h); }
    }
}
'@

try { Add-Type -TypeDefinition $sig -Language CSharp -ErrorAction Stop }
catch { Write-Log "FATAL: shim would not compile: $($_.Exception.Message)" 'FAIL'; exit 2 }

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
Write-Log "=== deskhopplus issue #40 - confirmation run ===" 'HEAD'
Write-Log ("Host {0}   User {1}" -f $env:COMPUTERNAME, $id.Name)
Write-Log ("This process: {0}" -f [W2]::DescribeProcess($PID))
Write-Log ("Log file: {0}" -f $LogPath)

# ---------------------------------------------------------------------------------------
# PART A - cursor matrix with real integrity levels
# ---------------------------------------------------------------------------------------

function Get-Foreground {
    $h = [W2]::GetForegroundWindow()
    $procId = [uint32]0
    [void][W2]::GetWindowThreadProcessId($h, [ref]$procId)
    $sb = New-Object Text.StringBuilder 512
    [void][W2]::GetWindowTextW($h, $sb, $sb.Capacity)
    $name = '<unknown>'
    try { $name = (Get-Process -Id $procId -ErrorAction Stop).ProcessName } catch { }
    [pscustomobject]@{
        Handle    = $h
        ProcessId = [int]$procId
        Name      = $name
        Title     = $sb.ToString()
        Integrity = [W2]::DescribeProcess([int]$procId)
    }
}

function Test-Cursor {
    param([string]$Condition, [int]$Attempts = 3)

    $fg = Get-Foreground
    Write-Log ("[{0}] foreground: pid={1} {2} '{3}' | token: {4}" -f $Condition, $fg.ProcessId, $fg.Name, $fg.Title, $fg.Integrity)

    $vx = [W2]::GetSystemMetrics(76); $vy = [W2]::GetSystemMetrics(77)
    $vw = [W2]::GetSystemMetrics(78); $vh = [W2]::GetSystemMetrics(79)

    $successes = 0
    $errors = @()
    for ($i = 1; $i -le $Attempts; $i++) {
        $before = New-Object W2+POINT
        [void][W2]::GetCursorPos([ref]$before)
        $tx = [Math]::Min($vx + $vw - 2, [Math]::Max($vx + 1, $before.X + (60 * $i)))
        $ty = [Math]::Min($vy + $vh - 2, [Math]::Max($vy + 1, $before.Y + (40 * $i)))

        $ok  = [W2]::SetCursorPos($tx, $ty)
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Start-Sleep -Milliseconds 250
        $after = New-Object W2+POINT
        [void][W2]::GetCursorPos([ref]$after)
        $moved = ([Math]::Abs($after.X - $tx) -le 2 -and [Math]::Abs($after.Y - $ty) -le 2)

        # The RETURN VALUE is the signal, not the final position. A blocked call leaves the
        # cursor exactly where it was; a human touching the mouse mid-test also produces
        # "not at target", but from a call that succeeded. Scoring on position alone reports
        # a phantom block whenever someone jiggles the mouse - which is what run 3 did.
        $stalled = ($before.X -eq $after.X -and $before.Y -eq $after.Y)
        if ($ok) { $successes++ } else { $errors += $err }
        $note = ''
        if (-not $ok) { $note = '  <- API REFUSED' }
        elseif (-not $moved -and -not $stalled) { $note = '  <- call succeeded but cursor ended elsewhere: physical mouse movement, not a block' }
        elseif (-not $moved) { $note = '  <- call succeeded yet cursor did not move: worth a second look' }

        Write-Log ("[{0}]   attempt {1}: SetCursorPos({2},{3}) ret={4} err={5} | {6},{7} -> {8},{9} | at-target={10}{11}" -f `
            $Condition, $i, $tx, $ty, $ok, $err, $before.X, $before.Y, $after.X, $after.Y, $moved, $note)
    }

    Write-Log ("[{0}] RESULT: {1}/{2} SetCursorPos calls accepted by the API" -f $Condition, $successes, $Attempts) `
        $(if ($successes -eq $Attempts) { 'PASS' } elseif ($successes -eq 0) { 'FAIL' } else { 'WARN' })

    [pscustomobject]@{ Condition = $Condition; Successes = $successes; Attempts = $Attempts; Foreground = $fg; Errors = $errors }
}

function Invoke-PartA {
    Write-Log "--- PART A: cursor matrix (window type vs elevation) ---" 'HEAD'
    $matrix = @()

    # A1 - our own console, medium IL
    $mine = [W2]::GetConsoleWindow()
    if ($mine -ne [IntPtr]::Zero) { [void][W2]::SetForegroundWindow($mine); Start-Sleep -Milliseconds 600 }
    $matrix += Test-Cursor -Condition 'A1 own-console-medium'

    # A2 - cmd.exe UNELEVATED: same window type as the failing test, so if this also fails,
    #      the cause is the console window, not elevation.
    Write-Log "A2: launching an UNELEVATED cmd.exe..."
    $plain = $null
    try {
        $plain = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" `
            -ArgumentList '/c title DESKHOPPLUS-PLAIN & timeout /t 40 /nobreak' -PassThru -ErrorAction Stop
    } catch { Write-Log "unelevated cmd launch failed: $($_.Exception.Message)" 'WARN' }
    if ($plain) {
        for ($i = 0; $i -lt 20; $i++) {
            Start-Sleep -Milliseconds 250
            try { $plain.Refresh() } catch { }
            if ($plain.MainWindowHandle -ne [IntPtr]::Zero) { break }
        }
        [void][W2]::SetForegroundWindow($plain.MainWindowHandle)
        Start-Sleep -Seconds 2
        $matrix += Test-Cursor -Condition 'A2 cmd-unelevated'
        try { if (-not $plain.HasExited) { $plain.Kill() } } catch { }
    }

    # A3 - cmd.exe ELEVATED: the actual question. Extra settle time so the UAC secure
    #      desktop transition cannot be mistaken for the effect being measured.
    Write-Log "A3: launching an ELEVATED cmd.exe - approve the UAC prompt, then leave input alone."
    $elev = $null
    try {
        $elev = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" `
            -ArgumentList '/c title DESKHOPPLUS-ELEV & timeout /t 60 /nobreak' -Verb RunAs -PassThru -ErrorAction Stop
    } catch { Write-Log "elevated launch failed/declined: $($_.Exception.Message)" 'WARN' }

    if ($elev) {
        $deadline = (Get-Date).AddSeconds(30)
        do { Start-Sleep -Milliseconds 500; $fg = Get-Foreground } while ($fg.ProcessId -ne $elev.Id -and (Get-Date) -lt $deadline)

        if ($fg.ProcessId -ne $elev.Id) {
            Write-Log ("elevated cmd never took the foreground (foreground pid={0} {1})" -f $fg.ProcessId, $fg.Name) 'WARN'
        } else {
            Write-Log "settling 4s so the UAC secure-desktop transition is well clear of the measurement..."
            Start-Sleep -Seconds 4
            $matrix += Test-Cursor -Condition 'A3 cmd-ELEVATED'
        }
    }

    # A4 - back to our own console: does the block clear with focus, or is it sticky?
    #
    # SetForegroundWindow is useless here: a medium-IL process cannot pull the foreground away
    # from a high-IL window, so calling it leaves the elevated window focused and silently
    # re-runs A3 under an A4 label (this is exactly what run 2 did). Only real human input can
    # move that focus, so ask for a click and VERIFY the foreground changed before believing it.
    $a3Pid = -1
    if ($elev) { $a3Pid = $elev.Id }
    Write-Host ''
    Write-Host '  Click on this PowerShell window now - a real mouse click, the API cannot do it' -ForegroundColor Yellow
    Write-Host '  because we are medium integrity and the focused window is high. Then press Enter.' -ForegroundColor Yellow
    Read-Host '  Press Enter once this window is genuinely focused'

    $fgNow = Get-Foreground
    if ($fgNow.ProcessId -eq $a3Pid -or $fgNow.Integrity -match 'HIGH|SYSTEM') {
        Write-Log ("A4 INVALID: foreground is still pid={0} [{1}] - this would just repeat A3, so it proves nothing about stickiness. Skipping." -f $fgNow.ProcessId, $fgNow.Integrity) 'WARN'
    } else {
        $matrix += Test-Cursor -Condition 'A4 own-console-again'
    }

    Write-Log "PART A matrix:" 'HEAD'
    foreach ($m in $matrix) {
        Write-Log ("  {0,-24} {1}/{2}  [{3}]" -f $m.Condition, $m.Successes, $m.Attempts, $m.Foreground.Integrity)
    }

    $a2 = $matrix | Where-Object { $_.Condition -like 'A2*' }
    $a3 = $matrix | Where-Object { $_.Condition -like 'A3*' }
    $a4 = $matrix | Where-Object { $_.Condition -like 'A4*' }

    if ($a3 -and $a3.Foreground.Integrity -notmatch 'HIGH|SYSTEM') {
        Write-Log "A3's foreground process was NOT high integrity - the elevated condition never actually happened, so nothing is proven about UIPI." 'WARN'
    } elseif ($a2 -and $a2.Successes -eq 0) {
        Write-Log "CONCLUSION: cursor moves fail against an UNELEVATED console too - the cause is the console window / window type, NOT elevation." 'WARN'
    } elseif ($a3 -and $a3.Successes -eq 0 -and $a2 -and $a2.Successes -eq $a2.Attempts) {
        Write-Log "CONCLUSION: SetCursorPos works against an unelevated cmd window and fails against an elevated one. Elevation is the cause - check 1 is a genuine FAIL." 'FAIL'
        if (-not $a4) { Write-Log "  scope of the block is UNRESOLVED - A4 did not run with a genuinely different foreground window." 'WARN' }
        elseif ($a4.Successes -eq $a4.Attempts) { Write-Log "  and the block clears as soon as focus leaves the elevated window - it is a per-focus constraint, not a session-wide one." }
        else { Write-Log "  and it does NOT recover after focus moved to a medium-IL window - the block is sticky, which is worse." 'FAIL' }
    } elseif ($a3 -and $a3.Successes -eq $a3.Attempts) {
        Write-Log "CONCLUSION: SetCursorPos DID work with a confirmed high-integrity window focused - run 1's failure was a transient (likely the UAC transition). Check 1 passes." 'PASS'
    }
}

# ---------------------------------------------------------------------------------------
# PART B - is the registered logon task actually real and enabled?
# ---------------------------------------------------------------------------------------

function Invoke-PartB {
    Write-Log "--- PART B: does the logon task really exist, enabled, with a logon trigger? ---" 'HEAD'
    $name = 'DeskHopPlusProbe_Confirm'

    try {
        $action  = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\cmd.exe" -Argument '/c exit'
        $trigger = New-ScheduledTaskTrigger -AtLogOn -User $id.Name
        Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Force -ErrorAction Stop | Out-Null
        Write-Log "Register-ScheduledTask -> OK"
    } catch {
        Write-Log "Register-ScheduledTask FAILED: $($_.Exception.Message)" 'FAIL'
        return
    }

    try {
        $t = Get-ScheduledTask -TaskName $name -ErrorAction Stop
        Write-Log ("Get-ScheduledTask: State={0}  Path={1}  Author={2}" -f $t.State, $t.TaskPath, $t.Author)
        Write-Log ("Principal: UserId={0}  LogonType={1}  RunLevel={2}" -f $t.Principal.UserId, $t.Principal.LogonType, $t.Principal.RunLevel)
        foreach ($tr in $t.Triggers) { Write-Log ("Trigger: {0} enabled={1} delay={2}" -f $tr.CimClass.CimClassName, $tr.Enabled, $tr.Delay) }

        $info = Get-ScheduledTaskInfo -TaskName $name -ErrorAction SilentlyContinue
        if ($info) { Write-Log ("TaskInfo: NextRunTime={0} LastTaskResult={1}" -f $info.NextRunTime, $info.LastTaskResult) }

        Write-Log "Full task XML:"
        foreach ($l in ((Export-ScheduledTask -TaskName $name) -split "`r?`n")) { Write-Log ("    " + $l) }

        # Prove the action actually fires when the trigger is honoured.
        Start-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 3
        $info2 = Get-ScheduledTaskInfo -TaskName $name -ErrorAction SilentlyContinue
        if ($info2) { Write-Log ("After manual Start: LastRunTime={0} LastTaskResult={1} (0 = ran cleanly)" -f $info2.LastRunTime, $info2.LastTaskResult) }

        if ($t.State -eq 'Ready' -or $t.State -eq 'Running') {
            Write-Log "PART B: task exists, is enabled, and carries a logon trigger for this user." 'PASS'
        } else {
            Write-Log ("PART B: task registered but State={0} - policy may be disabling it." -f $t.State) 'WARN'
        }
    } catch {
        Write-Log "Inspecting the task failed: $($_.Exception.Message)" 'FAIL'
    } finally {
        try { Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction Stop; Write-Log "cleanup: task removed" }
        catch { Write-Log "cleanup FAILED - remove '$name' by hand: $($_.Exception.Message)" 'WARN' }
    }
}

# ---------------------------------------------------------------------------------------
# PART C - which security agent is actually enforcing on this machine?
# ---------------------------------------------------------------------------------------

function Invoke-PartC {
    Write-Log "--- PART C: which EDR/DLP agent is actually running? ---" 'HEAD'
    Write-Log "Run 1 found Sense/WinDefend/MDCoreSvc all STOPPED, so Microsoft Endpoint DLP is probably not the enforcing agent here."

    $vendorPattern = 'crowdstrike|cylance|carbonblack|cbdefense|sentinel|tanium|netskope|zscaler|forcepoint|websense|digitalguardian|dgagent|symantec|mcafee|trellix|sophos|trend ?micro|cortex|traps|airwatch|bit9|code42|proofpoint|varonis|nightfall|purview|dlp'

    Write-Log "Running services matching known EDR/DLP vendors:"
    $hits = Get-Service | Where-Object { $_.Status -eq 'Running' -and ($_.Name -match $vendorPattern -or $_.DisplayName -match $vendorPattern) }
    if ($hits) { foreach ($s in $hits) { Write-Log ("  {0,-28} {1}" -f $s.Name, $s.DisplayName) } }
    else { Write-Log "  (none matched the vendor list)" }

    Write-Log "Filesystem/minifilter drivers currently running (these are where clipboard and device policy get enforced):"
    try {
        $drv = Get-CimInstance Win32_SystemDriver -ErrorAction Stop | Where-Object { $_.State -eq 'Running' -and ($_.Name -match $vendorPattern -or $_.DisplayName -match $vendorPattern) }
        if ($drv) { foreach ($d in $drv) { Write-Log ("  {0,-24} {1}" -f $d.Name, $d.DisplayName) } } else { Write-Log "  (none matched)" }
    } catch { Write-Log "  driver query failed: $($_.Exception.Message)" 'WARN' }

    Write-Log "Registered antivirus/antispyware products (SecurityCenter2):"
    try {
        $av = Get-CimInstance -Namespace 'root\SecurityCenter2' -ClassName AntiVirusProduct -ErrorAction Stop
        foreach ($a in $av) { Write-Log ("  {0}  (state=0x{1:X})" -f $a.displayName, $a.productState) }
    } catch { Write-Log "  SecurityCenter2 query failed: $($_.Exception.Message)" 'WARN' }

    Write-Log "Defender status detail:"
    try {
        $mp = Get-MpComputerStatus -ErrorAction Stop
        Write-Log ("  AMRunningMode={0} RealTimeProtectionEnabled={1} AntivirusEnabled={2} IsTamperProtected={3}" -f $mp.AMRunningMode, $mp.RealTimeProtectionEnabled, $mp.AntivirusEnabled, $mp.IsTamperProtected)
    } catch { Write-Log "  Get-MpComputerStatus failed (consistent with Defender being replaced): $($_.Exception.Message)" 'WARN' }

    Write-Log "Device installation restriction policy (pre-answers part of check 4):"
    foreach ($k in @(
        'HKLM:\SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions',
        'HKLM:\SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions\DenyDeviceIDs',
        'HKLM:\SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions\AllowDeviceIDs',
        'HKLM:\SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions\DenyDeviceClasses',
        'HKLM:\SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions\AllowDeviceClasses',
        'HKLM:\SOFTWARE\Policies\Microsoft\Windows\RemovableStorageDevices'
    )) {
        if (Test-Path $k) {
            $props = Get-ItemProperty -Path $k -ErrorAction SilentlyContinue
            $flat = ($props.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join '; '
            Write-Log ("  {0}: {1}" -f $k, $flat) 'WARN'
        } else {
            Write-Log ("  {0}: absent" -f $k)
        }
    }

    Write-Log "Existing usbser/COM ports already present (shows the in-box driver binds at all here):"
    try {
        $ports = Get-CimInstance Win32_PnPEntity -ErrorAction Stop | Where-Object { $_.Name -match '\(COM\d+\)' }
        if ($ports) { foreach ($p in $ports) { Write-Log ("  {0}  [{1}]  status={2}" -f $p.Name, $p.DeviceID, $p.Status) } }
        else { Write-Log "  (no COM ports present right now - expected until the CDC interface exists)" }
    } catch { Write-Log "  PnP query failed: $($_.Exception.Message)" 'WARN' }
}

# ---------------------------------------------------------------------------------------

function Invoke-PartD {
    Write-Log "--- PART D: how does the existing KVM software get away with it? ---" 'HEAD'
    Write-Log "MKRoamer works between these same two machines. Either it never needs SetCursorPos on"
    Write-Log "the Windows side (because Windows is the box the physical mouse is plugged into, so it"
    Write-Log "captures input rather than injecting position), or it runs above medium integrity and"
    Write-Log "UIPI simply does not apply to it. Its integrity level distinguishes the two."

    # \bkvm\b, not bare "kvm" - the unanchored form matches "CoworkVMService" and similar.
    $pattern = 'roamer|\bkvm\b|synergy|barrier|inputleap|mouse ?without ?borders|deskhop|sharemouse|multiplicity'

    Write-Log "Matching processes and their token integrity:"
    $found = $false
    foreach ($p in (Get-Process | Where-Object { $_.ProcessName -match $pattern })) {
        $found = $true
        Write-Log ("  pid={0,-7} {1,-24} {2}" -f $p.Id, $p.ProcessName, [W2]::DescribeProcess($p.Id)) 'WARN'
    }
    if (-not $found) { Write-Log "  (no matching process running right now - start MKRoamer and re-run: -Part D)" }

    Write-Log "Matching services (a service runs as SYSTEM and is immune to UIPI by construction):"
    $svc = Get-Service | Where-Object { $_.Name -match $pattern -or $_.DisplayName -match $pattern }
    if ($svc) {
        foreach ($s in $svc) { Write-Log ("  {0,-28} {1,-10} {2}" -f $s.Name, $s.Status, $s.DisplayName) 'WARN' }
        try {
            foreach ($s in $svc) {
                $w = Get-CimInstance Win32_Service -Filter "Name='$($s.Name)'" -ErrorAction Stop
                Write-Log ("    {0}: StartName={1} StartMode={2} Path={3}" -f $w.Name, $w.StartName, $w.StartMode, $w.PathName)
            }
        } catch { Write-Log "    service detail query failed: $($_.Exception.Message)" 'WARN' }
    } else { Write-Log "  (no matching service)" }

    Write-Log "Matching autostart entries (shows how it gets launched, which decides its integrity):"
    foreach ($k in @(
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run',
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
    )) {
        if (Test-Path $k) {
            $props = Get-ItemProperty -Path $k -ErrorAction SilentlyContinue
            foreach ($pr in ($props.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' })) {
                if ($pr.Name -match $pattern -or "$($pr.Value)" -match $pattern) { Write-Log ("  {0}\{1} = {2}" -f $k, $pr.Name, $pr.Value) 'WARN' }
            }
        }
    }
    try {
        $tasks = Get-ScheduledTask -ErrorAction Stop | Where-Object { $_.TaskName -match $pattern -or $_.TaskPath -match $pattern }
        foreach ($t in $tasks) { Write-Log ("  task {0}{1}: State={2} RunLevel={3} UserId={4}" -f $t.TaskPath, $t.TaskName, $t.State, $t.Principal.RunLevel, $t.Principal.UserId) 'WARN' }
    } catch { }
}

function Read-ClipboardRaw {
    param([string]$Label)

    $opened = $false
    $openErr = 0
    for ($i = 0; $i -lt 10; $i++) {
        [W2]::SetLastError(0)
        if ([W2]::OpenClipboard([IntPtr]::Zero)) { $opened = $true; break }
        $openErr = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Start-Sleep -Milliseconds 150
    }
    if (-not $opened) {
        Write-Log ("[{0}] OpenClipboard REFUSED after 10 tries (err={1})" -f $Label, $openErr) 'FAIL'
        return [pscustomobject]@{ Label = $Label; Opened = $false; Text = $null; OwnerIntegrity = 'n/a'; Error = $openErr }
    }

    $text = $null
    $err  = 0
    try {
        # Who owns the clipboard, and at what integrity? This is the whole point of the check:
        # it proves the content really was placed there by a higher-integrity process.
        $ownerDesc = 'no owner (data outlived the process that set it)'
        $ownerHwnd = [W2]::GetClipboardOwner()
        if ($ownerHwnd -ne [IntPtr]::Zero) {
            $ownerPid = [uint32]0
            [void][W2]::GetWindowThreadProcessId($ownerHwnd, [ref]$ownerPid)
            $ownerName = '<unknown>'
            try { $ownerName = (Get-Process -Id $ownerPid -ErrorAction Stop).ProcessName } catch { }
            $ownerDesc = "pid=$ownerPid $ownerName | $([W2]::DescribeProcess([int]$ownerPid))"
        }
        Write-Log ("[{0}] clipboard owner: {1}" -f $Label, $ownerDesc)

        $fmts = @()
        $f = 0
        while (($f = [W2]::EnumClipboardFormats($f)) -ne 0) {
            $sb = New-Object Text.StringBuilder 256
            $n = [W2]::GetClipboardFormatNameW($f, $sb, $sb.Capacity)
            $fmts += $(if ($n -gt 0) { "$f=$($sb.ToString())" } else { "$f" })
        }
        Write-Log ("[{0}] formats: {1}" -f $Label, ($fmts -join ', '))

        [W2]::SetLastError(0)
        $h = [W2]::GetClipboardData(13)   # CF_UNICODETEXT
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if ($h -eq [IntPtr]::Zero) {
            Write-Log ("[{0}] GetClipboardData(CF_UNICODETEXT) returned NULL (err={1})" -f $Label, $err) 'FAIL'
        } else {
            $p = [W2]::GlobalLock($h)
            if ($p -ne [IntPtr]::Zero) {
                $text = [Runtime.InteropServices.Marshal]::PtrToStringUni($p)
                [void][W2]::GlobalUnlock($h)
                $shown = ($text -replace '[\r\n]+', ' ')
                if ($shown.Length -gt 60) { $shown = $shown.Substring(0, 60) + '...' }
                Write-Log ("[{0}] read {1} chars: {2}" -f $Label, $text.Length, $shown)
            } else {
                $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                Write-Log ("[{0}] GlobalLock failed (err={1})" -f $Label, $err) 'FAIL'
            }
        }
        [pscustomobject]@{ Label = $Label; Opened = $true; Text = $text; OwnerIntegrity = $ownerDesc; Error = $err }
    } finally {
        [void][W2]::CloseClipboard()
    }
}

function Invoke-PartE {
    Write-Log "--- PART E: can a medium-IL process read a clipboard written by a HIGH-IL process? ---" 'HEAD'
    Write-Log "Check 1 proved UIPI blocks cursor placement across the integrity boundary. The clipboard"
    Write-Log "relay crosses the same boundary, so it needs its own answer rather than an assumption."
    Write-Log "NOTE: this overwrites your clipboard several times."

    # E0 - control: we set it ourselves at medium IL, we read it back.
    $m0 = "deskhopplus-E0-$(Get-Random)"
    try { Set-Clipboard -Value $m0 -ErrorAction Stop } catch { Write-Log "Set-Clipboard failed: $($_.Exception.Message)" 'WARN' }
    Start-Sleep -Milliseconds 400
    $e0 = Read-ClipboardRaw -Label 'E0 medium-writes'
    $e0ok = ($e0.Text -eq $m0)
    Write-Log ("E0 control (medium writes, medium reads): {0}" -f $e0ok) $(if ($e0ok) { 'PASS' } else { 'FAIL' })

    # E1 - elevated writer that EXITS: data placed by a high-IL process, owner gone by read time.
    $m1 = "deskhopplus-E1-$(Get-Random)"
    Write-Log "E1: elevated clip.exe writes the clipboard and exits - approve the UAC prompt."
    try {
        $p1 = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" `
            -ArgumentList "/c echo $m1| clip.exe" -Verb RunAs -PassThru -ErrorAction Stop
        $p1.WaitForExit(20000) | Out-Null
    } catch { Write-Log "E1 elevated launch failed/declined: $($_.Exception.Message)" 'WARN' }
    Start-Sleep -Milliseconds 800
    $e1 = Read-ClipboardRaw -Label 'E1 high-writes-then-exits'
    $e1ok = ($e1.Text -ne $null -and $e1.Text.Trim() -eq $m1)
    Write-Log ("E1 (HIGH-IL process wrote it, then exited): readable = {0}" -f $e1ok) $(if ($e1ok) { 'PASS' } else { 'FAIL' })

    # E2 - the strict case: an elevated writer that STAYS ALIVE owning the clipboard, and is
    #      also the foreground window. Both integrity variables hostile at once.
    $m2 = "deskhopplus-E2-$(Get-Random)"
    Write-Log "E2: elevated PowerShell writes the clipboard and HOLDS it while focused - approve the UAC prompt."
    $p2 = $null
    try {
        $inner = "Set-Clipboard -Value $m2; Write-Host 'holding clipboard - closes itself'; Start-Sleep -Seconds 45"
        $p2 = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
            -ArgumentList "-NoProfile -Sta -Command `"$inner`"" -Verb RunAs -PassThru -ErrorAction Stop
    } catch { Write-Log "E2 elevated launch failed/declined: $($_.Exception.Message)" 'WARN' }

    if ($p2) {
        Start-Sleep -Seconds 6
        $fg = Get-Foreground
        Write-Log ("E2 foreground at read time: pid={0} {1} | {2}" -f $fg.ProcessId, $fg.Name, $fg.Integrity)
        $e2 = Read-ClipboardRaw -Label 'E2 high-holds-and-focused'
        $e2ok = ($e2.Text -ne $null -and $e2.Text.Trim() -eq $m2)
        Write-Log ("E2 (HIGH-IL process owns the clipboard and holds the foreground): readable = {0}" -f $e2ok) $(if ($e2ok) { 'PASS' } else { 'FAIL' })

        Write-Log "PART E conclusion:" 'HEAD'
        if (-not $e0ok) {
            Write-Log "  Control failed - clipboard reads are broken for an unrelated reason. Nothing proven." 'WARN'
        } elseif ($e1ok -and $e2ok) {
            Write-Log "  Clipboard reads cross the integrity boundary freely, including while a HIGH-IL process both owns the clipboard and holds the foreground. UIPI constrains cursor placement but NOT clipboard relay." 'PASS'
        } elseif ($e1ok -and -not $e2ok) {
            Write-Log "  Readable once the elevated writer exits, but NOT while it holds the clipboard/foreground. The relay needs to defer and retry on focus change, exactly like cursor placement." 'WARN'
        } else {
            Write-Log "  Content written by an elevated process is NOT readable from medium IL. Clipboard relay silently drops anything copied out of an elevated app - a real constraint for #31." 'FAIL'
        }
    }
}

function Invoke-PartF {
    Write-Log "--- PART F: is Trellix device control actually able to touch our device? ---" 'HEAD'
    Write-Log "Per docs/research/trellix-dlp-endpoint-constraints.md, plug-and-play device rules are"
    Write-Log "enforced by the 'hdlpdbk' device filter driver, which Trellix documents as optional"
    Write-Log "('can be disabled in configuration'). #40's driver inventory did not list it. F1/F2 test that."

    # F1 - absent, or merely stopped? Those are very different risks: a disabled module can be
    #      re-enabled by policy at any time; an uninstalled one cannot, without a deployment.
    Write-Log "F1: hdlp* driver services and on-disk driver files"
    $svcSeen = @()
    try {
        $drv = Get-CimInstance Win32_SystemDriver -ErrorAction Stop | Where-Object { $_.Name -like 'hdlp*' }
        if ($drv) {
            foreach ($d in $drv) {
                $svcSeen += $d.Name.ToLower()
                Write-Log ("  service {0,-16} State={1,-8} StartMode={2,-9} {3}" -f $d.Name, $d.State, $d.StartMode, $d.DisplayName)
            }
        } else { Write-Log "  (no hdlp* driver services registered at all)" 'WARN' }
    } catch { Write-Log "  driver query failed: $($_.Exception.Message)" 'WARN' }

    $filesSeen = @()
    foreach ($f in (Get-ChildItem 'C:\Windows\System32\drivers\hdlp*.sys' -ErrorAction SilentlyContinue)) {
        $filesSeen += $f.BaseName.ToLower()
        Write-Log ("  file    {0,-16} {1,9} bytes  ver={2}" -f $f.Name, $f.Length, $f.VersionInfo.FileVersion)
    }

    $dbkService = ($svcSeen -contains 'hdlpdbk')
    $dbkFile    = ($filesSeen -contains 'hdlpdbk')
    if ($dbkService) {
        Write-Log "  hdlpdbk IS REGISTERED - the research inference that device blocking is inactive is WRONG. Check its State above." 'FAIL'
    } elseif ($dbkFile) {
        Write-Log "  hdlpdbk is on disk but has no registered service: installed but switched off. Revocable by policy at any time." 'WARN'
    } else {
        Write-Log "  hdlpdbk is absent from both the service list and disk: device blocking is not deployed on this machine." 'PASS'
    }

    # F2 - the highest-value read in the whole document: the device-class filter registry is the
    #      ground truth for whether Trellix sits in the stack our device would travel.
    Write-Log "F2: device-class filter drivers for the classes our composite device would land in"
    $classes = [ordered]@{
        '{4d36e978-e325-11ce-bfc1-08002be10318}' = 'Ports (COM & LPT) - where a usbser.sys CDC interface lands'
        '{88BAE032-5A81-49f0-BC3D-A4FF138216D6}' = 'USBDevice'
        '{745a17a0-74d3-11d0-b6fe-00a0c90f57da}' = 'HIDClass - the HID interfaces'
        '{36fc9e60-c465-11cf-8056-444553540000}' = 'USB (system) - the composite parent'
    }
    $anyDlpFilter = $false
    foreach ($guid in $classes.Keys) {
        $k = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$guid"
        if (-not (Test-Path $k)) { Write-Log ("  {0}: key absent" -f $classes[$guid]) 'WARN'; continue }
        $p = Get-ItemProperty -Path $k -ErrorAction SilentlyContinue
        $upper = @($p.UpperFilters); $lower = @($p.LowerFilters)
        $u = if ($upper -and $upper[0]) { $upper -join ', ' } else { '(none)' }
        $l = if ($lower -and $lower[0]) { $lower -join ', ' } else { '(none)' }
        Write-Log ("  {0}" -f $classes[$guid])
        Write-Log ("     UpperFilters: {0}" -f $u)
        Write-Log ("     LowerFilters: {0}" -f $l)
        if ("$u $l" -match 'hdlp|mfe|trellix|mcafee') { $anyDlpFilter = $true; Write-Log "     ^ a DLP/security filter is present on this class" 'FAIL' }
    }
    if (-not $anyDlpFilter) {
        Write-Log "  No Trellix/McAfee filter on any class our device uses - nothing of theirs sits in that device stack." 'PASS'
    }

    # F3 - pin the agent build so the product-guide version used by the research can be matched.
    Write-Log "F3: Trellix/McAfee DLP agent build"
    $agentDirs = @('C:\Program Files\McAfee\DLP\Agent', 'C:\Program Files\Trellix\DLP\Agent',
                   'C:\Program Files (x86)\McAfee\DLP\Agent', 'C:\Program Files (x86)\Trellix\DLP\Agent')
    $foundAgent = $false
    foreach ($d in $agentDirs) {
        if (Test-Path $d) {
            $foundAgent = $true
            Write-Log ("  {0}" -f $d)
            foreach ($e in (Get-ChildItem $d -Recurse -Filter '*.exe' -ErrorAction SilentlyContinue | Select-Object -First 12)) {
                Write-Log ("     {0,-24} ver={1}" -f $e.Name, $e.VersionInfo.FileVersion)
            }
        }
    }
    if (-not $foundAgent) { Write-Log "  (no agent directory readable at the expected paths)" 'WARN' }

    # F4/F5 - device stack state. Until the CDC interface exists this is a control measurement:
    #         it shows the Ports class binds normally for other devices on this machine.
    Write-Log "F4/F5: current COM ports (control) and any USB device in a problem state"
    try {
        foreach ($p in (Get-CimInstance Win32_PnPEntity -ErrorAction Stop | Where-Object { $_.Name -match '\(COM\d+\)' })) {
            Write-Log ("  COM: {0} | status={1} | {2}" -f $p.Name, $p.Status, $p.DeviceID)
        }
    } catch { Write-Log "  PnP query failed: $($_.Exception.Message)" 'WARN' }
    try {
        $bad = Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object { $_.Status -ne 'OK' -and $_.InstanceId -like 'USB*' }
        if ($bad) {
            Write-Log "  USB devices NOT in an OK state (a Trellix block should look like this - present but errored):" 'WARN'
            foreach ($b in $bad) { Write-Log ("     {0} | Status={1} Problem={2} | {3}" -f $b.FriendlyName, $b.Status, $b.Problem, $b.InstanceId) }
        } else { Write-Log "  every present USB device is in an OK state" 'PASS' }
    } catch { Write-Log "  Get-PnpDevice failed: $($_.Exception.Message)" 'WARN' }
}

function Invoke-PartG {
    Write-Log "--- PART G: does Trellix DLP intercept clipboard reads of CLASSIFIED content? ---" 'HEAD'
    Write-Log "#40's clipboard pass used UNLABELED content. The research shows a Trellix clipboard rule"
    Write-Log "can specify Classification = 'is any data (ALL)' with a Block action, so unlabeled content"
    Write-Log "is NOT exempt by design - and labeled content is the likeliest thing to be covered."
    Write-Log "NOTE: this overwrites your clipboard."

    # G1 - the labeled-content read.
    Write-Host ''
    Write-Host '  ACTION NEEDED --------------------------------------------------------' -ForegroundColor Yellow
    Write-Host '  Copy text from the most sensitive/classified corporate document you can' -ForegroundColor Yellow
    Write-Host '  legitimately open - the strongest label available to you, not a test    ' -ForegroundColor Yellow
    Write-Host '  string. Then come back and press Enter.                                 ' -ForegroundColor Yellow
    Write-Host '  ----------------------------------------------------------------------' -ForegroundColor Yellow
    Read-Host '  Press Enter once you have copied labeled content'

    $g1 = Read-ClipboardRaw -Label 'G1 labeled-content'
    $g1ok = ($g1.Text -ne $null -and $g1.Text.Length -gt 0)
    Write-Log ("G1 (API read of labeled content): readable = {0}" -f $g1ok) $(if ($g1ok) { 'PASS' } else { 'FAIL' })

    if (-not $g1ok) {
        Write-Log ("G3 failure detail: OpenClipboard opened={0}, error={1}. A block that surfaces as an error is DETECTABLE by the helper - far better than silent empty/substituted content." -f $g1.Opened, $g1.Error) 'WARN'
    }

    $labelish = @()
    Write-Log "G1 format scan for classification-bearing formats:"
    # Re-open briefly just to name the formats, so the label evidence is explicit in the log.
    if ([W2]::OpenClipboard([IntPtr]::Zero)) {
        try {
            $f = 0
            while (($f = [W2]::EnumClipboardFormats($f)) -ne 0) {
                $sb = New-Object Text.StringBuilder 256
                if ([W2]::GetClipboardFormatNameW($f, $sb, $sb.Capacity) -gt 0) {
                    $n = $sb.ToString()
                    if ($n -match 'MSIP|Purview|Label|Classif|DLP|McAfee|Trellix|Titus|Boldon|Janus') {
                        $labelish += $n
                        Write-Log ("     $f = $n") 'WARN'
                    }
                }
            }
        } finally { [void][W2]::CloseClipboard() }
    }
    if ($labelish.Count -eq 0) {
        Write-Log "     none found - the content may carry no client-side label, which weakens G1 the same way #40's test was weakened." 'WARN'
    }

    # G2 - is enforcement API-shaped or paste-gesture-shaped? This decides whether the relay's
    #      read path is even in scope. No API can perform a real paste gesture, so ask.
    Write-Host ''
    Write-Host '  Now press Ctrl+V into Notepad (or any app) with that same clipboard content.' -ForegroundColor Yellow
    $pasteWorked = Read-Host '  Did the paste succeed? (y = pasted fine / n = blocked or a Trellix popup appeared)'

    Write-Log ("G2 manual paste of the same content succeeded: {0}" -f $pasteWorked)
    if ($g1ok -and $pasteWorked -match '^n') {
        Write-Log "  Paste blocked but API read allowed: enforcement is paste-gesture-shaped and the relay's read path sits OUTSIDE it." 'WARN'
    } elseif (-not $g1ok -and $pasteWorked -match '^n') {
        Write-Log "  Both blocked: enforcement is API-shaped and the clipboard relay is directly in scope." 'FAIL'
    } elseif ($g1ok -and $pasteWorked -match '^y') {
        Write-Log "  Neither blocked. Informative only if a blocking rule actually exists - which G4 is needed to establish." 'PASS'
    }

    # G4 - the client console: both a policy readout and the 'Trellix blocked it vs our bug'
    #      discriminator the relay's logging will need.
    Write-Log "G4: DLP Endpoint Console availability"
    $consoles = @(
        'C:\Program Files\McAfee\DLP\Agent\Tools\DlpConsoleRunner.exe',
        'C:\Program Files\Trellix\DLP\Agent\Tools\DlpConsoleRunner.exe'
    )
    $consoleFound = $null
    foreach ($c in $consoles) { if (Test-Path $c) { $consoleFound = $c; Write-Log ("  present: {0}" -f $c) 'WARN' } }
    if ($consoleFound) {
        $go = Read-Host '  Launch the DLP console to read Notifications History and About? (y/n)'
        if ($go -match '^y') {
            try {
                Start-Process $consoleFound -ErrorAction Stop
                Write-Log "  launched - record: does Notifications History show entries for the tests above? What does About report for agent status, operation mode, active policy and revision?"
            } catch { Write-Log "  launch failed: $($_.Exception.Message)" 'WARN' }
        }
    } else { Write-Log "  no DLP console found at the documented paths (the admin may not have enabled the client UI)" }

    # G5 - event log providers.
    Write-Log "G5: event log channels/providers mentioning DLP/McAfee/Trellix"
    try {
        $logs = Get-WinEvent -ListLog * -ErrorAction SilentlyContinue |
                Where-Object { $_.LogName -match 'DLP|McAfee|Trellix' -and $_.RecordCount -gt 0 }
        if ($logs) { foreach ($l in $logs) { Write-Log ("  log {0} ({1} records)" -f $l.LogName, $l.RecordCount) 'WARN' } }
        else { Write-Log "  (no matching non-empty log channels readable as a standard user)" }
    } catch { Write-Log "  event log enumeration failed: $($_.Exception.Message)" 'WARN' }

    # G6 - client artefacts, since no Trellix document states these paths.
    Write-Log "G6: client data directories readable as a standard user"
    foreach ($d in @('C:\ProgramData\McAfee\DLP', 'C:\ProgramData\Trellix')) {
        if (Test-Path $d) {
            Write-Log ("  {0} exists" -f $d) 'WARN'
            $items = Get-ChildItem $d -Recurse -Depth 2 -ErrorAction SilentlyContinue | Select-Object -First 25
            foreach ($i in $items) { Write-Log ("     {0}  {1} bytes  {2}" -f $i.FullName, $i.Length, $i.LastWriteTime) }
        } else { Write-Log ("  {0}: absent" -f $d) }
    }
}

function Invoke-PartH {
    Write-Log "--- PART H: is the local Trellix policy cache readable structure or opaque? ---" 'HEAD'
    Write-Log "Part G found the agent caches policy under C:\ProgramData\McAfee\DLP, world-readable, at"
    Write-Log "paths no Trellix document mentions. If .opg is parseable, it names the rules actually in"
    Write-Log "force - which answers the clipboard-rule and device-rule questions without waiting on ePO."
    Write-Log "Read-only. NOTE: any strings recovered are corporate policy content - treat this log as sensitive." 'WARN'

    $targets = @(
        "$env:ProgramData\McAfee\DLP\Agent\Policy\SCM.opg",
        "$env:ProgramData\McAfee\DLP\Temp\Policy\Configuration.opg",
        "$env:ProgramData\McAfee\DLP\Agent\S-1-5-*\NotificationList.opg",
        "$env:ProgramData\McAfee\DLP\Temp\TextExtractorDump*.log"
    )

    # Keywords chosen to answer the two open questions: does a clipboard rule exist, and does a
    # device rule name anything our composite device would match?
    $keywords = 'clipboard|removable|plug.?and.?play|device.?rule|justification|block|classif|' +
                'VID_|PID_|USB|COM\d|serial|composite|mass.?storage|whitelist|allowlist|deskhop|raspberry|pico'

    foreach ($pattern in $targets) {
        foreach ($f in (Get-ChildItem $pattern -ErrorAction SilentlyContinue)) {
            Write-Log ("--- {0} ({1} bytes, modified {2})" -f $f.FullName, $f.Length, $f.LastWriteTime)

            $bytes = $null
            try { $bytes = [IO.File]::ReadAllBytes($f.FullName) }
            catch { Write-Log ("    unreadable: {0}" -f $_.Exception.Message) 'WARN'; continue }
            if ($bytes.Length -eq 0) { Write-Log "    empty"; continue }

            $head = ($bytes[0..([Math]::Min(15, $bytes.Length - 1))] | ForEach-Object { $_.ToString('X2') }) -join ' '
            Write-Log ("    first 16 bytes: {0}" -f $head)

            # Shannon entropy over the byte histogram: ~8.0 means encrypted or compressed, and no
            # amount of string extraction will help. Below ~6 suggests recoverable structure.
            $hist = New-Object 'int[]' 256
            foreach ($b in $bytes) { $hist[$b]++ }
            $entropy = 0.0
            foreach ($c in $hist) {
                if ($c -gt 0) { $p = $c / $bytes.Length; $entropy -= $p * [Math]::Log($p, 2) }
            }
            $printable = @($bytes | Where-Object { ($_ -ge 32 -and $_ -le 126) -or $_ -eq 9 -or $_ -eq 10 -or $_ -eq 13 }).Count
            Write-Log ("    entropy = {0:N2} bits/byte, printable ASCII = {1:P1}" -f $entropy, ($printable / $bytes.Length))
            if ($entropy -gt 7.5) { Write-Log "    -> encrypted or compressed; string extraction will not help" 'WARN' }

            # ASCII and UTF-16LE runs of >= 6 chars, the usual two encodings in Windows blobs.
            $text = [Text.Encoding]::ASCII.GetString($bytes)
            $wide = [Text.Encoding]::Unicode.GetString($bytes)
            $strings = @()
            foreach ($src in @($text, $wide)) {
                $strings += [regex]::Matches($src, '[\x20-\x7E]{6,}') | ForEach-Object { $_.Value }
            }
            $strings = @($strings | Sort-Object -Unique)
            Write-Log ("    recovered {0} distinct printable runs of 6+ chars" -f $strings.Count)

            $hits = @($strings | Where-Object { $_ -match $keywords })
            if ($hits.Count -gt 0) {
                Write-Log ("    {0} run(s) match policy keywords - first 40:" -f $hits.Count) 'WARN'
                foreach ($h in ($hits | Select-Object -First 40)) {
                    $s = $h.Trim()
                    if ($s.Length -gt 160) { $s = $s.Substring(0, 160) + '...' }
                    Write-Log ("       $s")
                }
            } elseif ($strings.Count -gt 0) {
                Write-Log "    no policy keywords matched. Sample of what is in there:"
                foreach ($s in ($strings | Select-Object -First 15)) { Write-Log ("       $s") }
            }
        }
    }

    Write-Log "If SCM.opg is opaque, the remaining route is hdlpDiag.exe via the endpoint team (needs a"
    Write-Log "Help-Desk release code) - one conversation that answers the clipboard-rule, managed-class"
    Write-Log "and device-blocking questions authoritatively."
}

if ($Part -contains 'A') { Invoke-PartA }
if ($Part -contains 'B') { Invoke-PartB }
if ($Part -contains 'C') { Invoke-PartC }
if ($Part -contains 'D') { Invoke-PartD }
if ($Part -contains 'E') { Invoke-PartE }
if ($Part -contains 'F') { Invoke-PartF }
if ($Part -contains 'G') { Invoke-PartG }
if ($Part -contains 'H') { Invoke-PartH }

Write-Host ''
Write-Host "Log: $LogPath" -ForegroundColor Cyan
Write-Host 'Paste it back and I will finalise the issue #40 write-up.' -ForegroundColor Cyan
