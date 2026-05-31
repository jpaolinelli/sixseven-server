# build-win.ps1 — one-shot Windows configure+build for SixSevenDB.
# Self-contained: puts VS-bundled cmake/ninja/clang on PATH, initializes the
# MSVC + Windows SDK environment (Clang needs it on Windows), then configures,
# builds, and runs the unit tests. All output is appended to build\win-build.log
# and a RESULT line with exit codes is written at the end.
$ErrorActionPreference = "Continue"
$root = $PSScriptRoot
$log  = Join-Path $root "build\win-build.log"
New-Item -ItemType Directory -Force -Path (Join-Path $root "build") | Out-Null
"" | Set-Content -Path $log -Encoding utf8
function Log($m) { Add-Content -Path $log -Value $m -Encoding utf8 }

# 1) VS-bundled tools on PATH (this process only).
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
$toolDirs = @(
  "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin",
  "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja",
  "$vs\VC\Tools\Llvm\x64\bin"
)
$env:Path = ($toolDirs -join ';') + ';' + $env:Path
Log ("cmake  = " + (Get-Command cmake  -ErrorAction SilentlyContinue).Source)
Log ("ninja  = " + (Get-Command ninja  -ErrorAction SilentlyContinue).Source)
Log ("clang++= " + (Get-Command clang++ -ErrorAction SilentlyContinue).Source)

# 2) MSVC + Windows SDK environment.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
Log ("VSCMD_VER = " + $env:VSCMD_VER)

Set-Location $root

# 3) Configure (vcpkg installs all manifest deps on first run — can be slow).
Log "=== CONFIGURE ==="
cmake --preset default *>> $log
$cfg = $LASTEXITCODE
Log "CONFIGURE_EXIT=$cfg"

$bld = -1
if ($cfg -eq 0) {
  Log "=== BUILD ==="
  cmake --build build/debug *>> $log
  $bld = $LASTEXITCODE
  Log "BUILD_EXIT=$bld"
}

$tst = -1
if ($bld -eq 0) {
  Log "=== UNIT TESTS ==="
  $exe = Join-Path $root "build\debug\tests\unit\sixseven_unit_tests.exe"
  if (Test-Path $exe) { & $exe *>> $log; $tst = $LASTEXITCODE } else { Log "test exe not found at $exe" }
  Log "TESTS_EXIT=$tst"
}

Log "RESULT configure=$cfg build=$bld tests=$tst"
