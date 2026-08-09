param(
    [switch]$Clean,
    [ValidateSet("Debug", "ReleaseNoMinify")]
    [string]$Configuration = "ReleaseNoMinify"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$androidProject = Join-Path $root "Source\Android"
$distRoot = Join-Path $root "dist"

function Assert-Path([string]$Path, [string]$Label) {
    if (-not (Test-Path $Path)) {
        throw "$Label not found: $Path"
    }
}

function Resolve-AndroidSdk {
    $candidates = @(
        $env:ANDROID_HOME,
        $env:ANDROID_SDK_ROOT,
        (Join-Path $env:LOCALAPPDATA "Android\Sdk")
    ) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique

    $candidateList = @($candidates)
    if ($candidateList.Count -eq 0) {
        throw "Android SDK not found. Set ANDROID_HOME or ANDROID_SDK_ROOT."
    }
    return $candidateList[0]
}

function Resolve-JavaHome {
    if ($env:JAVA_HOME -and (Test-Path (Join-Path $env:JAVA_HOME "bin\java.exe"))) {
        return $env:JAVA_HOME
    }

    $candidates = @(
        (Join-Path $env:ProgramFiles "Android\Android Studio\jbr"),
        (Join-Path $env:ProgramFiles "Android\Android Studio\jre")
    )

    $unityEditors = Join-Path $env:ProgramFiles "Unity\Hub\Editor"
    if (Test-Path $unityEditors) {
        $unityJdks = Get-ChildItem $unityEditors -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName "Editor\Data\PlaybackEngines\AndroidPlayer\OpenJDK" }
        $candidates += $unityJdks
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate "bin\java.exe"))) {
            return $candidate
        }
    }

    throw "JDK not found. Set JAVA_HOME to a JDK 17+ installation."
}

$androidHome = Resolve-AndroidSdk
$javaHome = Resolve-JavaHome
$ndkHome = Join-Path $androidHome "ndk\29.0.14206865"
$cmakeHome = Join-Path $androidHome "cmake\3.22.1"

Assert-Path (Join-Path $javaHome "bin\java.exe") "Java"
Assert-Path $ndkHome "Android NDK 29.0.14206865"
Assert-Path $cmakeHome "Android CMake 3.22.1"
Assert-Path (Join-Path $androidProject "gradlew.bat") "Gradle wrapper"

$env:JAVA_HOME = $javaHome
$env:ANDROID_HOME = $androidHome
$env:ANDROID_SDK_ROOT = $androidHome
$env:ANDROID_NDK_HOME = $ndkHome
$env:PATH = "$javaHome\bin;$androidHome\platform-tools;$cmakeHome\bin;$env:PATH"

Write-Host "Animal Crossing VR/MR Standalone - Quest build" -ForegroundColor Cyan
Write-Host "Root:       $root"
Write-Host "Android SDK: $androidHome"
Write-Host "JDK:         $javaHome"
Write-Host "Variant:     Quest$Configuration"

# Ensure bundled dependencies are present when the repository was cloned without --recursive.
Push-Location $root
try {
    & git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) {
        throw "git submodule update failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

# Keep the game-specific profiles mirrored into the Android asset tree before Gradle packages it.
$androidSysAssetsDir = Join-Path $root "Source\Android\app\src\main\assets\Sys"
$androidGameSettingsDir = Join-Path $androidSysAssetsDir "GameSettings"
$androidGameSettingsVrDir = Join-Path $androidSysAssetsDir "GameSettingsVR"
New-Item -ItemType Directory -Force -Path $androidGameSettingsDir | Out-Null
New-Item -ItemType Directory -Force -Path $androidGameSettingsVrDir | Out-Null

$gameSettings = Join-Path $root "Data\Sys\GameSettings\GAFE01.ini"
$marker = Join-Path $root "Data\Sys\GameSettings\GAFE01.ACVR.txt"
$vrProfile = Join-Path $root "Data\Sys\GameSettingsVR\GAFE01.ini"

Assert-Path $gameSettings "GAFE01 game settings"
Assert-Path $marker "ACVR marker"
Assert-Path $vrProfile "GAFE01 VR profile"

Copy-Item -Force $gameSettings (Join-Path $androidGameSettingsDir "GAFE01.ini")
Copy-Item -Force $marker (Join-Path $androidGameSettingsDir "GAFE01.ACVR.txt")
Copy-Item -Force $vrProfile (Join-Path $androidGameSettingsVrDir "GAFE01.ini")

if ($Clean) {
    Push-Location $androidProject
    try {
        & ".\gradlew.bat" clean
        if ($LASTEXITCODE -ne 0) {
            throw "Gradle clean failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }
}

$task = if ($Configuration -eq "Debug") {
    "app:assembleQuestDebug"
} else {
    "app:assembleQuestReleaseNoMinify"
}

Push-Location $androidProject
try {
    & ".\gradlew.bat" $task "--stacktrace"
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

$apk = if ($Configuration -eq "Debug") {
    Join-Path $androidProject "app\build\outputs\apk\quest\debug\app-quest-debug.apk"
} else {
    Join-Path $androidProject "app\build\outputs\apk\quest\releaseNoMinify\app-quest-releaseNoMinify.apk"
}

Assert-Path $apk "Quest APK"
New-Item -ItemType Directory -Force -Path $distRoot | Out-Null
$outApk = Join-Path $distRoot "AnimalCrossingVR-Quest3.apk"
Copy-Item -Force $apk $outApk

$hash = (Get-FileHash -Algorithm SHA256 $outApk).Hash
Write-Host "" 
Write-Host "Build complete" -ForegroundColor Green
Write-Host "APK:    $outApk" -ForegroundColor Green
Write-Host "SHA256: $hash" -ForegroundColor Green
