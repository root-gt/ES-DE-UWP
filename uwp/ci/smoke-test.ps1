#  ES-DE UWP smoke test, run by .github/workflows/uwp.yml on a Windows runner.
#
#  1. Installs the signed package and its framework dependencies.
#  2. Starts ES-DE without any configuration (first start, no games).
#  3. Starts ES-DE again with a RomM server configured (tests/romm/mock_romm.py) and the
#     RomMSelfTestGame setting, which makes ES-DE import the RomM library and download
#     and extract a game inside the app container on startup.
#  4. Checks the log for the Direct3D 12 renderer, the RomM self-test result and a
#     completed startup, and that the application is still running. Screenshots, logs
#     and the downloaded files are written to the output directory.

param(
    [string]$Dist = "dist",
    [string]$Output = "smoke",
    [int]$StartupTimeout = 180
)

$ErrorActionPreference = "Stop"
$Output = (New-Item -ItemType Directory -Force $Output).FullName
$failures = New-Object System.Collections.Generic.List[string]

function Save-Screenshot([string]$name) {
    try {
        Add-Type -AssemblyName System.Windows.Forms, System.Drawing
        $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
        $bitmap.Save("$Output\$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
        $graphics.Dispose(); $bitmap.Dispose()
        Write-Host "Screenshot: $name.png ($($bounds.Width)x$($bounds.Height))"
    } catch {
        Write-Host "Couldn't take screenshot: $_"
    }
}

function Find-Log([string]$localState) {
    foreach ($candidate in @("$localState\ES-DE\logs\es_log.txt", "E:\ES-DE\logs\es_log.txt")) {
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

function Start-ESDE([string]$pfn, [string]$localState, [string]$name) {
    Get-Process ES-DE -ErrorAction SilentlyContinue | Stop-Process -Force
    $log = Find-Log $localState
    if ($log) { Remove-Item $log -Force }

    Write-Host "Starting ES-DE ($name)"
    Start-Process "shell:AppsFolder\$pfn!App"

    $deadline = (Get-Date).AddSeconds($StartupTimeout)
    $started = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        $log = Find-Log $localState
        if ($log -and (Select-String -Path $log -Pattern "Application startup time" -Quiet)) {
            $started = $true
            break
        }
        if ($log -and -not (Get-Process ES-DE -ErrorAction SilentlyContinue)) {
            Start-Sleep -Seconds 2
            break
        }
    }
    # Let it render a few seconds before checking that it's still alive.
    Start-Sleep -Seconds 8
    Save-Screenshot $name
    $running = [bool](Get-Process ES-DE -ErrorAction SilentlyContinue)

    $log = Find-Log $localState
    if ($log) {
        Copy-Item $log "$Output\$name-es_log.txt"
        Write-Host "---- $name log ----"
        Get-Content $log | Write-Host
        Write-Host "-------------------"
    }
    return @{ Log = $log; Started = $started; Running = $running }
}

function Assert-Log($result, [string]$name, [string]$pattern, [string]$description) {
    if (-not $result.Log -or -not (Select-String -Path $result.Log -Pattern $pattern -Quiet)) {
        $failures.Add("${name}: $description")
        Write-Host "FAIL ${name}: $description" -ForegroundColor Red
    } else {
        Write-Host "PASS ${name}: $description" -ForegroundColor Green
    }
}

function Assert-True([bool]$condition, [string]$description) {
    if (-not $condition) {
        $failures.Add($description)
        Write-Host "FAIL $description" -ForegroundColor Red
    } else {
        Write-Host "PASS $description" -ForegroundColor Green
    }
}

# --------------------------------------------------------------------------------------
# Installation.

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host "Running as $($identity.Name), administrator: $isAdmin"
Write-Host "OS: $((Get-CimInstance Win32_OperatingSystem).Caption) $([Environment]::OSVersion.Version)"

# Sideloading of trusted packages is allowed by default on current Windows versions, the
# policy is only set in case it isn't.
try {
    New-Item -Path "HKLM:\SOFTWARE\Policies\Microsoft\Windows\Appx" -Force -ErrorAction Stop | Out-Null
    Set-ItemProperty "HKLM:\SOFTWARE\Policies\Microsoft\Windows\Appx" AllowAllTrustedApps 1 -Type DWord -ErrorAction Stop
} catch {
    Write-Host "Couldn't set the sideloading policy (continuing): $($_.Exception.Message)"
}

$certificate = Get-ChildItem $Dist -Filter *.cer | Select-Object -First 1
foreach ($store in @("Cert:\LocalMachine\TrustedPeople", "Cert:\LocalMachine\Root", "Cert:\CurrentUser\TrustedPeople")) {
    try {
        Import-Certificate -FilePath $certificate.FullName -CertStoreLocation $store -ErrorAction Stop | Out-Null
        Write-Host "Certificate imported into $store"
    } catch {
        Write-Host "Couldn't import the certificate into ${store}: $($_.Exception.Message)"
    }
}

$package = Get-ChildItem $Dist -File | Where-Object { $_.Extension -in ".msix", ".appx" } | Select-Object -First 1
$dependencies = @(Get-ChildItem "$Dist\Dependencies" -File -ErrorAction SilentlyContinue | ForEach-Object FullName)
Write-Host "Installing $($package.Name) with dependencies: $($dependencies -join ', ')"
try {
    if ($dependencies.Count -gt 0) {
        Add-AppxPackage -Path $package.FullName -DependencyPath $dependencies
    } else {
        Add-AppxPackage -Path $package.FullName
    }
} catch {
    Write-Host "Add-AppxPackage failed: $($_.Exception.Message)"
    try { Get-AppxLog -All | Where-Object { $_.Level -le 3 } | Select-Object -Last 10 TimeCreated, Message | Format-List | Out-String | Write-Host } catch { }
    throw
}

$app = Get-AppxPackage -Name "ESDE.UWP"
if (-not $app) { throw "The package was not installed" }
$pfn = $app.PackageFamilyName
$localState = "$env:LOCALAPPDATA\Packages\$pfn\LocalState"
Write-Host "Installed $($app.PackageFullName)"
Write-Host "Install location: $($app.InstallLocation)"
(Get-ChildItem $app.InstallLocation | Select-Object Name, Length | Format-Table -AutoSize | Out-String) | Write-Host

# --------------------------------------------------------------------------------------
# First start without any configuration.

$first = Start-ESDE $pfn $localState "1-first-start"
Assert-Log $first "first start" "Setting up Direct3D 12" "Direct3D 12 renderer selected"
Assert-Log $first "first start" "Direct3D 12 adapter" "Direct3D 12 device created"
Assert-True ($first.Started) "first start: startup completed"
Assert-True ($first.Running) "first start: ES-DE still running after startup"
Get-Process ES-DE -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

# --------------------------------------------------------------------------------------
# Second start with RomM configured.

$firstLog = Find-Log $localState
$esHome = if ($firstLog) { Split-Path (Split-Path (Split-Path $firstLog)) } else { $localState }
Write-Host "ES-DE home directory: $esHome"

$python = (Get-Command python -ErrorAction SilentlyContinue).Source
$mock = Start-Process $python -ArgumentList "tests/romm/mock_romm.py", "8099" -PassThru `
    -RedirectStandardError "$Output\mock_romm.log" -NoNewWindow
Start-Sleep -Seconds 2

# UWP applications can't connect to localhost unless they are exempted.
CheckNetIsolation.exe LoopbackExempt -a -n="$pfn" | Write-Host

New-Item -ItemType Directory -Force "$esHome\ES-DE\settings" | Out-Null
$settingsFile = "$esHome\ES-DE\settings\es_settings.xml"
$settings = if (Test-Path $settingsFile) { Get-Content $settingsFile -Raw } else { "<?xml version=`"1.0`"?>`n" }
$settings += @"
<bool name="RomMEnabled" value="true" />
<string name="RomMServerURL" value="http://127.0.0.1:8099" />
<string name="RomMAPIToken" value="testtoken" />
<string name="RomMExtractedExtensions" value="snes=sfc" />
<string name="RomMSelfTestGame" value="snes/Super Metroid (USA).sfc" />
"@
Set-Content -Path $settingsFile -Value $settings -Encoding UTF8

$second = Start-ESDE $pfn $localState "2-romm"
Assert-Log $second "RomM" "RomM self-test: Connection" "connection to the RomM server"
Assert-Log $second "RomM" "RomM self-test: Imported" "library import"
Assert-Log $second "RomM" "RomM self-test: PASSED" "game download and ZIP extraction"
Assert-True ($second.Started) "RomM: startup completed with the imported games"
Assert-True ($second.Running) "RomM: ES-DE still running after startup"

$sfc = "$esHome\ROMs\snes\Super Metroid (USA).sfc"
Assert-True ((Test-Path $sfc) -and (Get-Item $sfc).Length -eq 1048576) "RomM: extracted game has the expected size"
Get-ChildItem "$esHome\ROMs" -Recurse -File -ErrorAction SilentlyContinue |
    Select-Object -First 20 FullName, Length | Format-Table -AutoSize | Out-String | Write-Host

Get-Process ES-DE -ErrorAction SilentlyContinue | Stop-Process -Force
Stop-Process -Id $mock.Id -Force -ErrorAction SilentlyContinue

# --------------------------------------------------------------------------------------
# Diagnostics and result.

Get-WinEvent -FilterHashtable @{ LogName = "Application"; Level = 1, 2; StartTime = (Get-Date).AddHours(-1) } -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match "ES-DE" } | Select-Object -First 10 TimeCreated, ProviderName, Message |
    Format-List | Out-File "$Output\application-events.txt"
if ($esHome -and (Test-Path "$esHome\ES-DE")) {
    Copy-Item "$esHome\ES-DE\settings\es_settings.xml" "$Output\es_settings.xml" -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "$($failures.Count) check(s) failed:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
Write-Host ""
Write-Host "All smoke test checks passed" -ForegroundColor Green
