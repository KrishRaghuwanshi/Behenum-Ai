$ErrorActionPreference = 'Continue'

$msvc  = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
$sdk   = "C:\Program Files (x86)\Windows Kits\10"
$sdk_v = "10.0.26100.0"
$wdk_v = "10.0.28000.0"
$src   = "C:\Users\Krish Raghuwanshi\Work\Baihan-Ai\chimera-project\source\c"
$out   = "C:\ChimeraRun"
$log   = "$src\driver_build.txt"

$cl = "$msvc\bin\Hostx64\x64\cl.exe"
$lk = "$msvc\bin\Hostx64\x64\link.exe"
$st = "$sdk\bin\$sdk_v\x64\signtool.exe"

$km_inc    = "$sdk\Include\$wdk_v\km"
$km_shared = "$sdk\Include\$sdk_v\shared"
$km_ucrt   = "$sdk\Include\$sdk_v\ucrt"
$km_lib    = "$sdk\Lib\$wdk_v\km\x64"
$ms_inc    = "$msvc\include"

$lines = [System.Collections.Generic.List[string]]::new()
Set-Location $src

Write-Host "=== Step 1: Compile ==="
$lines.Add("=== Compile ===")
$cargs = @(
    "/nologo", "/kernel", "/std:c11", "/c", "/GR-", "/Zl", "/O1", "/W3",
    "/D", "NDEBUG", "/D", "_AMD64_", "/D", "AMD64", "/D", "_WIN64",
    "/I", $km_inc, "/I", $km_shared, "/I", $km_ucrt, "/I", $ms_inc,
    "chimera_guard.c"
)
& $cl @cargs 2>&1 | ForEach-Object { $lines.Add([string]$_); Write-Host $_ }
$lines.Add("EXIT: $LASTEXITCODE")
if ($LASTEXITCODE -ne 0) { $lines | Set-Content $log; Write-Host "COMPILE FAILED"; exit 1 }
Write-Host "[+] Compiled OK"

Write-Host "=== Step 2: Link ==="
$lines.Add("=== Link ===")
$largs = @(
    "/nologo", "/DRIVER", "/SUBSYSTEM:NATIVE", "/ENTRY:DriverEntry",
    "/NODEFAULTLIB", "/INTEGRITYCHECK",
    "/LIBPATH:$km_lib",
    "ntoskrnl.lib",
    "chimera_guard.obj",
    "/OUT:chimera_guard.sys"
)
& $lk @largs 2>&1 | ForEach-Object { $lines.Add([string]$_); Write-Host $_ }
$lines.Add("EXIT: $LASTEXITCODE")
if ($LASTEXITCODE -ne 0) { $lines | Set-Content $log; Write-Host "LINK FAILED"; exit 1 }
Write-Host "[+] chimera_guard.sys built"

Write-Host "=== Step 3: Certificate ==="
# Create cert in CurrentUser\My with EXPORTABLE private key.
# LocalMachine\My certs have keys protected by SYSTEM LSA -- Export-PfxCertificate
# fails silently and signtool reports "No certificates were found."
# We recreate fresh each run to avoid stale non-exportable certs.
$oldCerts = Get-ChildItem Cert:\CurrentUser\My -ErrorAction SilentlyContinue |
            Where-Object { $_.Subject -eq "CN=ChimeraDriver" }
foreach ($c in $oldCerts) {
    Remove-Item "Cert:\CurrentUser\My\$($c.Thumbprint)" -Force -ErrorAction SilentlyContinue
}
$cert = New-SelfSignedCertificate `
    -Subject "CN=ChimeraDriver" `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -Type CodeSigningCert `
    -KeyUsage DigitalSignature `
    -KeyAlgorithm RSA `
    -KeyLength 2048 `
    -HashAlgorithm SHA256 `
    -KeyExportPolicy Exportable `
    -NotAfter (Get-Date).AddYears(10)
$pfxPath = "$src\chimera.pfx"
$pfxPwd  = ConvertTo-SecureString -String "chimera123" -AsPlainText -Force
Export-PfxCertificate -Cert $cert -FilePath $pfxPath -Password $pfxPwd -Force | Out-Null
Export-Certificate    -Cert $cert -FilePath "$src\chimera.cer" -Force | Out-Null
Import-Certificate -FilePath "$src\chimera.cer" -CertStoreLocation "Cert:\LocalMachine\Root"             | Out-Null
Import-Certificate -FilePath "$src\chimera.cer" -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher" | Out-Null
Write-Host "[+] Certificate ready (thumbprint: $($cert.Thumbprint))"

Write-Host "=== Step 4: Sign ==="
$lines.Add("=== Sign ===")
$sargs = @("sign", "/fd", "SHA256", "/f", $pfxPath, "/p", "chimera123", "/ph", "chimera_guard.sys")
& $st @sargs 2>&1 | ForEach-Object { $lines.Add([string]$_); Write-Host $_ }
$lines.Add("EXIT: $LASTEXITCODE")
if ($LASTEXITCODE -ne 0) { $lines | Set-Content $log; Write-Host "SIGN FAILED"; exit 1 }
Write-Host "[+] Signed"

Write-Host "=== Step 5: Deploy ==="
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Path $out -Force | Out-Null }
Copy-Item -Force "chimera_guard.sys" "$out\chimera_guard.sys"
Write-Host "[+] Deployed to $out\chimera_guard.sys"

Write-Host "=== Step 6: Install service ==="
$svcName = "WinNetSvc"
$svc = Get-Service -Name $svcName -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "[.] Stopping old service..."
    sc.exe stop $svcName | Out-Null
    Start-Sleep -Seconds 2
    sc.exe delete $svcName | Out-Null
    Start-Sleep -Seconds 1
}
sc.exe create $svcName type= kernel start= demand binPath= "$out\chimera_guard.sys" DisplayName= "Windows Network Service"
Write-Host "[.] Starting..."
sc.exe start $svcName
$lines.Add("sc start exit: $LASTEXITCODE")

$lines | Set-Content $log -Encoding UTF8
Write-Host "Done. Log: $log"
