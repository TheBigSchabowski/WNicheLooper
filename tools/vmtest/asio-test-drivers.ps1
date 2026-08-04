<#
.SYNOPSIS
    Registers fake ASIO driver entries that exercise every branch of the
    driver enumeration in native/AsioBackend.cpp — no hardware, no real driver.

.DESCRIPTION
    Each entry is a HKLM\SOFTWARE\ASIO key plus (usually) a COM registration
    whose InprocServer32 points at an existing system DLL. That is enough for
    the enumeration, which only reads the registry and the DLL's PE header.

    These are LIST-ONLY dummies. Picking one and pressing Start fails cleanly
    ("CoCreateInstance failed for ASIO …") because ole32.dll does not serve our
    made-up CLSIDs — that is expected and is itself a useful test of the error
    path. For a driver that actually runs, build the SDK's sample driver
    (third_party/asiosdk/driver/asiosample) or install FlexASIO / ASIO4ALL.

    Run from an ELEVATED PowerShell (writes to HKLM). Undo with -Remove.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\asio-test-drivers.ps1
    powershell -ExecutionPolicy Bypass -File .\asio-test-drivers.ps1 -Remove
#>
param([switch]$Remove)

$ErrorActionPreference = 'Stop'

# Dll64 / Dll32: existing system DLLs, used purely as PE-header specimens.
$Dll64 = 'C:\Windows\System32\ole32.dll'
$Dll32 = 'C:\Windows\SysWOW64\ole32.dll'

$entries = @(
    @{ Name     = 'NLTest 01 ok64'
       Clsid    = '{9E7A1C00-0001-4E00-9B00-000000000001}'
       AsioRoot = 'HKLM:\SOFTWARE\ASIO'
       ComRoot  = 'HKLM:\SOFTWARE\Classes\CLSID'
       Dll      = $Dll64
       Expect   = 'OK       — the baseline: 64-bit server, everything normal' }

    @{ Name     = 'NLTest 02 wrongbits'
       Clsid    = '{9E7A1C00-0002-4E00-9B00-000000000002}'
       AsioRoot = 'HKLM:\SOFTWARE\ASIO'
       ComRoot  = 'HKLM:\SOFTWARE\Classes\CLSID'
       Dll      = $Dll32
       Expect   = 'SKIPPED  — wrong bitness (machine=0x014c), a 64-bit host cannot load it' }

    @{ Name     = 'NLTest 03 deaddll'
       Clsid    = '{9E7A1C00-0003-4E00-9B00-000000000003}'
       AsioRoot = 'HKLM:\SOFTWARE\ASIO'
       ComRoot  = 'HKLM:\SOFTWARE\Classes\CLSID'
       Dll      = 'C:\NicheLooperTest\uninstalled-driver.dll'
       Expect   = 'SKIPPED  — driver DLL missing, i.e. the leftover of an uninstall' }

    @{ Name     = 'NLTest 04 noserver'
       Clsid    = '{9E7A1C00-0004-4E00-9B00-000000000004}'
       AsioRoot = 'HKLM:\SOFTWARE\ASIO'
       ComRoot  = $null   # ASIO key only, no COM registration at all
       Dll      = $null
       Expect   = 'SKIPPED  — no COM server registered' }

    @{ Name     = 'NLTest 05 wow64only'
       Clsid    = '{9E7A1C00-0005-4E00-9B00-000000000005}'
       AsioRoot = 'HKLM:\SOFTWARE\WOW6432Node\ASIO'
       ComRoot  = 'HKLM:\SOFTWARE\Classes\WOW6432Node\CLSID'
       Dll      = $Dll64
       Expect   = 'OK       — only in the 32-bit registry view; INVISIBLE before v1.1.1' }

    @{ Name     = 'NLTest 06 peruser'
       Clsid    = '{9E7A1C00-0006-4E00-9B00-000000000006}'
       AsioRoot = 'HKLM:\SOFTWARE\ASIO'
       ComRoot  = 'HKCU:\Software\Classes\CLSID'   # installer without admin rights
       Dll      = $Dll64
       Expect   = 'OK       — per-user COM server; needs the HKCR lookup added in v1.1.2' }

    @{ Name     = 'NLTest 07 expandsz'
       Clsid    = '{9E7A1C00-0007-4E00-9B00-000000000007}'
       AsioRoot = 'HKLM:\SOFTWARE\ASIO'
       ComRoot  = 'HKLM:\SOFTWARE\Classes\CLSID'
       Dll      = '%SystemRoot%\System32\ole32.dll'
       Expand   = $true                            # REG_EXPAND_SZ, not REG_SZ
       Expect   = 'OK       — REG_EXPAND_SZ value + env var in the path' }
)

function Remove-Entries {
    foreach ($e in $entries) {
        foreach ($p in @("$($e.AsioRoot)\$($e.Name)",
                         $(if ($e.ComRoot) { "$($e.ComRoot)\$($e.Clsid)" }))) {
            if ($p -and (Test-Path $p)) {
                Remove-Item -Path $p -Recurse -Force
                Write-Host "removed  $p"
            }
        }
    }
}

if ($Remove) {
    Remove-Entries
    Write-Host "`nDone. Restart WNicheLooper; the NLTest entries are gone."
    return
}

Remove-Entries  # idempotent: a re-run replaces instead of duplicating

foreach ($e in $entries) {
    $asioKey = "$($e.AsioRoot)\$($e.Name)"
    New-Item -Path $asioKey -Force | Out-Null
    $type = if ($e.Expand) { 'ExpandString' } else { 'String' }
    New-ItemProperty -Path $asioKey -Name 'CLSID' -Value $e.Clsid -PropertyType $type -Force | Out-Null
    New-ItemProperty -Path $asioKey -Name 'Description' -Value $e.Name -PropertyType String -Force | Out-Null

    if ($e.ComRoot) {
        $inproc = "$($e.ComRoot)\$($e.Clsid)\InprocServer32"
        New-Item -Path $inproc -Force | Out-Null
        # The default value ("(Default)") is what the ASIO SDK reads.
        New-ItemProperty -Path $inproc -Name '(Default)' -Value $e.Dll -PropertyType $type -Force | Out-Null
        New-ItemProperty -Path $inproc -Name 'ThreadingModel' -Value 'Both' -PropertyType String -Force | Out-Null
    }
    Write-Host ("installed  {0,-22} {1}" -f $e.Name, $e.Expect)
}

Write-Host @"

Done. Start WNicheLooper from a terminal and compare the
"NicheLooper: ASIO …" block against the expectations above:

  - 01, 05, 06, 07 must be listed as OK and appear in the device dropdown.
  - 02, 03, 04 must be SKIPPED with exactly the reason shown above.

05 and 06 are the two cases that made a real driver disappear from our list
while other hosts showed it — if either says SKIPPED, that regression is back.

Undo with:  .\asio-test-drivers.ps1 -Remove
"@
