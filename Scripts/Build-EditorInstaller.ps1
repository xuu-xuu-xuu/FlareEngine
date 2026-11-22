param(
    [ValidateSet('Release')][string]$Configuration = 'Release',
    [string]$Version = '',
    [string]$PackageDirectory = '',
    [string]$OutputDirectory = '',
    [switch]$IncludePortableZip
)

$ErrorActionPreference = 'Stop'
if ([Console]::IsOutputRedirected) { [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) }

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path.TrimEnd('\', '/')
Set-Location -LiteralPath $root
. (Join-Path $PSScriptRoot 'NativeProgram.ps1')
. (Join-Path $PSScriptRoot 'EditorPackageRuntime.ps1')

if (-not $Version) {
    $cmakeText = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw -Encoding UTF8
    if ($cmakeText -match 'project\s*\(\s*FlareEngine\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        $Version = $Matches[1]
    } else {
        $Version = '0.1.0'
    }
}
$Version = $Version.TrimStart('v', 'V')
if ($Version -notmatch '^[0-9A-Za-z._-]+$') {
    throw "Invalid installer version string: $Version"
}

$nativeDir = Join-Path $root ('out/build/windows-x64-' + $Configuration.ToLowerInvariant())
$installerStub = Join-Path $nativeDir 'FlareInstaller.exe'
if (-not (Test-Path -LiteralPath $installerStub)) {
    throw "FlareInstaller.exe is missing; build target FlareInstaller first: $installerStub"
}

if (-not $PackageDirectory) {
    $candidatePkg = Join-Path $root 'out/packages/FlareEngine'
    $candidateDelivery = Join-Path $root 'out/deliveries/FlareEngine'
    if (Test-Path -LiteralPath (Join-Path $candidatePkg 'editor-package.json')) {
        $PackageDirectory = $candidatePkg
    } elseif (Test-Path -LiteralPath (Join-Path $candidateDelivery 'editor-package.json')) {
        $PackageDirectory = $candidateDelivery
    } else {
        throw 'Package directory not found; run Scripts/Build-EditorPackage.ps1 first or pass -PackageDirectory.'
    }
}
$PackageDirectory = (Resolve-Path -LiteralPath $PackageDirectory).Path

$outDir = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $root 'out/release' }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$setupName = "FlareEngine-Setup-v$Version-x64.exe"
$setupPath = Join-Path $outDir $setupName
$packLog = Join-Path $outDir 'installer-pack.log'

Write-Output "Packing Windows setup installer: $setupName ..."
$packCode = Invoke-EngineProgram $installerStub @('--pack', $PackageDirectory, $setupPath, '--version', $Version) $packLog
if ($packCode -ne 0 -or -not (Test-Path -LiteralPath $setupPath)) {
    $logDetail = if (Test-Path -LiteralPath $packLog) { Get-Content -LiteralPath $packLog -Raw } else { '' }
    throw "FlareInstaller --pack failed (exit=$packCode): $logDetail"
}
Remove-Item -LiteralPath $packLog -Force -ErrorAction SilentlyContinue

# 端到端自检：在隔离临时目录执行静默安装 -> 启动器自检 -> 静默卸载，确保发布的安装程序 100% 可安装、可启动、可卸载。
$verifyRoot = Join-Path $outDir ('.verify-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$verifyInstallDir = Join-Path $verifyRoot 'FlareEngine'
New-Item -ItemType Directory -Path $verifyRoot -Force | Out-Null
try {
    $installCode = Invoke-EngineProgram $setupPath @('--silent', '--dir', $verifyInstallDir, '--no-shortcuts', '--no-registry') (Join-Path $verifyRoot 'install.log')
    if ($installCode -ne 0) {
        throw "Installer silent verification failed (exit=$installCode): $(Get-Content -LiteralPath (Join-Path $verifyRoot 'install.log') -Raw)"
    }
    foreach ($expected in @('FlareEditor.exe', 'Uninstall.exe', 'uninstall.dat', 'editor-package.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $verifyInstallDir $expected))) {
            throw "Installed verification missing expected file: $expected"
        }
    }
    [void](Read-EngineEditorPackage $verifyInstallDir)
    $savedLogDir = $env:FLARE_LOG_DIRECTORY
    try {
        $env:FLARE_LOG_DIRECTORY = $verifyRoot
        $smokeCode = Invoke-EngineProgram (Join-Path $verifyInstallDir 'FlareEditor.exe') @('--smoke-launcher', '300', '--no-ui-errors') (Join-Path $verifyRoot 'smoke.log')
    } finally {
        $env:FLARE_LOG_DIRECTORY = $savedLogDir
    }
    if ($smokeCode -ne 0) {
        throw "Installed FlareEditor smoke check failed (exit=$smokeCode)"
    }
    $uninstallCode = Invoke-EngineProgram (Join-Path $verifyInstallDir 'Uninstall.exe') @('--uninstall', '--silent', '--no-registry') (Join-Path $verifyRoot 'uninstall.log')
    if ($uninstallCode -ne 0 -or (Test-Path -LiteralPath (Join-Path $verifyInstallDir 'FlareEditor.exe'))) {
        throw "Uninstaller verification failed (exit=$uninstallCode)"
    }
    Start-Sleep -Milliseconds 1500
} finally {
    if (Test-Path -LiteralPath $verifyRoot) {
        Remove-Item -LiteralPath $verifyRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$artifacts = @($setupPath)

if ($IncludePortableZip) {
    $zipName = "FlareEngine-Portable-v$Version-x64.zip"
    $zipPath = Join-Path $outDir $zipName
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    $zipStage = Join-Path $outDir ('.zipstage-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $zipNested = Join-Path $zipStage 'FlareEngine'
    New-Item -ItemType Directory -Path $zipNested -Force | Out-Null
    try {
        Get-ChildItem -LiteralPath $PackageDirectory -Force | Copy-Item -Destination $zipNested -Recurse -Force
        $tarExe = Join-Path $env:SystemRoot 'System32/tar.exe'
        if (Test-Path -LiteralPath $tarExe) {
            Push-Location -LiteralPath $zipStage
            try {
                & $tarExe -a -cf $zipPath 'FlareEngine'
                if ($LASTEXITCODE -ne 0) { throw "tar.exe zip creation failed with exit code $LASTEXITCODE" }
            } finally {
                Pop-Location
            }
        } else {
            Compress-Archive -LiteralPath $zipNested -DestinationPath $zipPath -CompressionLevel Optimal -Force
        }
    } finally {
        if (Test-Path -LiteralPath $zipStage) {
            Remove-Item -LiteralPath $zipStage -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    $artifacts += $zipPath
}

$shaLines = foreach ($file in $artifacts) {
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $(Split-Path -Leaf $file)"
}
$shaFile = Join-Path $outDir 'SHA256SUMS.txt'
[IO.File]::WriteAllText($shaFile, ($shaLines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))

Write-Output "Release artifacts verified and ready in: $outDir"
foreach ($line in $shaLines) { Write-Output "  $line" }
exit 0
