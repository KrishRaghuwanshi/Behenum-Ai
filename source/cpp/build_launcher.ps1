$ErrorActionPreference = 'Continue'
$msvc="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
$sdk="C:\Program Files (x86)\Windows Kits\10"; $v="10.0.26100.0"
$src="C:\Users\Krish Raghuwanshi\Work\Baihan-Ai\chimera-project\source\cpp"
$cl="$msvc\bin\Hostx64\x64\cl.exe"; $lk="$msvc\bin\Hostx64\x64\link.exe"
$inc=@("$msvc\include","$sdk\Include\$v\ucrt","$sdk\Include\$v\um","$sdk\Include\$v\shared")
$lib=@("$msvc\lib\x64","$sdk\Lib\$v\ucrt\x64","$sdk\Lib\$v\um\x64")
$incF=($inc|%{"/I`"$_`""}) -join " "
$libA=($lib|%{"/LIBPATH:`"$_`""}) -join " "
Set-Location $src
$r = cmd /c "`"$cl`" /nologo /O2 /std:c++17 /EHa /MT /W3 /c $incF ChimeraLauncher.cpp 2>&1"
$r | ForEach-Object { Write-Host $_ }
if($LASTEXITCODE -ne 0){ Write-Host "COMPILE FAIL"; exit 1 }
$crt="libcmt.lib libcpmt.lib libvcruntime.lib libucrt.lib"
$r2 = cmd /c "`"$lk`" /nologo /SUBSYSTEM:WINDOWS /NODEFAULTLIB /OUT:ChimeraLauncher.exe ChimeraLauncher.obj $libA $crt user32.lib kernel32.lib advapi32.lib 2>&1"
$r2 | ForEach-Object { Write-Host $_ }
if($LASTEXITCODE -ne 0){ Write-Host "LINK FAIL"; exit 1 }
Copy-Item -Force ChimeraLauncher.exe "C:\ChimeraRun\ChimeraLauncher.exe"
Copy-Item -Force ChimeraLauncher.exe "C:\ChimeraRun-working\ChimeraLauncher.exe"
Write-Host "[+] ChimeraLauncher.exe deployed"
