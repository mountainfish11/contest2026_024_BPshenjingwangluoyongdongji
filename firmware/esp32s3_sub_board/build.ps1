# ESP32-S3 Sub Board Build Script
# Usage: .\build.ps1

param(
    [string]$Action = "build",
    [string]$Port = "COM3"
)

# Check if IDF_PATH is set
if (-not $env:IDF_PATH) {
    Write-Host "Error: IDF_PATH environment variable is not set." -ForegroundColor Red
    Write-Host "Please set IDF_PATH to your ESP-IDF installation directory." -ForegroundColor Yellow
    Write-Host "Example: `$env:IDF_PATH = 'C:\Users\$env:USERNAME\esp\esp-idf'" -ForegroundColor Yellow
    exit 1
}

# Source ESP-IDF environment
$idfExportScript = Join-Path $env:IDF_PATH "export.ps1"
if (Test-Path $idfExportScript) {
    Write-Host "Setting up ESP-IDF environment..." -ForegroundColor Cyan
    & $idfExportScript
} else {
    Write-Host "Warning: ESP-IDF export.ps1 not found at $idfExportScript" -ForegroundColor Yellow
    Write-Host "Please run ESP-IDF environment setup manually." -ForegroundColor Yellow
}

# Navigate to project directory
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

switch ($Action.ToLower()) {
    "build" {
        Write-Host "Building ESP32-S3 Sub Board..." -ForegroundColor Green
        idf.py set-target esp32s3
        idf.py build
    }
    "flash" {
        Write-Host "Flashing to port $Port..." -ForegroundColor Green
        idf.py -p $Port flash
    }
    "monitor" {
        Write-Host "Monitoring on port $Port..." -ForegroundColor Green
        idf.py -p $Port monitor
    }
    "menuconfig" {
        Write-Host "Opening menuconfig..." -ForegroundColor Green
        idf.py menuconfig
    }
    "clean" {
        Write-Host "Cleaning build files..." -ForegroundColor Green
        idf.py fullclean
    }
    default {
        Write-Host "Usage: .\build.ps1 [-Action <build|flash|monitor|menuconfig|clean>] [-Port <COMx>]" -ForegroundColor Yellow
    }
}
