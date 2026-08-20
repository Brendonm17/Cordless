# set-token.ps1 - write your Discord token to the Vita card for Cordless.
# Validates the token, confirms the account, finds the card, writes token.txt.
#
# Usage:
#   .\set-token.ps1                 # prompt for the token, auto-find the card
#   .\set-token.ps1 -Drive D:       # force a specific drive
#   .\set-token.ps1 -SkipCheck      # skip the Discord verify call
[CmdletBinding()]
param(
    [string]$Token,
    [string]$Drive,
    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# --- 1. get the token ------------------------------------------------------
# Accept a bare token or a copied DevTools request; pull the token from
# the Authorization header. Check the clipboard, then prompt.
function Get-DiscordToken([string]$text) {
    if (-not $text) { return $null }
    $t = $text.Trim()
    if ($t -match '^[A-Za-z0-9_\-]{20,}\.[A-Za-z0-9_\-]{5,}\.[A-Za-z0-9_\-]{20,}$') { return $t }
    $m = [regex]::Match($text,
         'authorization["'']?\s*[:=]\s*["'']?\s*([A-Za-z0-9_.\-]{50,100})',
         [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}

if (-not $Token) {
    $clipTok = Get-DiscordToken (Get-Clipboard -Raw -ErrorAction SilentlyContinue)
    if ($clipTok) {
        Write-Host 'Found a Discord token on your clipboard (from a copied request or a bare token).' -ForegroundColor Cyan
        if ((Read-Host 'Use it? (Y/n)') -ne 'n') { $Token = $clipTok }
    }
}
if (-not $Token) {
    $sec  = Read-Host 'Paste your token (or a copied cURL / fetch request)' -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
    try   { $raw = [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
    $Token = Get-DiscordToken $raw
    if (-not $Token) { $Token = "$raw".Trim() }
}
$Token = $Token.Trim()

# --- 2. sanity-check the shape (base64url.base64url.base64url) --------------
$dots = ([regex]::Matches($Token, '\.')).Count
if ($Token.Length -lt 50 -or $Token.Length -gt 100 -or $dots -ne 2 -or $Token -match '\s') {
    Write-Host "That doesn't look like a Discord user token." -ForegroundColor Yellow
    Write-Host "Expected ~59-80 characters, exactly 2 dots, no spaces. Re-copy the whole" -ForegroundColor Yellow
    Write-Host "'authorization' header value and try again." -ForegroundColor Yellow
    return
}

# --- 3. confirm it works + whose account it is -----------------------------
if (-not $SkipCheck) {
    try {
        $me = Invoke-RestMethod -Uri 'https://discord.com/api/v10/users/@me' `
                -Headers @{ Authorization = $Token } -TimeoutSec 15
        $name = if ($me.global_name) { $me.global_name } else { $me.username }
        Write-Host ("Token OK - {0} (@{1}, id {2})" -f $name, $me.username, $me.id) -ForegroundColor Green
    } catch {
        $status = $null
        if ($_.Exception.Response) { $status = [int]$_.Exception.Response.StatusCode }
        if ($status -eq 401) {
            Write-Host "Discord rejected this token (401) - it's expired or mistyped. Not writing it." -ForegroundColor Red
            return
        }
        Write-Host ("Couldn't reach Discord to verify ({0}). Network problem?" -f $_.Exception.Message) -ForegroundColor Yellow
        if ((Read-Host 'Write it to the card anyway? (y/N)') -ne 'y') { return }
    }
}

# --- 4. find the Vita card (a drive that already has data\cordless) ---------
function Find-CordlessDrives {
    Get-PSDrive -PSProvider FileSystem |
        Where-Object { $_.Root -match '^[A-Za-z]:\\$' -and (Test-Path (Join-Path $_.Root 'data\cordless')) } |
        ForEach-Object { $_.Root.Substring(0, 2).ToUpper() }
}

if (-not $Drive) {
    $cands = @(Find-CordlessDrives)
    if ($cands.Count -eq 1) {
        $Drive = $cands[0]
        Write-Host ("Found your Cordless card at {0}" -f $Drive) -ForegroundColor Cyan
    } elseif ($cands.Count -gt 1) {
        Write-Host ("More than one card has a Cordless folder: {0}" -f ($cands -join ', ')) -ForegroundColor Yellow
        $Drive = Read-Host 'Which drive is the Vita you want to write to? (e.g. D:)'
    } else {
        $Drive = Read-Host 'Enter your Vita card drive letter (e.g. D:)'
    }
}
if ($Drive -notmatch '^[A-Za-z]:?$') { Write-Host "Not a drive letter: '$Drive'." -ForegroundColor Red; return }
$Drive = $Drive.Substring(0, 1).ToUpper() + ':'

$dir  = Join-Path $Drive 'data\cordless'
$path = Join-Path $dir  'token.txt'

# --- 5. confirm the target, then write exact bytes (ASCII, no BOM/newline) --
Write-Host ""
Write-Host ("About to write your token to:  {0}" -f $path) -ForegroundColor Cyan
if ((Read-Host 'Proceed? (y/N)') -ne 'y') { Write-Host 'Cancelled - nothing written.'; return }

if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
[System.IO.File]::WriteAllText($path, $Token, (New-Object System.Text.ASCIIEncoding))

$wrote = [System.IO.File]::ReadAllBytes($path).Length
Write-Host ("Wrote {0} bytes to {1}" -f $wrote, $path) -ForegroundColor Green
Write-Host "Eject the card, put it in the Vita, and launch Cordless." -ForegroundColor Green
