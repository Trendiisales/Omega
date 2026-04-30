# cmake-discover.ps1 -- bulletproof cmake.exe discovery, dot-sourced from QUICK_RESTART.ps1.
# Sets $cmakeExe; exits 1 with [FATAL] if not found. Glob-based so cmake version bumps
# do not break the build. Multiple candidate locations checked in priority order.
$_globs = @(
    "C:\vcpkg\vcpkg\downloads\tools\cmake-*-windows\cmake-*-windows-*\bin\cmake.exe",
    "C:\vcpkg\downloads\tools\cmake-*-windows\cmake-*-windows-*\bin\cmake.exe",
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)
$cmakeExe = $null
foreach ($g in $_globs) {
    $m = Get-ChildItem -Path $g -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($m) { $cmakeExe = $m.FullName; break }
}
if (-not $cmakeExe) {
    $p = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($p) { $cmakeExe = $p.Source }
}
if (-not $cmakeExe -or -not (Test-Path $cmakeExe)) {
    Write-Host "[FATAL] cmake.exe not found. Searched:" -ForegroundColor Red
    $_globs | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    Write-Host "  - PATH (Get-Command cmake.exe)" -ForegroundColor Red
    exit 1
}
Write-Host "[cmake] Using $cmakeExe" -ForegroundColor DarkGreen
