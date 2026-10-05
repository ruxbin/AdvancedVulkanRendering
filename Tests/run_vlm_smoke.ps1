param(
  [string]$Renderer='',
  [string]$PythonExe='python'
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(-not $Renderer) { $Renderer=Join-Path $root 'Bin/AdvancedVulkanRendering.exe' }
$Renderer=(Resolve-Path -LiteralPath $Renderer).Path
$outputRelative='.cache/vlm-validation'
$output=Join-Path $root $outputRelative
New-Item -ItemType Directory -Force -Path $output | Out-Null
$validator=Join-Path $PSScriptRoot 'validate_vlm_log.py'

function Run-Renderer($name,[string[]]$arguments,[int]$expectedExit=0) {
  $log=Join-Path $output "$name.log"
  $process=Start-Process -FilePath $Renderer -WorkingDirectory $root -ArgumentList $arguments -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError (Join-Path $output "$name.err") -PassThru
  try {
    $deadline=[DateTime]::UtcNow.AddSeconds(120)
    while(-not $process.WaitForExit(500)) {
      if([DateTime]::UtcNow -gt $deadline) { throw "$name timed out" }
    }
    $text=Get-Content -LiteralPath $log -Raw
    if($process.ExitCode -ne $expectedExit) { throw "$name exit=$($process.ExitCode), expected $expectedExit; see $log" }
    if($text -match 'VUID-|Validation Error') { throw "$name Vulkan validation error; see $log" }
    Write-Output "$name exit=$($process.ExitCode)"
    return $text
  } finally {
    # Stop only the process created by this invocation if its bounded test failed.
    if(-not $process.HasExited) { Stop-Process -Id $process.Id }
  }
}

function Run-Bake($name,[string[]]$extra,$mode='') {
  $asset="$outputRelative/$name.vlm"
  $args=@('--vlm-bake',$asset,'--vlm-bake-volume','camera,1,1,1',
    '--vlm-bake-spacing','1','--vlm-exit-after-bake')+$extra
  $bakeText=(Run-Renderer $name $args) -join "`n"
  $bakeText -split "`n" | Where-Object { $_ -match 'exit=|vlm validate|vlm bake: wrote' } | Write-Output
  $validation=@($validator,(Join-Path $output "$name.log"),'--asset',(Join-Path $root $asset))
  if($mode) { $validation+=@('--mode',$mode) }
  & $PythonExe @validation
  if($LASTEXITCODE -ne 0) { throw "$name acceptance failed" }
}

Run-Bake 'const' @('--vlm-bake-samples','4096','--vlm-bake-sky-only','--vlm-bake-const-env','1,0.5,0.25') 'const'
Run-Bake 'direction' @('--vlm-bake-samples','65536','--vlm-bake-batch-samples','256','--vlm-bake-sky-only') 'direction'
$geometry=@('--vlm-bake-samples','8192','--vlm-bake-batch-samples','64','--vlm-bake-max-bounces','2',
 '--vlm-bake-const-env','1,0.5,0.25','--vlm-local-light-scale','0','--vlm-sun-mode','analytic')
Run-Bake 'dark' ($geometry+@('--vlm-env-scale','0','--vlm-sun-scale','0'))
Run-Bake 'env' ($geometry+@('--vlm-env-scale','1','--vlm-sun-scale','0'))
Run-Bake 'sun' ($geometry+@('--vlm-env-scale','0','--vlm-sun-scale','1'))
Run-Bake 'both' ($geometry+@('--vlm-env-scale','1','--vlm-sun-scale','1'))
$linearity=@($validator,'--linearity')
foreach($name in @('dark','env','sun','both')) { $linearity+=(Join-Path $output "$name.vlm") }
& $PythonExe @linearity
if($LASTEXITCODE -ne 0) { throw 'superposition acceptance failed' }
Run-Bake 'environment-sun' @('--vlm-bake-samples','4096','--vlm-bake-batch-samples','64','--vlm-sun-mode','environment')

$validAsset="$outputRelative/environment-sun.vlm"
$fixture=[System.IO.File]::ReadAllBytes((Join-Path $root $validAsset))
$fixture[$fixture.Length-1]=$fixture[$fixture.Length-1] -bxor 1
[System.IO.File]::WriteAllBytes((Join-Path $output 'corrupt.vlm'),$fixture)
foreach($case in @(
  @{name='load'; args=@('--vlm',$validAsset); expect='vlm: runtime activated'},
  @{name='stale'; args=@('--vlm',$validAsset,'--vlm-sun-mode','analytic'); expect='vlm: stale asset disabled'},
  @{name='corrupt'; args=@('--vlm',"$outputRelative/corrupt.vlm"); expect='vlm: rejected asset disabled'}
)) {
  $text=(Run-Renderer $case.name ($case.args+@('--vlm-validation-frames','30'))) -join "`n"
  if($text -notmatch $case.expect -or $text -notmatch 'vlm validation: rendered 30 frames' -or $text -match 'vlm bake:') {
    throw "$($case.name) load smoke failed"
  }
  if($case.name -ne 'load' -and $text -match 'vlm: runtime activated') { throw 'invalid asset activated' }
  Write-Output "$($case.name): rendered 30 frames and exited normally"
}
$invalidText=(Run-Renderer 'invalid-cli' @('--vlm-bake-samples','0') 2) -join "`n"
$invalidText -split "`n" | Where-Object { $_ -match 'exit=|Invalid or missing' } | Write-Output
$volumeText=(Run-Renderer 'invalid-volume' @('--vlm-bake',"$outputRelative/invalid-volume.vlm",'--vlm-bake-volume','camera,0,1,1','--vlm-validation-frames','1') 1) -join "`n"
if($volumeText -notmatch 'vlm bake: FAILED' -or $volumeText -match 'vlm bake: wrote') { throw 'invalid volume was published' }
$volumeText -split "`n" | Where-Object { $_ -match 'exit=|vlm bake: FAILED' } | Write-Output
Write-Output "VLM smoke passed; artifacts: $output"
