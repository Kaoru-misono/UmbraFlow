[CmdletBinding()]
param(
    [string]$Preset = $(if ($env:CPP_PRESET) { $env:CPP_PRESET } else { "x64-debug" }),
    [string]$Target = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot

function Assert-NativeSuccess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Step,
        [Parameter(Mandatory = $true)]
        [int]$ExitCode
    )

    if ($ExitCode -ne 0) {
        throw "$Step failed with exit code $ExitCode."
    }
}

try {
    python scripts/fix_format.py --check
    Assert-NativeSuccess "Source normalization check" $LASTEXITCODE
    python scripts/check_cpp_format.py
    Assert-NativeSuccess "C++ format check" $LASTEXITCODE
    python scripts/check_modules.py
    Assert-NativeSuccess "Module check" $LASTEXITCODE
    python scripts/check_safety.py
    Assert-NativeSuccess "Safety check" $LASTEXITCODE
    python scripts/check_luau_identity.py
    Assert-NativeSuccess "Luau environment identity check" $LASTEXITCODE
    python scripts/generate_unicode_luau.py --check
    Assert-NativeSuccess "Pinned Unicode Luau check" $LASTEXITCODE
    python scripts/generate_public_contract.py --check
    Assert-NativeSuccess "Public contract check" $LASTEXITCODE
    cmake --preset $Preset
    Assert-NativeSuccess "CMake configure" $LASTEXITCODE

    if ($Target) {
        cmake --build --preset $Preset --target $Target
    } else {
        cmake --build --preset $Preset
    }
    Assert-NativeSuccess "CMake build" $LASTEXITCODE

    ctest --test-dir "build/$Preset" -L CI --output-on-failure
    Assert-NativeSuccess "CTest" $LASTEXITCODE
    Write-Output "GATE: PASS"
} finally {
    Pop-Location
}
