$ErrorActionPreference = 'Stop'

$msvc = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
$sdk  = "C:\Program Files (x86)\Windows Kits\10"
$v    = "10.0.26100.0"
$src  = "C:\Users\Krish Raghuwanshi\Work\Baihan-Ai\chimera-project\source\cpp"
$cl   = "$msvc\bin\Hostx64\x64\cl.exe"
$lk   = "$msvc\bin\Hostx64\x64\link.exe"

$inc = @(
    "/I`"$msvc\include`"",
    "/I`"$sdk\Include\$v\ucrt`"",
    "/I`"$sdk\Include\$v\um`"",
    "/I`"$sdk\Include\$v\shared`""
)
$lib = @(
    "/LIBPATH:`"$msvc\lib\x64`"",
    "/LIBPATH:`"$sdk\Lib\$v\ucrt\x64`"",
    "/LIBPATH:`"$sdk\Lib\$v\um\x64`""
)

Set-Location $src
$failed = $false

# ══════════════════════════════════════════════════════════════════════
# STEP 1: Compile all DLL source files
# ══════════════════════════════════════════════════════════════════════
Write-Host "`n═══ STEP 1: Compiling DLL sources ═══" -ForegroundColor Cyan

$dllSources = @("ChimeraMain", "NvidiaApi", "ScreenCapture", "OverlayWindow", "OverlayUi")
foreach ($f in $dllSources) {
    Write-Host "  Compiling $f.cpp..." -NoNewline
    $args = @("/nologo", "/O2", "/std:c++17", "/EHa", "/MT", "/W3", "/D_USRDLL", "/D_WINDLL", "/c") + $inc + @("$f.cpp")
    & "$cl" @args 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host " FAIL" -ForegroundColor Red
        $failed = $true; break
    }
    Write-Host " OK" -ForegroundColor Green
}

# ══════════════════════════════════════════════════════════════════════
# STEP 2: Link ChimeraMain.dll
# ══════════════════════════════════════════════════════════════════════
if (-not $failed) {
    Write-Host "`n═══ STEP 2: Linking ChimeraMain.dll ═══" -ForegroundColor Cyan
    $sysLibs = @("user32.lib","gdi32.lib","kernel32.lib","ole32.lib","windowscodecs.lib","winhttp.lib","psapi.lib","advapi32.lib","crypt32.lib")
    $linkArgs = @("/nologo","/DLL","/OUT:ChimeraMain.dll","ChimeraMain.obj","NvidiaApi.obj","ScreenCapture.obj","OverlayWindow.obj","OverlayUi.obj") + $lib + $sysLibs
    & "$lk" @linkArgs 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "  FAIL" -ForegroundColor Red; $failed = $true }
    else { Write-Host "  ChimeraMain.dll OK" -ForegroundColor Green }
}

# ══════════════════════════════════════════════════════════════════════
# STEP 3: Build ChimeraHelper.exe
# ══════════════════════════════════════════════════════════════════════
if (-not $failed) {
    Write-Host "`n═══ STEP 3: Building ChimeraHelper.exe ═══" -ForegroundColor Cyan
    $args = @("/nologo","/O2","/std:c++17","/EHa","/MT","/W3","/c") + $inc + @("ChimeraHelper.cpp")
    & "$cl" @args 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "  Compile FAIL" -ForegroundColor Red; $failed = $true }
    else {
        $crt = @("libcmt.lib","libcpmt.lib","libvcruntime.lib","libucrt.lib")
        $linkArgs = @("/nologo","/SUBSYSTEM:CONSOLE","/NODEFAULTLIB","/OUT:ChimeraHelper.exe","ChimeraHelper.obj") + $lib + $crt + @("user32.lib","kernel32.lib","advapi32.lib")
        & "$lk" @linkArgs 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Host "  Link FAIL" -ForegroundColor Red; $failed = $true }
        else { Write-Host "  ChimeraHelper.exe OK" -ForegroundColor Green }
    }
}

# ══════════════════════════════════════════════════════════════════════
# STEP 4: Build ChimeraHost.exe
# ══════════════════════════════════════════════════════════════════════
if (-not $failed) {
    Write-Host "`n═══ STEP 4: Building ChimeraHost.exe ═══" -ForegroundColor Cyan
    $args = @("/nologo","/O2","/std:c++17","/EHa","/MT","/W3","/c") + $inc + @("ChimeraHost.cpp")
    & "$cl" @args 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "  Compile FAIL" -ForegroundColor Red; $failed = $true }
    else {
        $crt = @("libcmt.lib","libcpmt.lib","libvcruntime.lib","libucrt.lib")
        $linkArgs = @("/nologo","/SUBSYSTEM:WINDOWS","/NODEFAULTLIB","/OUT:ChimeraHost.exe","ChimeraHost.obj") + $lib + $crt + @("user32.lib","kernel32.lib")
        & "$lk" @linkArgs 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Host "  Link FAIL" -ForegroundColor Red; $failed = $true }
        else { Write-Host "  ChimeraHost.exe OK" -ForegroundColor Green }
    }
}

# ══════════════════════════════════════════════════════════════════════
# STEP 5: Build ChimeraBootstrap.exe
# ══════════════════════════════════════════════════════════════════════
if (-not $failed) {
    Write-Host "`n═══ STEP 5: Building ChimeraBootstrap.exe ═══" -ForegroundColor Cyan
    $args = @("/nologo","/O2","/std:c++17","/EHa","/MT","/W3","/c") + $inc + @("ChimeraBootstrap.cpp")
    & "$cl" @args 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "  Compile FAIL" -ForegroundColor Red; $failed = $true }
    else {
        $crt = @("libcmt.lib","libcpmt.lib","libvcruntime.lib","libucrt.lib")
        $linkArgs = @("/nologo","/SUBSYSTEM:CONSOLE","/NODEFAULTLIB","/OUT:ChimeraBootstrap.exe","ChimeraBootstrap.obj") + $lib + $crt + @("user32.lib","kernel32.lib","psapi.lib","ntdll.lib","advapi32.lib")
        & "$lk" @linkArgs 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Host "  Link FAIL" -ForegroundColor Red; $failed = $true }
        else { Write-Host "  ChimeraBootstrap.exe OK" -ForegroundColor Green }
    }
}

# ══════════════════════════════════════════════════════════════════════
# STEP 6: Deploy to C:\ChimeraDeploy
# ══════════════════════════════════════════════════════════════════════
if (-not $failed) {
    Write-Host "`n═══ STEP 6: Deploying to C:\ChimeraDeploy ═══" -ForegroundColor Cyan
    $deploy = "C:\ChimeraDeploy"
    if (-not (Test-Path $deploy)) { New-Item -ItemType Directory -Path $deploy | Out-Null }
    
    Copy-Item -Force "$src\ChimeraMain.dll"       "$deploy\ChimeraMain.dll"
    Copy-Item -Force "$src\ChimeraBootstrap.exe"   "$deploy\ChimeraBootstrap.exe"
    Copy-Item -Force "$src\ChimeraHelper.exe"      "$deploy\ChimeraHelper.exe"
    Copy-Item -Force "$src\ChimeraHost.exe"        "$deploy\ChimeraHost.exe"
    # RuntimeBroker.exe is a copy of ChimeraHost.exe (stealth name)
    Copy-Item -Force "$src\ChimeraHost.exe"        "$deploy\RuntimeBroker.exe"
    
    Write-Host "  All files deployed!" -ForegroundColor Green
    Write-Host ""
    Write-Host "═══ BUILD COMPLETE ═══" -ForegroundColor Green
    Write-Host "  Deploy dir: $deploy" -ForegroundColor Yellow
    Get-ChildItem $deploy | Format-Table Name, Length, LastWriteTime -AutoSize
} else {
    Write-Host "`n═══ BUILD FAILED ═══" -ForegroundColor Red
}
