param([switch]$Headless,[switch]$Hidden,[switch]$Editor,[switch]$Play,[ValidateRange(0,100000)][int]$Frames=0,[string]$Capture='',[ValidateRange(1,100000)][int]$CaptureFrame=1,[string]$Resolution='',[double]$FixedStep=0,[string]$CaptureSequence='',[ValidateRange(0,100000)][int]$CaptureCount=0,[string]$Profile='',[ValidateSet('zh-CN','en-US')][string]$Locale='zh-CN')
if([Console]::IsOutputRedirected){[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)}
$ErrorActionPreference='Stop'
function PackageFile([string]$Relative) {
    if([IO.Path]::IsPathRooted($Relative)){throw 'Export.PathOutsideRoot'}
    $path=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot $Relative))
    if(!$path.StartsWith($PSScriptRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Export.PathOutsideRoot'}
    return $path
}
$settings=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'launch.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if($Headless -and $Editor){throw 'Bridge.InvalidArgument'}
$arguments=@('--project-root',(PackageFile 'Project'),'--scene',(PackageFile $settings.scene),'--asset-root',(PackageFile $settings.assetRoot),'--asset-cache',(PackageFile $settings.assetCache),'--project',(PackageFile $settings.scriptAssembly),'--script',$settings.scriptType,'--locale',$Locale)
if($settings.menu){$arguments+='--menu'}
if($settings.manifest){$arguments+=@('--player-manifest',(PackageFile $settings.manifest))}
if($settings.bindings){$arguments+=@('--bindings',(PackageFile $settings.bindings))}
if($settings.ui){$arguments+=@('--ui',(PackageFile $settings.ui))}
if($Editor){$arguments+='--editor';if($Play){$arguments+='--play'}}elseif(!$Headless){$arguments+='--window'}
if($Hidden){$arguments+='--hidden'}
if($Frames -gt 0){$arguments+=@('--frames',[string]$Frames)}
if($Resolution){if($Resolution -notmatch '^\d+x\d+$'){throw 'Bridge.InvalidArgument'};$arguments+=@('--resolution',$Resolution)}
if($FixedStep -gt 0){$arguments+=@('--fixed-step',([string]::Format([Globalization.CultureInfo]::InvariantCulture,'{0}',[double]$FixedStep)))}
if($Capture){if($Headless -or $Editor){throw 'Bridge.InvalidArgument'};$arguments+=@('--capture',[IO.Path]::GetFullPath($Capture),'--capture-frame',[string]$CaptureFrame)}
if($Profile){$arguments+=@('--profile',($(if([IO.Path]::IsPathRooted($Profile)){$Profile}else{PackageFile $Profile}))) }
if($CaptureSequence){
    if($Headless -or $Editor -or $Capture){throw 'Bridge.InvalidArgument'}
    if($CaptureCount -le 0){throw 'Bridge.InvalidArgument'}
    $arguments+=@('--capture-sequence',[IO.Path]::GetFullPath($CaptureSequence),'--capture-count',[string]$CaptureCount)
}
# 原生宿主把着色器、设备等诊断写到 stderr。PowerShell 5.1 在 Stop 模式下会把重定向后的
# 任何 stderr 行变成终止错误，导致警告被当成启动失败；调用子进程前只放宽本脚本的策略，
# 真正结果仍以进程退出码判定（脚本随后直接退出，无需恢复）。
$ErrorActionPreference='Continue'
$hostExe=Join-Path $PSScriptRoot 'FlareEngine.Bootstrap.exe'
if(!(Test-Path -LiteralPath $hostExe)){$hostExe=Join-Path $PSScriptRoot 'SelfEngine.Bootstrap.exe'}
& $hostExe @arguments
exit $LASTEXITCODE
