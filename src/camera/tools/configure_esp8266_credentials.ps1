[CmdletBinding()]
param(
    [switch] $Clear
)

$ErrorActionPreference = "Stop"

$cameraRoot = Split-Path -Parent $PSScriptRoot
$sourceDir = Join-Path $cameraRoot "src"
$targetPath = Join-Path $sourceDir "esp_at_credentials.h"
$ignorePath = Join-Path $cameraRoot ".gitignore"
$utf8 = [System.Text.UTF8Encoding]::new($false)

function ConvertTo-CStringText {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string] $Value
    )

    $builder = [System.Text.StringBuilder]::new()
    foreach ($character in $Value.ToCharArray()) {
        $code = [int] $character
        if ($code -eq 34) {
            [void] $builder.Append('\"')
        }
        elseif ($code -eq 92) {
            [void] $builder.Append('\\')
        }
        elseif (($code -lt 32) -or ($code -eq 127)) {
            throw "Wi-Fi credentials cannot contain control characters."
        }
        else {
            [void] $builder.Append($character)
        }
    }

    return $builder.ToString()
}

if (-not (Test-Path -LiteralPath $ignorePath)) {
    throw "Camera .gitignore is missing: $ignorePath"
}
$ignoreText = [System.IO.File]::ReadAllText($ignorePath, $utf8)
if ($ignoreText -notmatch '(?m)^/src/esp_at_credentials\.h\s*$') {
    throw "Refusing to write credentials because esp_at_credentials.h is not Git-ignored."
}

$configured = 0
$ssidText = ""
$passwordText = ""
if (-not $Clear) {
    $ssidText = Read-Host "Enter the 2.4 GHz Wi-Fi SSID"
    if ([string]::IsNullOrWhiteSpace($ssidText)) {
        throw "Wi-Fi SSID cannot be empty."
    }

    $securePassword = Read-Host "Enter the Wi-Fi password (input is hidden)" -AsSecureString
    $passwordPointer = [IntPtr]::Zero
    try {
        $passwordPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($securePassword)
        $passwordText = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($passwordPointer)
    }
    finally {
        if ($passwordPointer -ne [IntPtr]::Zero) {
            [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($passwordPointer)
        }
    }

    $ssidBytes = $utf8.GetByteCount($ssidText)
    $passwordBytes = $utf8.GetByteCount($passwordText)
    if (($ssidBytes -lt 1) -or ($ssidBytes -gt 32)) {
        throw "Wi-Fi SSID must be 1-32 UTF-8 bytes."
    }
    if ($passwordBytes -gt 64) {
        throw "Wi-Fi password must not exceed 64 UTF-8 bytes."
    }
    $configured = 1
}

$escapedSsid = ConvertTo-CStringText -Value $ssidText
$escapedPassword = ConvertTo-CStringText -Value $passwordText
$header = @"
#ifndef ESP_AT_CREDENTIALS_H_
#define ESP_AT_CREDENTIALS_H_

#define ESP_AT_WIFI_CREDENTIALS_CONFIGURED  ($configured)
#define ESP_AT_WIFI_SSID                    "$escapedSsid"
#define ESP_AT_WIFI_PASSWORD                "$escapedPassword"

#endif /* ESP_AT_CREDENTIALS_H_ */
"@

[System.IO.File]::WriteAllText($targetPath, $header, $utf8)
$passwordText = $null

if ($Clear) {
    Write-Output "ESP8266 local credentials cleared. Firmware will remain in credentials-wait."
}
else {
    Write-Output "ESP8266 local credentials configured in the ignored header. Rebuild before flashing."
}
