param(
    [string]$Port = 'COM3',
    [switch]$Status,
    [switch]$Clear
)
$ErrorActionPreference = 'Stop'

function ConvertFrom-SecurePlain([Security.SecureString]$sec) {
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

function Send-FieldLine([IO.Ports.SerialPort]$port, [string]$line, [string]$pattern, [int]$seconds = 6) {
    Start-Sleep -Milliseconds 150
    $null = $port.ReadExisting()
    $port.Write("$line`n")
    $end = [DateTime]::UtcNow.AddSeconds($seconds)
    $buf = ''
    do {
        $buf += $port.ReadExisting()
        if ($buf -match $pattern) {
            return ($buf -split "`r?`n" | Where-Object { $_ -match '^FIELD_' }) -join "`n"
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $end)
    throw "Timed out waiting for board reply ($pattern)."
}

$p = [IO.Ports.SerialPort]::new($Port, 115200)
try {
    $p.Open()
    Start-Sleep -Milliseconds 1200
    if ($Status) {
        Write-Host (Send-FieldLine $p ':field' 'FIELD_MODE ')
        return
    }
    if ($Clear) {
        Write-Host (Send-FieldLine $p ':clear' 'FIELD_MODE CLEARED|FIELD_ERROR ')
        return
    }

    $wifiSec = Read-Host -AsSecureString 'Enter Standalone Field Wi-Fi WPA2 password (12-63 ASCII chars)'
    $wifiConfirm = Read-Host -AsSecureString 'Confirm Wi-Fi WPA2 password'
    $wifi = ConvertFrom-SecurePlain $wifiSec
    $wifi2 = ConvertFrom-SecurePlain $wifiConfirm
    if ($wifi -ne $wifi2) { throw 'Wi-Fi passwords did not match.' }
    if ($wifi.Length -lt 12 -or $wifi.Length -gt 63) { throw 'Wi-Fi password must be 12 to 63 ASCII characters.' }

    $passSec = Read-Host -AsSecureString 'Enter Standalone Field Operator passphrase (8-32 ASCII chars)'
    $passConfirm = Read-Host -AsSecureString 'Confirm Operator passphrase'
    $pass = ConvertFrom-SecurePlain $passSec
    $pass2 = ConvertFrom-SecurePlain $passConfirm
    if ($pass -ne $pass2) { throw 'Operator passphrases did not match.' }
    if ($pass.Length -lt 8 -or $pass.Length -gt 32) { throw 'Operator passphrase must be 8 to 32 ASCII characters.' }

    Write-Host (Send-FieldLine $p ":wifi $wifi" 'FIELD_WIFI SAVED|FIELD_ERROR ')
    Write-Host (Send-FieldLine $p ":pass $pass" 'FIELD_PASS SAVED|FIELD_ERROR ' 10)
    Write-Host 'Standalone Field Mode configured. Hold BOOT (1.5 s) to start AP without a laptop; press BOOT to stop.'
} finally {
    $wifi = $null; $wifi2 = $null; $pass = $null; $pass2 = $null
    if ($p.IsOpen) { $p.Close() }
    $p.Dispose()
}
