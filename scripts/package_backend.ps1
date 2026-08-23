$ErrorActionPreference = "Stop"

$ProjectRoot = Resolve-Path "$PSScriptRoot\.."
$AppBackendDir = "$ProjectRoot\app\resources\backend"
$ServerReleaseDir = "$ProjectRoot\build\server\Release"
$BinReleaseDir = "$ProjectRoot\build\bin\Release"
$VisionReleaseDir = "$ProjectRoot\build\third_party\vision.cpp\src\visp\Release"

Write-Host "Preparing backend packaging..."

# 1. Create or empty the backend directory
if (Test-Path $AppBackendDir) {
    Remove-Item -Path "$AppBackendDir\*" -Recurse -Force
} else {
    New-Item -ItemType Directory -Path $AppBackendDir | Out-Null
}

# 2. Copy the executable
Write-Host "Copying vllama_server.exe..."
if (Test-Path "$ServerReleaseDir\vllama_server.exe") {
    Copy-Item "$ServerReleaseDir\vllama_server.exe" -Destination $AppBackendDir -Force
} else {
    Write-Error "vllama_server.exe not found! Please compile the server first."
}

# 3. Copy project DLLs
Write-Host "Copying project DLLs..."
if (Test-Path $BinReleaseDir) {
    Copy-Item "$BinReleaseDir\*.dll" -Destination $AppBackendDir -Force
}
if (Test-Path $VisionReleaseDir) {
    Copy-Item "$VisionReleaseDir\*.dll" -Destination $AppBackendDir -Force
}

# 4. Find CUDA Toolkit and copy runtime DLLs
Write-Host "Looking for CUDA Toolkit..."
$CudaPath = $null

if ($env:CUDA_PATH -and (Test-Path $env:CUDA_PATH)) {
    $CudaPath = $env:CUDA_PATH
} else {
    # Check common locations
    $CommonLocations = @(
        "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.3",
        "D:\Nvidia CUDA",
        "D:\NVIDIA CUDA"
    )
    foreach ($loc in $CommonLocations) {
        if (Test-Path $loc) {
            $CudaPath = $loc
            break
        }
    }
}

if ($CudaPath) {
    Write-Host "Found CUDA Toolkit at: $CudaPath"
    $CudaBin = "$CudaPath\bin"
    
    $CudaDlls = @(
        "cudart64_12.dll",
        "cublas64_12.dll",
        "cublasLt64_12.dll",
        "cusparse64_12.dll"
    )

    foreach ($dll in $CudaDlls) {
        $dllPath = "$CudaBin\$dll"
        if (Test-Path $dllPath) {
            Write-Host "Copying $dll..."
            Copy-Item $dllPath -Destination $AppBackendDir -Force
        } else {
            Write-Warning "Missing CUDA runtime DLL: $dll"
        }
    }
} else {
    Write-Warning "Could not find CUDA Toolkit automatically! The packaged app may crash on user machines if they don't have CUDA installed."
}

Write-Host "Backend packaging complete!"
