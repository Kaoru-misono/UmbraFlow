[CmdletBinding()]
param(
    [string]$Preset = $(if ($env:CPP_PRESET) { $env:CPP_PRESET } else { "x64-debug" }),
    [string]$Target = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot

try {
    python scripts/fix_format.py --check
    python scripts/check_modules.py
    python scripts/check_safety.py
    cmake --preset $Preset

    if ($Target) {
        cmake --build --preset $Preset --target $Target
    } else {
        cmake --build --preset $Preset
    }

    ctest --test-dir "build/$Preset" -L CI --output-on-failure
    Write-Output "GATE: PASS"
} finally {
    Pop-Location
}
