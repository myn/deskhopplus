<#
.SYNOPSIS
    Empirical checks 1-3 from deskhopplus issue #40, run on the real work laptop.

.DESCRIPTION
    Each check retires an inference that the Windows research (issue #36) could not
    settle from documentation alone:

      1. Does SetCursorPos work while an ELEVATED window has focus? (UIPI question)
      2. Can a STANDARD USER register a logon-triggered scheduled task? (autostart)
      3. Is Purview / Endpoint DLP intercepting clipboard reads? (clipboard relay)

    Check 4 (usbser.sys binding) needs the CDC interface to exist and is not covered.

    RUN THIS UNELEVATED. That is the whole point of check 1 - the script must sit at
    medium integrity so UIPI has something to block. The script refuses to run check 1
    if it detects it is already elevated.

    Everything is read-only or self-cleaning: the scheduled task, Run-key value and
    Startup-folder file it creates are all deleted again before exit, and the clipboard
    is only read (plus one self-set baseline string).

.PARAMETER OutDir
    Where to write the log + JSON. Defaults to the desktop.

.PARAMETER NoInteractive
    Skip the two steps that need a human: focusing the elevated window is still
    automatic, but the "copy something from a corporate app" step in check 3 is skipped.

.PARAMETER SkipElevatedLaunch
    Do not spawn an elevated cmd.exe for check 1 (i.e. no UAC prompt). Instead the
    script waits for YOU to focus an already-running elevated window, and asks you to
    confirm it really is elevated.

.PARAMETER Only
    Run a subset, e.g. -Only 1,3

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\Check-WindowsConstraints.ps1
#>

[CmdletBinding()]
param(
    [string]$OutDir = [Environment]::GetFolderPath('Desktop'),
    [switch]$NoInteractive,
    [switch]$SkipElevatedLaunch,
    # NOTE: deliberately no ValidateSet - powershell.exe -File passes arguments as literal
    # strings, so "-Only 1,3" arrives as the single string "1,3" and a ValidateSet rejects it.
    [string[]]$Only = @('1', '2', '3')
)

$ErrorActionPreference = 'Continue'

$Only = @($Only | ForEach-Object { $_ -split '[,\s]+' } | Where-Object { $_ })
$bad = @($Only | Where-Object { $_ -notin @('1', '2', '3') })
if ($bad.Count -gt 0) {
    Write-Host "Unknown -Only value(s): $($bad -join ', '). Valid checks are 1, 2, 3 (e.g. -Only 1,3)." -ForegroundColor Red
    exit 1
}
$Only = @($Only | ForEach-Object { [int]$_ })
$script:Results = [ordered]@{}

# --------------------------------------------------------------------------------------
# Logging
# --------------------------------------------------------------------------------------

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$LogPath = Join-Path $OutDir "deskhopplus-win-checks-$stamp.log"
$JsonPath = Join-Path $OutDir "deskhopplus-win-checks-$stamp.json"

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

function Set-Result {
    param(
        [string]$Check,
        [ValidateSet('PASS', 'FAIL', 'INCONCLUSIVE', 'SKIPPED')][string]$Verdict,
        [string]$Detail,
        $Data
    )
    $script:Results[$Check] = [ordered]@{
        verdict = $Verdict
        detail  = $Detail
        data    = $Data
    }
    $lvl = switch ($Verdict) { 'PASS' { 'PASS' } 'FAIL' { 'FAIL' } default { 'WARN' } }
    Write-Log "VERDICT [$Check] = $Verdict - $Detail" $lvl
}

# --------------------------------------------------------------------------------------
# P/Invoke
# --------------------------------------------------------------------------------------

$sig = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class W32 {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }

    [DllImport("user32.dll", SetLastError = true)] public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", SetLastError = true)] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool SetForegroundWindow(IntPtr hWnd);

    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT {
        public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo;
    }
    // MOUSEINPUT is the largest member of the INPUT union, so type + MOUSEINPUT is the
    // full 40 bytes (x64) / 28 bytes (x86) SendInput expects for cbSize.
    [StructLayout(LayoutKind.Sequential)] public struct INPUT {
        public uint type; public MOUSEINPUT mi;
    }
    [DllImport("user32.dll", SetLastError = true)] public static extern uint SendInput(uint n, INPUT[] pInputs, int cbSize);

    // Clipboard
    [DllImport("user32.dll", SetLastError = true)] public static extern bool OpenClipboard(IntPtr hWndNewOwner);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool CloseClipboard();
    [DllImport("user32.dll", SetLastError = true)] public static extern IntPtr GetClipboardData(uint uFormat);
    [DllImport("user32.dll", SetLastError = true)] public static extern uint EnumClipboardFormats(uint format);
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern int GetClipboardFormatNameW(uint format, StringBuilder lpsz, int cch);
    [DllImport("user32.dll", SetLastError = true)] public static extern IntPtr GetClipboardOwner();
    [DllImport("user32.dll", SetLastError = true)] public static extern uint GetClipboardSequenceNumber();
    [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr GlobalLock(IntPtr hMem);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool GlobalUnlock(IntPtr hMem);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern UIntPtr GlobalSize(IntPtr hMem);
}
'@

$langMode = $ExecutionContext.SessionState.LanguageMode
Write-Log "PowerShell language mode: $langMode"
try {
    Add-Type -TypeDefinition $sig -Language CSharp -ErrorAction Stop
} catch {
    Write-Log "FATAL: could not compile the P/Invoke shim: $($_.Exception.Message)" 'FAIL'
    if ("$langMode" -ne 'FullLanguage') {
        Write-Log "Cause is almost certainly ConstrainedLanguage mode (WDAC/AppLocker policy). That is itself a finding worth recording in issue #40: checks 1 and 3 cannot be done from PowerShell on this machine and need a small compiled test EXE instead." 'WARN'
    }
    if (($Only | Where-Object { $_ -ne 2 }).Count -gt 0) {
        Write-Log "Checks 1 and 3 need the shim; aborting. Re-run with -Only 2 to at least get the autostart answer." 'FAIL'
        exit 2
    }
    Write-Log "Only check 2 was requested, which does not need the shim - continuing." 'WARN'
}

# --------------------------------------------------------------------------------------
# Environment banner
# --------------------------------------------------------------------------------------

$id        = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($id)
$IsElevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$IsAdminMember = $false
try {
    $adminSid = New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')
    $IsAdminMember = [bool]($id.Groups | Where-Object { $_.Value -eq $adminSid.Value })
} catch { }

Write-Log "=== deskhopplus issue #40 Windows checks ===" 'HEAD'
Write-Log ("Host            : {0}" -f $env:COMPUTERNAME)
Write-Log ("User            : {0}" -f $id.Name)
Write-Log ("PS version      : {0} ({1})" -f $PSVersionTable.PSVersion, $PSVersionTable.PSEdition)
Write-Log ("Apartment state : {0}" -f [Threading.Thread]::CurrentThread.GetApartmentState())
Write-Log ("Process elevated: {0}" -f $IsElevated)
Write-Log ("In Administrators group (even if not elevated): {0}" -f $IsAdminMember)
try {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    Write-Log ("OS              : {0} build {1}" -f $os.Caption, $os.BuildNumber)
} catch { Write-Log "OS query failed: $($_.Exception.Message)" 'WARN' }
try {
    $cs = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop
    Write-Log ("Domain / joined : {0} (part of domain: {1})" -f $cs.Domain, $cs.PartOfDomain)
} catch { }
Write-Log ("Log file        : {0}" -f $LogPath)

if ($IsElevated) {
    Write-Log "This process IS elevated. Check 1 is meaningless from here - re-run unelevated." 'WARN'
}

# --------------------------------------------------------------------------------------
# CHECK 1 - SetCursorPos with an elevated window focused
# --------------------------------------------------------------------------------------

function Get-ForegroundInfo {
    $h = [W32]::GetForegroundWindow()
    $procId = [uint32]0
    [void][W32]::GetWindowThreadProcessId($h, [ref]$procId)
    $sb = New-Object Text.StringBuilder 512
    [void][W32]::GetWindowTextW($h, $sb, $sb.Capacity)
    $name = '<unknown>'
    try { $name = (Get-Process -Id $procId -ErrorAction Stop).ProcessName } catch { }
    [pscustomobject]@{ Handle = $h; ProcessId = [int]$procId; ProcessName = $name; Title = $sb.ToString() }
}

function Test-CursorMove {
    param([string]$Label)

    $before = New-Object W32+POINT
    $okGet = [W32]::GetCursorPos([ref]$before)
    if (-not $okGet) {
        return [pscustomobject]@{ Label = $Label; Method = 'SetCursorPos'; Moved = $false; Error = "GetCursorPos failed ($([Runtime.InteropServices.Marshal]::GetLastWin32Error()))" }
    }

    $vx = [W32]::GetSystemMetrics(76)   # SM_XVIRTUALSCREEN
    $vy = [W32]::GetSystemMetrics(77)   # SM_YVIRTUALSCREEN
    $vw = [W32]::GetSystemMetrics(78)   # SM_CXVIRTUALSCREEN
    $vh = [W32]::GetSystemMetrics(79)   # SM_CYVIRTUALSCREEN

    $targetX = [Math]::Min($vx + $vw - 2, [Math]::Max($vx + 1, $before.X + 137))
    $targetY = [Math]::Min($vy + $vh - 2, [Math]::Max($vy + 1, $before.Y + 89))
    if ($targetX -eq $before.X -and $targetY -eq $before.Y) { $targetX = $vx + 10; $targetY = $vy + 10 }

    # --- SetCursorPos ---
    $ok  = [W32]::SetCursorPos($targetX, $targetY)
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Start-Sleep -Milliseconds 200
    $after = New-Object W32+POINT
    [void][W32]::GetCursorPos([ref]$after)
    $movedSCP = ([Math]::Abs($after.X - $targetX) -le 2 -and [Math]::Abs($after.Y - $targetY) -le 2)

    Write-Log ("[{0}] SetCursorPos({1},{2}) returned {3} (GetLastError={4}); cursor {5},{6} -> {7},{8}; moved={9}" -f `
        $Label, $targetX, $targetY, $ok, $err, $before.X, $before.Y, $after.X, $after.Y, $movedSCP)

    # --- SendInput contrast (documented as UIPI-subject; expected to fail when blocked) ---
    $siTargetX = [Math]::Max($vx + 1, $before.X + 40)
    $siTargetY = [Math]::Max($vy + 1, $before.Y + 40)
    $absX = [int](($siTargetX - $vx) * 65535 / [Math]::Max(1, $vw - 1))
    $absY = [int](($siTargetY - $vy) * 65535 / [Math]::Max(1, $vh - 1))

    $inp = New-Object W32+INPUT
    $inp.type = 0                       # INPUT_MOUSE
    $mi = New-Object W32+MOUSEINPUT
    $mi.dx = $absX; $mi.dy = $absY
    $mi.dwFlags = 0x8001 -bor 0x4000    # MOVE | ABSOLUTE | VIRTUALDESK
    $inp.mi = $mi
    $arr = New-Object 'W32+INPUT[]' 1
    $arr[0] = $inp
    $sent = [W32]::SendInput(1, $arr, [Runtime.InteropServices.Marshal]::SizeOf([type]'W32+INPUT'))
    $siErr = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Start-Sleep -Milliseconds 200
    $afterSi = New-Object W32+POINT
    [void][W32]::GetCursorPos([ref]$afterSi)
    $movedSI = ([Math]::Abs($afterSi.X - $siTargetX) -le 4 -and [Math]::Abs($afterSi.Y - $siTargetY) -le 4)

    Write-Log ("[{0}] SendInput(abs move) inserted {1} event(s) (GetLastError={2}); cursor now {3},{4}; moved={5}" -f `
        $Label, $sent, $siErr, $afterSi.X, $afterSi.Y, $movedSI)

    # restore
    [void][W32]::SetCursorPos($before.X, $before.Y)

    [pscustomobject]@{
        Label              = $Label
        SetCursorPosReturn = $ok
        SetCursorPosError  = $err
        SetCursorPosMoved  = $movedSCP
        SendInputInserted  = $sent
        SendInputError     = $siErr
        SendInputMoved     = $movedSI
    }
}

function Invoke-Check1 {
    Write-Log "--- CHECK 1: SetCursorPos while an elevated window has focus ---" 'HEAD'

    if ($IsElevated) {
        Set-Result -Check '1-setcursorpos-over-elevated' -Verdict 'SKIPPED' `
            -Detail 'Script itself is elevated; UIPI cannot apply. Re-run from an unelevated shell.'
        return
    }

    # Baseline: same calls with an ordinary (medium-IL) window focused.
    Write-Log "Baseline: launching unelevated notepad to own the foreground..."
    $np = $null
    try { $np = Start-Process notepad.exe -PassThru -ErrorAction Stop } catch { Write-Log "notepad launch failed: $($_.Exception.Message)" 'WARN' }
    if ($np) {
        for ($i = 0; $i -lt 20; $i++) {
            Start-Sleep -Milliseconds 250
            try { $np.Refresh() } catch { }
            if ($np.MainWindowHandle -ne [IntPtr]::Zero) { break }
        }
        [void][W32]::SetForegroundWindow($np.MainWindowHandle)
        Start-Sleep -Milliseconds 700
    }
    $fg = Get-ForegroundInfo
    Write-Log ("Baseline foreground: pid={0} {1} '{2}'" -f $fg.ProcessId, $fg.ProcessName, $fg.Title)
    $baseline = Test-CursorMove -Label 'baseline-unelevated'
    if ($np) { try { $np.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 300; if (-not $np.HasExited) { $np.Kill() } } catch { } }

    if (-not $baseline.SetCursorPosMoved) {
        Set-Result -Check '1-setcursorpos-over-elevated' -Verdict 'INCONCLUSIVE' `
            -Detail 'SetCursorPos did not even work against an unelevated window - something else (RDP session, no interactive desktop, remote control software) is interfering.' `
            -Data @{ baseline = $baseline }
        return
    }

    # Now the real thing: an elevated window in the foreground.
    $elevProc = $null
    if ($SkipElevatedLaunch) {
        if ($NoInteractive) {
            Set-Result -Check '1-setcursorpos-over-elevated' -Verdict 'SKIPPED' -Detail '-SkipElevatedLaunch with -NoInteractive leaves no way to get an elevated window focused.'
            return
        }
        Write-Host ''
        Write-Host 'Focus an ALREADY-RUNNING elevated window now (e.g. Task Manager started as admin,' -ForegroundColor Yellow
        Write-Host 'or an admin PowerShell). You have 15 seconds. Do NOT touch the mouse after that.' -ForegroundColor Yellow
        Start-Sleep -Seconds 15
    } else {
        Write-Log "Launching an elevated cmd.exe - APPROVE THE UAC PROMPT, then do not touch mouse/keyboard."
        try {
            $elevProc = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" `
                -ArgumentList '/c title DESKHOPPLUS-ELEVATED-PROBE & echo Elevated probe window, closes itself. & timeout /t 30 /nobreak' `
                -Verb RunAs -PassThru -ErrorAction Stop
        } catch {
            Set-Result -Check '1-setcursorpos-over-elevated' -Verdict 'INCONCLUSIVE' `
                -Detail "Could not launch an elevated process (UAC declined, or this account cannot elevate at all): $($_.Exception.Message). Re-run with -SkipElevatedLaunch and focus an existing elevated window." `
                -Data @{ baseline = $baseline }
            return
        }

        # Wait for it to own the foreground.
        $deadline = (Get-Date).AddSeconds(20)
        do {
            Start-Sleep -Milliseconds 500
            $fg = Get-ForegroundInfo
        } while ($fg.ProcessId -ne $elevProc.Id -and (Get-Date) -lt $deadline)

        if ($fg.ProcessId -ne $elevProc.Id) {
            Write-Log ("Elevated probe never reached the foreground (foreground is pid={0} {1}). Result would be meaningless." -f $fg.ProcessId, $fg.ProcessName) 'WARN'
            Set-Result -Check '1-setcursorpos-over-elevated' -Verdict 'INCONCLUSIVE' `
                -Detail 'Elevated window never took the foreground.' -Data @{ baseline = $baseline; foreground = $fg }
            return
        }
    }

    $fg = Get-ForegroundInfo
    Write-Log ("Foreground during test: pid={0} {1} '{2}'" -f $fg.ProcessId, $fg.ProcessName, $fg.Title)

    # Confirm the foreground process really is higher-integrity: an unelevated caller
    # cannot open it for VM_READ / QUERY_INFORMATION, and Get-Process -Module fails.
    $elevatedEvidence = 'unknown'
    try {
        $p = Get-Process -Id $fg.ProcessId -ErrorAction Stop
        $null = $p.Path       # throws Access Denied for a higher-IL process from medium IL
        $elevatedEvidence = 'readable (process path accessible => probably NOT elevated)'
    } catch {
        $elevatedEvidence = "path unreadable => higher integrity than us ($($_.Exception.Message.Split([char]10)[0]))"
    }
    Write-Log ("Integrity evidence for foreground process: {0}" -f $elevatedEvidence)

    $elevTest = Test-CursorMove -Label 'elevated-focused'

    if ($elevProc) { try { if (-not $elevProc.HasExited) { $elevProc.CloseMainWindow() | Out-Null } } catch { } }

    if ($elevTest.SetCursorPosMoved) {
        Set-Result -Check '1-setcursorpos-over-elevated' -Verdict 'PASS' `
            -Detail ("SetCursorPos moved the cursor while an elevated window had focus. SendInput moved={0} (contrast)." -f $elevTest.SendInputMoved) `
            -Data @{ baseline = $baseline; elevated = $elevTest; foreground = $fg; integrityEvidence = $elevatedEvidence }
    } else {
        Set-Result -Check '1-setcursorpos-over-elevated' -Verdict 'FAIL' `
            -Detail ("SetCursorPos did NOT move the cursor with an elevated window focused (return={0}, GetLastError={1}). Cursor placement is blocked whenever an admin window is focused." -f $elevTest.SetCursorPosReturn, $elevTest.SetCursorPosError) `
            -Data @{ baseline = $baseline; elevated = $elevTest; foreground = $fg; integrityEvidence = $elevatedEvidence }
    }
}

# --------------------------------------------------------------------------------------
# CHECK 2 - logon-triggered scheduled task as a standard user (+ Run key / Startup folder)
# --------------------------------------------------------------------------------------

function Invoke-Check2 {
    Write-Log "--- CHECK 2: autostart mechanisms available to this account ---" 'HEAD'

    if ($IsElevated) {
        Write-Log "NOTE: running elevated - a task that registers here may still be refused unelevated." 'WARN'
    }

    $taskName = 'DeskHopPlusProbe_Logon'
    $data = [ordered]@{}

    # --- (a) schtasks.exe /SC ONLOGON ---
    $tr = '"%SystemRoot%\System32\cmd.exe" /c exit'
    $out = & schtasks.exe /Create /TN $taskName /TR $tr /SC ONLOGON /F 2>&1
    $rc  = $LASTEXITCODE
    $outText = ($out | Out-String).Trim()
    Write-Log ("schtasks /Create /SC ONLOGON -> exit {0}: {1}" -f $rc, $outText)
    $schtasksOk = ($rc -eq 0)
    $data['schtasks'] = @{ exitCode = $rc; output = $outText }

    if ($schtasksOk) {
        $q = & schtasks.exe /Query /TN $taskName /V /FO LIST 2>&1
        $qText = ($q | Out-String).Trim()
        Write-Log "schtasks /Query result:"
        foreach ($l in ($qText -split "`r?`n")) {
            if ($l -match '^(TaskName|Status|Logon Mode|Run As User|Scheduled Task State|Schedule Type|Task To Run):') { Write-Log ("    " + $l.Trim()) }
        }
        $data['schtasksQuery'] = $qText
    }

    # --- (b) Register-ScheduledTask (Task Scheduler 2.0 COM path - can differ from schtasks) ---
    $psTaskName = 'DeskHopPlusProbe_Logon_PS'
    $psOk = $false
    try {
        $action  = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\cmd.exe" -Argument '/c exit'
        $trigger = New-ScheduledTaskTrigger -AtLogOn -User $id.Name
        Register-ScheduledTask -TaskName $psTaskName -Action $action -Trigger $trigger -Force -ErrorAction Stop | Out-Null
        $psOk = $true
        Write-Log "Register-ScheduledTask -AtLogOn -> OK"
    } catch {
        Write-Log ("Register-ScheduledTask -AtLogOn -> FAILED: {0}" -f $_.Exception.Message) 'WARN'
        $data['registerScheduledTaskError'] = $_.Exception.Message
    }
    $data['registerScheduledTask'] = $psOk

    # cleanup both
    if ($schtasksOk) {
        $d = & schtasks.exe /Delete /TN $taskName /F 2>&1
        Write-Log ("cleanup: schtasks /Delete -> exit {0} {1}" -f $LASTEXITCODE, (($d | Out-String).Trim()))
    }
    if ($psOk) {
        try { Unregister-ScheduledTask -TaskName $psTaskName -Confirm:$false -ErrorAction Stop; Write-Log "cleanup: Unregister-ScheduledTask -> OK" }
        catch { Write-Log "cleanup: Unregister-ScheduledTask failed: $($_.Exception.Message)" 'WARN' }
    }

    # --- (c) Fallback 1: HKCU Run key writable? ---
    $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    $runOk = $false
    try {
        New-ItemProperty -Path $runKey -Name 'DeskHopPlusProbe' -Value 'cmd.exe /c exit' -PropertyType String -Force -ErrorAction Stop | Out-Null
        $runOk = $null -ne (Get-ItemProperty -Path $runKey -Name 'DeskHopPlusProbe' -ErrorAction SilentlyContinue)
        Remove-ItemProperty -Path $runKey -Name 'DeskHopPlusProbe' -Force -ErrorAction SilentlyContinue
        Write-Log "HKCU Run key: writable = $runOk (probe value removed)"
    } catch {
        Write-Log "HKCU Run key write FAILED: $($_.Exception.Message)" 'WARN'
        $data['runKeyError'] = $_.Exception.Message
    }
    $data['hkcuRunKeyWritable'] = $runOk

    # --- (d) Fallback 2: Startup folder writable? ---
    $startup = [Environment]::GetFolderPath('Startup')
    $startupOk = $false
    try {
        $probe = Join-Path $startup 'deskhopplus-probe.txt'
        Set-Content -Path $probe -Value 'probe' -ErrorAction Stop
        $startupOk = Test-Path $probe
        Remove-Item $probe -Force -ErrorAction SilentlyContinue
        Write-Log "Startup folder ($startup): writable = $startupOk (probe file removed)"
    } catch {
        Write-Log "Startup folder write FAILED: $($_.Exception.Message)" 'WARN'
        $data['startupFolderError'] = $_.Exception.Message
    }
    $data['startupFolderWritable'] = $startupOk

    # --- (e) Relevant policy visibility ---
    foreach ($k in @(
        'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Task Scheduler',
        'HKCU:\Software\Policies\Microsoft\Windows\Task Scheduler',
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\Explorer',
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer'
    )) {
        if (Test-Path $k) {
            $props = Get-ItemProperty -Path $k -ErrorAction SilentlyContinue
            Write-Log ("policy {0}: {1}" -f $k, (($props.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join '; '))
        }
    }

    if ($schtasksOk -or $psOk) {
        Set-Result -Check '2-logon-scheduled-task' -Verdict 'PASS' `
            -Detail ("Logon-trigger task registered as this user (schtasks={0}, Register-ScheduledTask={1}). Run key writable={2}, Startup folder writable={3}." -f $schtasksOk, $psOk, $runOk, $startupOk) `
            -Data $data
    } else {
        Set-Result -Check '2-logon-scheduled-task' -Verdict 'FAIL' `
            -Detail ("Policy refuses a logon-trigger task for this account. Fallbacks: Run key writable={0}, Startup folder writable={1} - both documented as possibly delayed, so the handshake must tolerate a late helper." -f $runOk, $startupOk) `
            -Data $data
    }
}

# --------------------------------------------------------------------------------------
# CHECK 3 - Purview / Endpoint DLP and clipboard reads
# --------------------------------------------------------------------------------------

function Get-ClipboardSnapshot {
    param([string]$Label)

    $result = [ordered]@{
        label       = $Label
        opened      = $false
        openError   = 0
        formats     = @()
        textLength  = $null
        textPreview = $null
        getDataError = $null
        sequence    = [W32]::GetClipboardSequenceNumber()
    }

    # OpenClipboard can legitimately fail transiently if another app holds it; retry.
    for ($i = 0; $i -lt 10; $i++) {
        if ([W32]::OpenClipboard([IntPtr]::Zero)) { $result.opened = $true; break }
        $result.openError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Start-Sleep -Milliseconds 150
    }

    if (-not $result.opened) {
        Write-Log ("[{0}] OpenClipboard failed after 10 tries, GetLastError={1}" -f $Label, $result.openError) 'WARN'
        return [pscustomobject]$result
    }

    try {
        $fmts = @()
        $f = 0
        while (($f = [W32]::EnumClipboardFormats($f)) -ne 0) {
            $sb = New-Object Text.StringBuilder 256
            $n = [W32]::GetClipboardFormatNameW($f, $sb, $sb.Capacity)
            $name = if ($n -gt 0) { $sb.ToString() } else {
                switch ($f) {
                    1  { 'CF_TEXT' } 2 { 'CF_BITMAP' } 3 { 'CF_METAFILEPICT' } 7 { 'CF_OEMTEXT' }
                    8  { 'CF_DIB' } 13 { 'CF_UNICODETEXT' } 14 { 'CF_ENHMETAFILE' } 15 { 'CF_HDROP' }
                    16 { 'CF_LOCALE' } 17 { 'CF_DIBV5' }
                    default { "<standard format $f>" }
                }
            }
            $fmts += [pscustomobject]@{ Id = $f; Name = $name }
        }
        $result.formats = $fmts
        Write-Log ("[{0}] {1} clipboard format(s): {2}" -f $Label, $fmts.Count, (($fmts | ForEach-Object { "$($_.Id)=$($_.Name)" }) -join ', '))

        $h = [W32]::GetClipboardData(13)  # CF_UNICODETEXT
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if ($h -eq [IntPtr]::Zero) {
            $result.getDataError = $err
            Write-Log ("[{0}] GetClipboardData(CF_UNICODETEXT) returned NULL, GetLastError={1}" -f $Label, $err) 'WARN'
        } else {
            $p = [W32]::GlobalLock($h)
            if ($p -ne [IntPtr]::Zero) {
                $s = [Runtime.InteropServices.Marshal]::PtrToStringUni($p)
                [void][W32]::GlobalUnlock($h)
                $result.textLength = $s.Length
                $preview = if ($s.Length -gt 60) { $s.Substring(0, 60) + '...' } else { $s }
                $result.textPreview = ($preview -replace '[\r\n]+', ' ')
                Write-Log ("[{0}] GetClipboardData OK: {1} chars, preview: {2}" -f $Label, $s.Length, $result.textPreview)
            } else {
                $result.getDataError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                Write-Log ("[{0}] GlobalLock failed, GetLastError={1}" -f $Label, $result.getDataError) 'WARN'
            }
        }
    } finally {
        [void][W32]::CloseClipboard()
    }

    [pscustomobject]$result
}

function Invoke-Check3 {
    Write-Log "--- CHECK 3: Purview / Endpoint DLP and clipboard reads ---" 'HEAD'
    $data = [ordered]@{}

    # --- (a) Is Defender for Endpoint / Endpoint DLP even present? ---
    $svcNames = @('Sense', 'WinDefend', 'MDCoreSvc', 'MsSecFlt', 'DiagTrack', 'MDESensor')
    $svcs = foreach ($n in $svcNames) {
        $s = Get-Service -Name $n -ErrorAction SilentlyContinue
        if ($s) { [pscustomobject]@{ Name = $s.Name; Display = $s.DisplayName; Status = "$($s.Status)" } }
    }
    foreach ($s in $svcs) { Write-Log ("service {0,-12} {1,-10} ({2})" -f $s.Name, $s.Status, $s.Display) }
    $data['services'] = $svcs

    $procNames = @('MsSense', 'SenseIR', 'SenseCE', 'SenseNdr', 'MpDlpService', 'MsMpEng', 'MSIP.App.Sync', 'msip')
    $procs = foreach ($n in $procNames) {
        $p = Get-Process -Name $n -ErrorAction SilentlyContinue
        if ($p) { [pscustomobject]@{ Name = $n; Count = @($p).Count } }
    }
    foreach ($p in $procs) { Write-Log ("process {0} running (x{1})" -f $p.Name, $p.Count) }
    $data['processes'] = $procs

    $files = @(
        "$env:ProgramFiles\Windows Defender Advanced Threat Protection\MsSense.exe",
        "$env:ProgramFiles\Windows Defender Advanced Threat Protection\Classification\MpDlpService.exe",
        "$env:ProgramData\Microsoft\Windows Defender Advanced Threat Protection\DataLossPrevention",
        "$env:ProgramFiles\Microsoft Azure Information Protection",
        "$env:ProgramFiles (x86)\Microsoft Azure Information Protection"
    )
    $filesFound = foreach ($f in $files) { if (Test-Path $f) { Write-Log ("present: {0}" -f $f); $f } }
    $data['artifacts'] = @($filesFound)

    $regKeys = @(
        'HKLM:\SOFTWARE\Microsoft\Windows Advanced Threat Protection',
        'HKLM:\SOFTWARE\Microsoft\Windows Advanced Threat Protection\Status',
        'HKLM:\SOFTWARE\Policies\Microsoft\Windows Advanced Threat Protection',
        'HKLM:\SOFTWARE\Microsoft\Windows Defender\Miscellaneous Configuration',
        'HKLM:\SOFTWARE\Microsoft\MSIP',
        'HKCU:\SOFTWARE\Microsoft\MSIP'
    )
    $regFound = [ordered]@{}
    foreach ($k in $regKeys) {
        if (Test-Path $k) {
            $props = Get-ItemProperty -Path $k -ErrorAction SilentlyContinue
            $flat = ($props.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join '; '
            Write-Log ("reg {0}: {1}" -f $k, $flat)
            $regFound[$k] = $flat
        }
    }
    $data['registry'] = $regFound

    $onboarded = $false
    try {
        $st = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Advanced Threat Protection\Status' -ErrorAction Stop
        $onboarded = ($st.OnboardingState -eq 1)
    } catch { }
    $data['mdeOnboarded'] = $onboarded
    Write-Log ("Defender for Endpoint onboarding state: {0}" -f $(if ($onboarded) { 'ONBOARDED' } else { 'not detected / not readable' }))

    $dlpPresent = ($procs.Name -contains 'MpDlpService') -or
                  ($filesFound -match 'MpDlpService|DataLossPrevention') -or
                  ($regFound.Keys -match 'Advanced Threat Protection')
    Write-Log ("Endpoint DLP components detected: {0}" -f [bool]$dlpPresent)
    $data['dlpComponentsDetected'] = [bool]$dlpPresent

    # --- (b) Baseline clipboard read: we set it ourselves, we read it back ---
    $marker = "deskhopplus-probe-$((Get-Random))"
    $setOk = $true
    try { Set-Clipboard -Value $marker -ErrorAction Stop } catch { $setOk = $false; Write-Log "Set-Clipboard failed: $($_.Exception.Message)" 'WARN' }
    Start-Sleep -Milliseconds 300
    $baseSnap = Get-ClipboardSnapshot -Label 'baseline-self-set'
    $baselineOk = ($baseSnap.textPreview -eq $marker)
    Write-Log ("Baseline clipboard round-trip (set by us, read via Win32 GetClipboardData): {0}" -f $baselineOk)
    $data['baselineClipboard'] = @{ setOk = $setOk; readBack = $baselineOk; snapshot = $baseSnap }

    # --- (c) The real question: read content copied out of a CORPORATE app ---
    if ($NoInteractive) {
        Write-Log "Interactive corporate-copy step skipped (-NoInteractive)." 'WARN'
        $verdict = if ($baselineOk) { 'INCONCLUSIVE' } else { 'FAIL' }
        Set-Result -Check '3-purview-dlp-clipboard' -Verdict $verdict `
            -Detail ("DLP components detected={0}; own-clipboard read works={1}; corporate-content step not run." -f [bool]$dlpPresent, $baselineOk) `
            -Data $data
        return
    }

    Write-Host ''
    Write-Host '  ACTION NEEDED -------------------------------------------------------' -ForegroundColor Yellow
    Write-Host '  Switch to a CORPORATE application - Outlook, Teams, a SharePoint or'   -ForegroundColor Yellow
    Write-Host '  OneDrive document, ideally one with a sensitivity label applied -'      -ForegroundColor Yellow
    Write-Host '  select some text and press Ctrl+C. Then come back here and press Enter.' -ForegroundColor Yellow
    Write-Host '  ---------------------------------------------------------------------' -ForegroundColor Yellow
    Read-Host '  Press Enter once you have copied text from a corporate app'

    $corpSnap = Get-ClipboardSnapshot -Label 'corporate-content'
    $data['corporateClipboard'] = $corpSnap

    $labelFormats = @($corpSnap.formats | Where-Object { $_.Name -match 'MSIP|Purview|Label|Classification|DLP' })
    if ($labelFormats.Count -gt 0) {
        Write-Log ("Sensitivity/classification clipboard formats present: {0}" -f (($labelFormats | ForEach-Object { $_.Name }) -join ', '))
    } else {
        Write-Log "No MSIP/Purview classification clipboard formats seen - content may be unlabeled, which weakens the test." 'WARN'
    }
    $data['classificationFormats'] = $labelFormats

    $changed  = ($corpSnap.sequence -ne $baseSnap.sequence) -or ($corpSnap.textPreview -ne $baseSnap.textPreview)
    $readable = ($corpSnap.textLength -ne $null -and $corpSnap.textLength -gt 0)

    if (-not $changed) {
        Set-Result -Check '3-purview-dlp-clipboard' -Verdict 'INCONCLUSIVE' `
            -Detail 'Clipboard did not change - the copy did not happen, or was blocked at the source. Re-run and confirm the Ctrl+C worked.' -Data $data
    } elseif ($readable) {
        Set-Result -Check '3-purview-dlp-clipboard' -Verdict 'PASS' `
            -Detail ("A third-party unelevated process read {0} chars of corporate-app clipboard content via GetClipboardData. DLP components detected={1}; classification formats seen={2}. NOTE: this only proves reads are allowed for the content actually tested - copy-to-removable-USB restrictions are a separate policy and are NOT tested here." -f $corpSnap.textLength, [bool]$dlpPresent, $labelFormats.Count) `
            -Data $data
    } else {
        Set-Result -Check '3-purview-dlp-clipboard' -Verdict 'FAIL' `
            -Detail ("Clipboard changed but GetClipboardData returned nothing (formats: {0}; GetLastError={1}). Consistent with DLP interception of third-party clipboard reads." -f (($corpSnap.formats | ForEach-Object { $_.Name }) -join ', '), $corpSnap.getDataError) `
            -Data $data
    }
}

# --------------------------------------------------------------------------------------
# Run
# --------------------------------------------------------------------------------------

if ($Only -contains 1) { Invoke-Check1 } else { Set-Result -Check '1-setcursorpos-over-elevated' -Verdict 'SKIPPED' -Detail 'not selected' }
if ($Only -contains 2) { Invoke-Check2 } else { Set-Result -Check '2-logon-scheduled-task' -Verdict 'SKIPPED' -Detail 'not selected' }
if ($Only -contains 3) { Invoke-Check3 } else { Set-Result -Check '3-purview-dlp-clipboard' -Verdict 'SKIPPED' -Detail 'not selected' }

Write-Log "=== SUMMARY ===" 'HEAD'
foreach ($k in $script:Results.Keys) {
    $r = $script:Results[$k]
    $lvl = switch ($r.verdict) { 'PASS' { 'PASS' } 'FAIL' { 'FAIL' } default { 'WARN' } }
    Write-Log ("{0,-32} {1,-13} {2}" -f $k, $r.verdict, $r.detail) $lvl
}
Write-Log "Check 4 (usbser.sys binding under device-installation policy) is not covered - it needs the CDC interface to exist."

$payload = [ordered]@{
    generated = (Get-Date).ToString('o')
    host      = $env:COMPUTERNAME
    user      = $id.Name
    elevated  = $IsElevated
    results   = $script:Results
}
$payload | ConvertTo-Json -Depth 8 | Set-Content -Path $JsonPath -Encoding UTF8

Write-Host ''
Write-Host "Log : $LogPath"  -ForegroundColor Cyan
Write-Host "JSON: $JsonPath" -ForegroundColor Cyan
Write-Host 'Paste the log into issue #40.' -ForegroundColor Cyan
