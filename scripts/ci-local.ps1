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

    # A developer works on this machine while the gate runs, so the build is
    # capped at half the logical processors rather than Ninja's default of all
    # of them. CI invokes `cmake --build --preset ...` directly and never this
    # script, so its runners stay uncapped.
    $BuildJobs = [Math]::Max(2, [Environment]::ProcessorCount / 2)

    if ($Target) {
        cmake --build --preset $Preset --target $Target -j $BuildJobs
    } else {
        cmake --build --preset $Preset -j $BuildJobs
    }
    Assert-NativeSuccess "CMake build" $LASTEXITCODE

    # Six, not one and not the whole machine. CTest runs one test at a time by
    # default, which spends 362 seconds on a suite whose critical path is its
    # slowest single test; six workers finish it in 86. Past roughly six the
    # extra processes buy no wall clock and only take the machine away from the
    # developer sitting at it. Every gate registered here therefore has to hold
    # its TIMEOUT with five siblings running beside it, which is why the two
    # fattest binaries are registered as index shards in tests/CMakeLists.txt.
    # The cap lives here rather than in CMakePresets.json for the same reason
    # the build cap above does: CI invokes ctest directly and stays uncapped.
    $TestJobs = 6

    ctest --test-dir "build/$Preset" -L CI --output-on-failure --parallel $TestJobs
    Assert-NativeSuccess "CTest" $LASTEXITCODE
    Write-Output "GATE: PASS"
} finally {
    Pop-Location
}
