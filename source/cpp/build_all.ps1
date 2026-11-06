$msvc="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
$sdk="C:\Program Files (x86)\Windows Kits\10"; $v="10.0.26100.0"
$src="C:\Users\Krish Raghuwanshi\Work\Baihan-Ai\chimera-project\source\cpp"
$cl="$msvc\bin\Hostx64\x64\cl.exe"; $lk="$msvc\bin\Hostx64\x64\link.exe"
$inc=@("$msvc\include","$sdk\Include\$v\ucrt","$sdk\Include\$v\um","$sdk\Include\$v\shared")
$lib=@("$msvc\lib\x64","$sdk\Lib\$v\ucrt\x64","$sdk\Lib\$v\um\x64")
$incF=($inc|%{"/I`"$_`""}) -join " "
$libA=($lib|%{"/LIBPATH:`"$_`""}) -join " "
Set-Location $src
# DLL
$ok=$true
foreach($f in @("ChimeraMain","NvidiaApi","ScreenCapture","OverlayWindow","OverlayUi")){
    Invoke-Expression "& `"$cl`" /nologo /O2 /std:c++17 /EHa /MT /W3 /D_USRDLL /D_WINDLL /c $incF $f.cpp"
    if($LASTEXITCODE-ne 0){$ok=$false;Write-Host "FAIL DLL: $f";break}
}
if($ok){
    $sysL="user32.lib gdi32.lib kernel32.lib ole32.lib windowscodecs.lib winhttp.lib psapi.lib advapi32.lib crypt32.lib"
    Invoke-Expression "& `"$lk`" /nologo /DLL /OUT:ChimeraMain.dll ChimeraMain.obj NvidiaApi.obj ScreenCapture.obj OverlayWindow.obj OverlayUi.obj $libA $sysL"
    if($LASTEXITCODE-eq 0){Write-Host "DLL:OK"}else{Write-Host "DLL:FAIL";$ok=$false}
}
# Bootstrap
if($ok){
    Invoke-Expression "& `"$cl`" /nologo /O2 /std:c++17 /EHa /MT /W3 /c $incF ChimeraBootstrap.cpp"
    if($LASTEXITCODE-eq 0){
        # /NODEFAULTLIB suppresses implicit library demands (uuid.lib from SDK headers).
        # List only what we actually need: MT CRT + C++ STL + Win32 APIs.
        # libcpmt.lib = C++ STL (std::string, std::vector) for /MT builds
        $crt="libcmt.lib libcpmt.lib libvcruntime.lib libucrt.lib"
        Invoke-Expression "& `"$lk`" /nologo /SUBSYSTEM:CONSOLE /NODEFAULTLIB /OUT:ChimeraBootstrap.exe ChimeraBootstrap.obj $libA $crt user32.lib kernel32.lib psapi.lib ntdll.lib advapi32.lib"
        if($LASTEXITCODE-eq 0){Write-Host "Bootstrap:OK"}else{Write-Host "Bootstrap:FAIL"}
    }else{Write-Host "Bootstrap compile FAIL"}
}
