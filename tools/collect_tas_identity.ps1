# Hashes local files only. Does not launch, attach to, stop, or modify the game.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [Parameter(Mandatory=$true)][string]$OutputFile,
    [Parameter(Mandatory=$true)][string[]]$Archives
)
$ErrorActionPreference = 'Stop'
function Get-Identity([string]$Path) {
    $item = Get-Item -LiteralPath $Path
    if ($item.PSIsContainer) { throw "Expected file: $Path" }
    $stream = [System.IO.File]::Open($item.FullName, 'Open', 'Read', 'Read')
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $digest = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
        $size = $stream.Length
    } finally { $sha.Dispose(); $stream.Dispose() }
    [ordered]@{ name=$item.Name; bytes=$size; sha256=$digest; lastWriteUtc=$item.LastWriteTimeUtc.ToString('o') }
}
$files = @((Join-Path $GameDirectory 'BBCF.exe'), (Join-Path $GameDirectory 'dinput8.dll')) + $Archives
$identities = @($files | ForEach-Object { Get-Identity $_ })
$result = [ordered]@{
    schema='BBCF_TAS_FILE_IDENTITY_1'; collectedUtc=[DateTime]::UtcNow.ToString('o')
    note='Disk files only. Does not prove loaded-image identity, match lifetime or resource ownership.'
    files=$identities
} | ConvertTo-Json -Depth 5
# CreateNew refuses overwriting an earlier evidence record. Parent must already exist.
$output = [System.IO.File]::Open([System.IO.Path]::GetFullPath($OutputFile), 'CreateNew', 'Write', 'None')
try {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($result)
    $output.Write($bytes, 0, $bytes.Length)
    $output.Flush()
} finally { $output.Dispose() }
Write-Output "Identity written: $OutputFile"
