param(
    [string]$ArduinoCli = "$env:LOCALAPPDATA/Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe",
    [string]$ArduinoData = "$env:LOCALAPPDATA/Arduino15"
)

$ErrorActionPreference = 'Stop'
$firmware = Join-Path $PSScriptRoot '../firmware/NexusS3'
$fqbn = 'esp32:esp32:esp32s3:UploadSpeed=115200,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=160,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default'
$partitionHash = 'ACE02503447D0F470692E65FA76002F2D77A92DC81CD3813D8AA66718D716DA9'
$oldData = $env:ARDUINO_DIRECTORIES_DATA

try {
    $env:ARDUINO_DIRECTORIES_DATA = $ArduinoData
    $cliVersion = & $ArduinoCli version
    if ($LASTEXITCODE -ne 0 -or $cliVersion -notmatch 'Version: 1\.6\.0\b') {
        throw 'Arduino CLI 1.6.0 is required for the recorded build profile.'
    }
    $cores = & $ArduinoCli core list
    if ($LASTEXITCODE -ne 0 -or -not ($cores -match '^esp32:esp32\s+3\.3\.12\s')) {
        throw 'The installed ESP32 Arduino core must be 3.3.12.'
    }
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff')
    $build = New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot "../build/esp32s3-$stamp")
    $buildPath = $build.FullName
    Write-Host "Build directory $buildPath"
    $source = Join-Path $buildPath 'source/NexusS3'
    New-Item -ItemType Directory -Path $source | Out-Null
    Copy-Item -Path (Join-Path $firmware '*') -Destination $source
    $assets = [ordered]@{ index = 'index.html'; style = 'style.css'; app = 'app.js' }
    $header = @('#pragma once', 'namespace nexus::web {')
    foreach ($asset in $assets.GetEnumerator()) {
        $content = Get-Content -Raw -Encoding utf8 -LiteralPath (Join-Path $PSScriptRoot "../web/$($asset.Value)")
        if ($content.Contains(')NEXUS"')) { throw "Reserved asset delimiter in $($asset.Value)." }
        $header += 'inline constexpr char {0}[] = R"NEXUS({1})NEXUS";' -f $asset.Key, $content
    }
    $header += '}'
    [IO.File]::WriteAllLines((Join-Path $source 'web_assets.h'), $header, [Text.UTF8Encoding]::new($false))
    & $ArduinoCli compile --fqbn $fqbn --warnings default --build-path $buildPath `
        --build-property 'compiler.c.elf.extra_flags=-Wl,--wrap=initArduino' $source 2>&1 |
        Tee-Object -FilePath (Join-Path $buildPath 'compile.log')
    if ($LASTEXITCODE -ne 0) {
        throw "ESP32-S3 compile failed. See $buildPath/compile.log."
    }
    $partition = Join-Path $buildPath 'NexusS3.ino.partitions.bin'
    if ((Get-FileHash -LiteralPath $partition).Hash -ne $partitionHash) {
        throw 'Build partition image differs from the qualified v2.3 partition image.'
    }
    $app = Get-Item -LiteralPath (Join-Path $buildPath 'NexusS3.ino.bin')
    if ($app.Length -gt 0x300000) {
        throw 'Application image exceeds the 3 MiB app slot.'
    }
    $elf = Join-Path $buildPath 'NexusS3.ino.elf'
    $map = Join-Path $buildPath 'NexusS3.ino.map'
    $nm = Join-Path $ArduinoData 'packages/esp32/tools/esp-x32/2601/bin/xtensa-esp32s3-elf-nm.exe'
    $symbols = & $nm --defined-only $elf
    if ($LASTEXITCODE -ne 0 -or -not ($symbols -match '\bT __wrap_initArduino$') -or
        ($symbols -match '\binitArduino$')) {
        throw 'Could not verify exclusion of the Arduino initialization hook.'
    }
    [ordered]@{
        arduinoCli = $cliVersion
        arduinoCore = '3.3.12'
        fqbn = $fqbn
        appBytes = $app.Length
        slotBytes = 0x300000
        sha256 = [ordered]@{
            app = (Get-FileHash -LiteralPath $app.FullName).Hash
            elf = (Get-FileHash -LiteralPath $elf).Hash
            map = (Get-FileHash -LiteralPath $map).Hash
            partitions = $partitionHash
        }
    } | ConvertTo-Json | Set-Content -Encoding utf8 -LiteralPath (Join-Path $buildPath 'manifest.json')
    Write-Host "Verified build-only app image: $($app.Length) of 3145728 bytes."
} finally {
    $env:ARDUINO_DIRECTORIES_DATA = $oldData
}
