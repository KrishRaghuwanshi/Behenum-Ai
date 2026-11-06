$ErrorActionPreference = 'Continue'
$out = [System.Collections.Generic.List[string]]::new()
$log = "C:\Users\Krish Raghuwanshi\Work\Baihan-Ai\chimera-project\source\cpp\helper_build.txt"

$msvc="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
$sdk="C:\Program Files (x86)\Windows Kits\10"; $v="10.0.26100.0"
$src="C:\Users\Krish Raghuwanshi\Work\Baihan-Ai\chimera-project\source\cpp"
$cl="$msvc\bin\Hostx64\x64\cl.exe"; $lk="$msvc\bin\Hostx64\x64\link.exe"
$inc=@("$msvc\include","$sdk\Include\$v\ucrt","$sdk\Include\$v\um","$sdk\Include\$v\shared")
$lib=@("$msvc\lib\x64","$sdk\Lib\$v\ucrt\x64","$sdk\Lib\$v\um\x64")
$incF=($inc|%{"/I`"$_`""}) -join " "
$libA=($lib|%{"/LIBPATH:`"$_`""}) -join " "

Set-Location $src

function Run($label, $cmd) {
    $out.Add("=== $label ===")
    $r = cmd /c "$cmd 2>&1"
    foreach($l in $r) { $out.Add($l) }
    $out.Add("EXIT: $LASTEXITCODE")
    $LASTEXITCODE -eq 0
}

$ok = $true

# ── ChimeraHelper.exe ──
if($ok){
    if(-not(Run "Compile Helper" "`"$cl`" /nologo /O2 /std:c++17 /EHa /MT /W3 /c $incF ChimeraHelper.cpp")){
        $ok=$false; Write-Host "Helper compile FAIL"
    }
}
if($ok){
    $crt="libcmt.lib libcpmt.lib libvcruntime.lib libucrt.lib"
    if(-not(Run "Link Helper" "`"$lk`" /nologo /SUBSYSTEM:CONSOLE /NODEFAULTLIB /OUT:ChimeraHelper.exe ChimeraHelper.obj $libA $crt user32.lib kernel32.lib advapi32.lib")){
        $ok=$false; Write-Host "Helper link FAIL"
    }
}

# ── ChimeraHost.exe ──
if($ok){
    if(-not(Run "Compile Host" "`"$cl`" /nologo /O2 /std:c++17 /EHa /MT /W3 /c $incF ChimeraHost.cpp")){
        $ok=$false; Write-Host "Host compile FAIL"
    }
}
if($ok){
    $crt="libcmt.lib libcpmt.lib libvcruntime.lib libucrt.lib"
    if(-not(Run "Link Host" "`"$lk`" /nologo /SUBSYSTEM:WINDOWS /NODEFAULTLIB /OUT:ChimeraHost.exe ChimeraHost.obj $libA $crt user32.lib kernel32.lib")){
        $ok=$false; Write-Host "Host link FAIL"
    }
}

if($ok){
    Copy-Item -Force ChimeraHelper.exe "C:\ChimeraRun\ChimeraHelper.exe" -ErrorAction SilentlyContinue
    Copy-Item -Force ChimeraHost.exe "C:\ChimeraRun\ChimeraHost.exe" -ErrorAction SilentlyContinue
    $out.Add("DEPLOYED")
    Write-Host "[+] ChimeraHelper.exe + ChimeraHost.exe deployed to C:\ChimeraRun\"
}

$out | Set-Content -Path $log -Encoding UTF8
Write-Host "Log: $log"
