param(
    [Parameter(Mandatory=$true)][ValidateSet('NewKey','Issue')][string]$Action,
    [Parameter(Mandatory=$true)][string]$PrivateKeyPath,
    [string]$PublicHeaderPath,
    [string]$Email,
    [string]$OutputPath
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$privateFile = [IO.Path]::GetFullPath($PrivateKeyPath)
$scope = [Security.Cryptography.DataProtectionScope]::CurrentUser
$key = $null
$signer = $null
try {
    if ($Action -eq 'NewKey') {
        if (!$PublicHeaderPath) { throw 'PublicHeaderPath is required' }
        if (Test-Path -LiteralPath $privateFile) { throw 'Refusing to overwrite a signing key' }
        if (Test-Path -LiteralPath $PublicHeaderPath) { throw 'Refusing to overwrite a public key' }
        $parameters = New-Object Security.Cryptography.CngKeyCreationParameters
        $parameters.ExportPolicy = [Security.Cryptography.CngExportPolicies]::AllowPlaintextExport
        $key = [Security.Cryptography.CngKey]::Create([Security.Cryptography.CngAlgorithm]::ECDsaP256, ('BananaPlayer-Keygen-' + [Guid]::NewGuid().ToString()), $parameters)
        $private = $key.Export([Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
        $protected = [Security.Cryptography.ProtectedData]::Protect($private, $null, $scope)
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($privateFile)) | Out-Null
        [IO.File]::WriteAllBytes($privateFile, $protected)
        [Array]::Clear($private, 0, $private.Length)
        $public = $key.Export([Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
        $hex = ($public | ForEach-Object { '0x{0:x2}' -f $_ }) -join ', '
        $header = "#pragma once`n// ECDSA P-256 public verification key. No signing material.`ninline constexpr unsigned char kLicensePublicKey[] = { $hex };`n"
        $publicFile = [IO.Path]::GetFullPath($PublicHeaderPath)
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($publicFile)) | Out-Null
        [IO.File]::WriteAllText($publicFile, $header, [Text.UTF8Encoding]::new($false))
        Write-Output "Public verification key: $publicFile"
        Write-Output "Signing key protected for this Windows account: $privateFile"
    } else {
        if (!$Email -or !$OutputPath) { throw 'Email and OutputPath are required' }
        $normalized = $Email.Trim().ToLowerInvariant()
        if ($normalized.Length -gt 254 -or $normalized -notmatch '^[\x21-\x7e]+@[\x21-\x7e]+$') { throw 'Use the ASCII email from the purchase receipt' }
        $private = [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($privateFile), $null, $scope)
        try { $key = [Security.Cryptography.CngKey]::Import($private, [Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob) }
        finally { [Array]::Clear($private, 0, $private.Length) }
        $signer = [Security.Cryptography.ECDsaCng]::new($key)
        $signer.HashAlgorithm = [Security.Cryptography.CngAlgorithm]::Sha256
        $signature = $signer.SignData([Text.Encoding]::UTF8.GetBytes('banana-player-license-v5:' + $normalized))
        if ($signature.Length -ne 64) { throw 'Unexpected signature encoding' }
        $license = 'V5-' + [Convert]::ToBase64String($signature).TrimEnd('=').Replace('+','-').Replace('/','_')
        [IO.File]::WriteAllText([IO.Path]::GetFullPath($OutputPath), $license, [Text.UTF8Encoding]::new($false))
        Write-Output "License written to $OutputPath"
    }
} finally {
    if ($signer) { $signer.Dispose() }
    if ($key) { if ($Action -eq 'NewKey') { $key.Delete() }; $key.Dispose() }
}
