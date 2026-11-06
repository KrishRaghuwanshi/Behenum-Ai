$ErrorActionPreference = 'Stop'

$msvc="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
$sdk="C:\Program Files (x86)\Windows Kits\10"
$v="10.0.26100.0"
$src="C:\Users\Krish Raghuwanshi\Work\Baihan-Ai\chimera-project\source\cpp"
$cl="$msvc\bin\Hostx64\x64\cl.exe"
$lk="$msvc\bin\Hostx64\x64\link.exe"
$inc=@("$msvc\include","$sdk\Include\$v\ucrt","$sdk\Include\$v\um","$sdk\Include\$v\shared")
$lib=@("$msvc\lib\x64","$sdk\Lib\$v\ucrt\x64","$sdk\Lib\$v\um\x64")
$incF=($inc|%{"/I`"$_`""}) -join " "
$libA=($lib|%{"/LIBPATH:`"$_`""}) -join " "

Set-Location $src

Write-Host "[1/2] Compiling OverlayWindow.cpp..."
$args = @("/nologo", "/O2", "/std:c++17", "/EHa", "/MT", "/W3", "/D_USRDLL", "/D_WINDLL", "/c") + ($inc | ForEach-Object { "/I`"$_`"" }) + @("OverlayWindow.cpp")
& "$cl" @args
if ($LASTEXITCODE -ne 0) { throw "Compilation failed" }

Write-Host "[2/2] Linking ChimeraMain.dll..."
$sysLibs = @("user32.lib", "gdi32.lib", "kernel32.lib", "ole32.lib", "windowscodecs.lib", "winhttp.lib", "psapi.lib", "advapi32.lib", "crypt32.lib")
$linkArgs = @("/nologo", "/DLL", "/OUT:ChimeraMain.dll", "ChimeraMain.obj", "NvidiaApi.obj", "ScreenCapture.obj", "OverlayWindow.obj", "OverlayUi.obj") + ($lib | ForEach-Object { "/LIBPATH:`"$_`"" }) + $sysLibs
& "$lk" @linkArgs
if ($LASTEXITCODE -ne 0) { throw "Linking failed" }

Write-Host "[+] SUCCESS: ChimeraMain.dll rebuilt!"
Write-Host ""
Write-Host "Deploy to C:\chimera_devin\ ? (Y/N)"
$response = Read-Host
if ($response -eq 'Y' -or $response -eq 'y') {
    Copy-Item -Force ChimeraMain.dll "C:\chimera_devin\ChimeraMain.dll"
    Write-Host "[+] Deployed to C:\chimera_devin\ChimeraMain.dll"
    Write-Host "[!] Restart ChimeraHelper to load the new DLL"
}
