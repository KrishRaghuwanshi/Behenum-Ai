$ErrorActionPreference = 'Continue'
$out = [System.Collections.Generic.List[string]]::new()
$log = "C:\Users\Krish Raghuwanshi\Work\Baihan-Ai\chimera-project\source\cpp\build_result.txt"

$msvc="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
$sdk="C:\Program Files (x86)\Windows Kits\10"; $v="10.0.26100.0"
$src="C:\Users\Krish Raghuwanshi\Work\Baihan-Ai\chimera-project\source\cpp"
$cl="`"$msvc\bin\Hostx64\x64\cl.exe`""
$lk="`"$msvc\bin\Hostx64\x64\link.exe`""

$incF="/I`"$msvc\include`" /I`"$sdk\Include\$v\ucrt`" /I`"$sdk\Include\$v\um`" /I`"$sdk\Include\$v\shared`""
$libA="/LIBPATH:`"$msvc\lib\x64`" /LIBPATH:`"$sdk\Lib\$v\ucrt\x64`" /LIBPATH:`"$sdk\Lib\$v\um\x64`""

Set-Location $src

function Run($label, $cmd) {
    $out.Add("=== $label ===")
    $r = cmd /c "$cmd 2>&1"
    foreach($l in $r) { $out.Add($l) }
    $out.Add("EXIT: $LASTEXITCODE")
    $LASTEXITCODE -eq 0
}

$ok = $true

# ---- DLL ----
foreach($f in @("ChimeraMain","NvidiaApi","ScreenCapture","OverlayWindow","OverlayUi")){
    if(-not(Run "Compile $f" "$cl /nologo /O2 /std:c++17 /EHa /MT /W3 /D_USRDLL /D_WINDLL /c $incF $f.cpp")){
        $ok=$false; $out.Add("ABORT at $f"); break
    }
}
if($ok){
    $sysL="user32.lib gdi32.lib kernel32.lib ole32.lib windowscodecs.lib winhttp.lib psapi.lib advapi32.lib crypt32.lib"
    if(-not(Run "Link DLL" "$lk /nologo /DLL /OUT:ChimeraMain.dll ChimeraMain.obj NvidiaApi.obj ScreenCapture.obj OverlayWindow.obj OverlayUi.obj $libA $sysL")){
        $ok=$false
    }
}

# ---- Bootstrap ----
if($ok){
    # /c = compile only; linking is done separately with /NODEFAULTLIB below
    if(Run "Compile Bootstrap" "$cl /nologo /O2 /std:c++17 /EHa /MT /W3 /c $incF ChimeraBootstrap.cpp"){
        # libcpmt.lib = C++ STL for /MT builds (std::string, std::vector)
        $crt="libcmt.lib libcpmt.lib libvcruntime.lib libucrt.lib"
        if(Run "Link Bootstrap" "$lk /nologo /SUBSYSTEM:CONSOLE /NODEFAULTLIB /OUT:ChimeraBootstrap.exe ChimeraBootstrap.obj $libA $crt user32.lib kernel32.lib psapi.lib ntdll.lib advapi32.lib"){
            $out.Add("BUILD SUCCESS")
            # Deploy
            if(-not(Test-Path "C:\ChimeraRun")){ New-Item -ItemType Directory -Path "C:\ChimeraRun" -Force | Out-Null }
            Copy-Item -Force ChimeraMain.dll "C:\ChimeraRun\ChimeraMain.dll"
            Copy-Item -Force ChimeraBootstrap.exe "C:\ChimeraRun\ChimeraBootstrap.exe"
            $out.Add("DEPLOYED to C:\ChimeraRun\")
        }
    }
}

$out | Set-Content -Path $log -Encoding UTF8
Write-Host "Done. Log: $log"
