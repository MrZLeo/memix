# Interactive installer for Windows PowerShell 5.1+ and PowerShell 7+.
[CmdletBinding()]
param(
    [string] $Version = 'latest',
    [string] $InstallDir = (Join-Path $HOME '.local/bin'),
    [ValidateSet('claude', 'codex', 'opencode', 'pi', 'dsh')]
    [string[]] $Agent = @(),
    [switch] $Project,
    [switch] $Help
)

function Assert-MemixInteractive {
    if ([Console]::IsInputRedirected) {
        throw 'An interactive terminal is required. Run install.ps1 in a PowerShell terminal.'
    }
}

function Read-MemixResponse([string] $Prompt) {
    Write-Host "$Prompt [Y/n] " -NoNewline
    [Console]::ReadLine()
}

function Confirm-MemixAction([string] $Prompt) {
    while ($true) {
        $answer = Read-MemixResponse $Prompt
        if ($null -eq $answer) { throw 'Confirmation cancelled (EOF); no further actions will be performed.' }
        switch ($answer.Trim().ToLowerInvariant()) {
            '' { return $true }
            'y' { return $true }
            'yes' { return $true }
            'n' { return $false }
            'no' { return $false }
            default { Write-Host 'Please answer y or n.' }
        }
    }
}

function Get-MemixPlatform {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'Use install.sh on Linux or macOS.'
    }
    $architecture = $env:PROCESSOR_ARCHITEW6432
    if (-not $architecture) { $architecture = $env:PROCESSOR_ARCHITECTURE }
    if ($architecture -notin @('AMD64', 'x86_64')) {
        throw "No release binary for Windows/$architecture; no source build will be attempted."
    }
    return @{ Asset = 'memix-windows-x86_64.exe'; Description = 'Windows / x86_64' }
}

function Get-MemixDownloadMethod {
    if (Get-Command gh -CommandType Application -ErrorAction SilentlyContinue) {
        try {
            & gh auth status --hostname github.com *> $null
            if ($LASTEXITCODE -eq 0) { return 'gh' }
        } catch {
            # Windows PowerShell 5.1 may turn native stderr into a terminating error.
            # An unauthenticated gh must not prevent public HTTPS downloads.
        }
    }
    return 'web'
}

function Resolve-MemixRelease([string] $Tag, [string] $Method) {
    if ($Tag -eq 'latest') {
        if ($Method -eq 'gh') {
            $Tag = & gh release view --repo github.com/MrZLeo/memix --json tagName --jq '.tagName'
            if ($LASTEXITCODE -ne 0) { throw 'Could not resolve the latest release; check GitHub authentication.' }
        } else {
            $release = Invoke-RestMethod -Uri 'https://api.github.com/repos/MrZLeo/memix/releases/latest' `
                -Headers @{ Accept = 'application/vnd.github+json'; 'User-Agent' = 'memix-installer' }
            $Tag = $release.tag_name
        }
    }
    if ($Tag -notmatch '^[A-Za-z0-9][A-Za-z0-9._+-]*$') { throw "Invalid release tag: $Tag" }
    return $Tag
}

function Save-MemixAsset([string] $Tag, [string] $Name, [string] $Output, [string] $Method) {
    if ($Method -eq 'gh') {
        & gh release download $Tag --repo github.com/MrZLeo/memix --pattern $Name --output $Output
        if ($LASTEXITCODE -ne 0) { throw "Could not download $Name from release $Tag." }
    } else {
        Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/MrZLeo/memix/releases/download/$Tag/$Name" `
            -OutFile $Output -TimeoutSec 300
    }
}

function Assert-MemixDestination([string] $Path) {
    try { $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop }
    catch [System.Management.Automation.ItemNotFoundException] { return }
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to replace a directory or symlink: $Path"
    }
}

function Invoke-Memix([string] $Binary, [string[]] $Arguments) {
    & $Binary @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Memix command failed (exit $LASTEXITCODE): $($Arguments -join ' ')" }
}

function Test-MemixBinary([string] $Binary) {
    $reported = ((Invoke-Memix $Binary @('--version')) -join "`n").Trim()
    if ($reported -notmatch '^memix [^\r\n]+$') { throw 'Unexpected binary version output.' }
    Invoke-Memix $Binary @('skill', '--help') | Out-Null
    return $reported
}

function Find-MemixBinary([string] $Destination) {
    if (Test-Path -LiteralPath $Destination -PathType Leaf) { return $Destination }
    $existing = Get-Command memix -CommandType Application -ErrorAction SilentlyContinue
    if ($existing) { return $existing.Source }
    return $null
}

function Invoke-MemixInstaller {
    [CmdletBinding()]
    param(
        [string] $Version = 'latest',
        [string] $InstallDir = (Join-Path $HOME '.local/bin'),
        [ValidateSet('claude', 'codex', 'opencode', 'pi', 'dsh')]
        [string[]] $Agent = @(),
        [switch] $Project,
        [switch] $Help
    )
    $ErrorActionPreference = 'Stop'
    if ($Help) {
        Write-Host @'
Usage: .\install.ps1 [-Version TAG] [-InstallDir PATH] [-Agent pi,codex] [-Project]

Install a release binary, then optionally set up the bundled agent skill.
Both steps require an interactive [Y/n] confirmation. There is no -Yes option.
Default: latest release, ~/.local/bin, detected agents, user scope.
Supported: Windows x86_64. Use install.sh on Linux/macOS.
Private releases require an authenticated GitHub CLI (gh).
The installer never elevates privileges, edits PATH, or forces skill replacement.
'@
        return
    }
    $platform = Get-MemixPlatform
    Assert-MemixInteractive
    if (-not [IO.Path]::IsPathRooted($InstallDir) -or $InstallDir -match '^[A-Za-z]:[^\\/]') {
        throw '-InstallDir must be an absolute path.'
    }
    $InstallDir = [IO.Path]::GetFullPath($InstallDir)
    if ($InstallDir.TrimEnd('\', '/') -eq [IO.Path]::GetPathRoot($InstallDir).TrimEnd('\', '/')) {
        throw 'Do not install into the filesystem root.'
    }
    if ($Version -notmatch '^[A-Za-z0-9][A-Za-z0-9._+-]*$') { throw "Invalid release tag: $Version" }
    $destination = Join-Path $InstallDir 'memix.exe'
    Assert-MemixDestination $destination
    $stage = $null
    $oldProtocol = [Net.ServicePointManager]::SecurityProtocol
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Write-Host "`nMemix installer`n`n1/2  Binary"
        Write-Host "  Platform: $($platform.Description)`n  Release:  $Version`n  Target:   $destination`n"
        if (Test-Path -LiteralPath $destination) {
            Write-Host 'The existing binary will be replaced only after verification.'
        }
        if (Confirm-MemixAction 'Install this binary?') {
            $method = Get-MemixDownloadMethod
            $tag = Resolve-MemixRelease $Version $method
            Write-Host "`nSelected release: $tag"
            New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
            $stage = Join-Path $InstallDir ('.memix-install-' + [Guid]::NewGuid().ToString('N'))
            New-Item -ItemType Directory -Path $stage | Out-Null
            $manifest = Join-Path $stage 'SHA256SUMS'
            Save-MemixAsset $tag 'SHA256SUMS' $manifest $method
            $entries = @(Get-Content -LiteralPath $manifest | Where-Object {
                $parts = $_ -split '\s+'
                $parts.Count -eq 2 -and $parts[1] -ceq $platform.Asset
            })
            if ($entries.Count -ne 1) { throw "SHA256SUMS must contain exactly one entry for $($platform.Asset)." }
            $expected = ($entries[0] -split '\s+')[0]
            if ($expected -cnotmatch '^[0-9a-f]{64}$') { throw 'Malformed SHA-256 checksum.' }
            $downloaded = Join-Path $stage $platform.Asset
            Save-MemixAsset $tag $platform.Asset $downloaded $method
            $actual = (Get-FileHash -LiteralPath $downloaded -Algorithm SHA256).Hash
            if ($actual -ne $expected) { throw 'Checksum mismatch; the existing binary was not replaced.' }
            $reported = Test-MemixBinary $downloaded
            Assert-MemixDestination $destination
            if (Test-Path -LiteralPath $destination) {
                [IO.File]::Replace($downloaded, $destination, [NullString]::Value)
            } else {
                [IO.File]::Move($downloaded, $destination)
            }
            $binary = $destination
            Write-Host "`nInstalled $reported`n  $binary"
        } else {
            Write-Host "`nSkipped binary installation."
            $binary = Find-MemixBinary $destination
            if (-not $binary) {
                Write-Host 'No existing memix found; skill setup was skipped.'
                return
            }
            $reported = Test-MemixBinary $binary
            Write-Host "Using $reported`n  $binary"
        }
        $resolved = Get-Command memix -CommandType Application -ErrorAction SilentlyContinue
        if (-not $resolved -or $resolved.Source -ne $binary) {
            Write-Host "`nPATH notice: this binary is not the memix currently resolved by your shell."
            Write-Host 'Add its directory before other Memix installations in your user PATH:'
            Write-Host "  $([IO.Path]::GetDirectoryName($binary))"
            Write-Host 'Then open a new terminal. The installer does not modify PATH.'
        }
        $skillOptions = @()
        foreach ($name in $Agent) { $skillOptions += @('--agent', $name) }
        if ($Project) { $skillOptions += '--project' }
        Write-Host "`n2/2  Agent skill`n"
        try { Invoke-Memix $binary (@('skill', 'setup', '--dry-run') + $skillOptions) }
        catch { Write-Host "Preview reported problems: $($_.Exception.Message)`nSetup may only succeed for some paths." }
        Write-Host "`nSetup will not use --force, install hooks, or modify memory."
        if (Confirm-MemixAction 'Run skill setup with these options?') {
            try { Invoke-Memix $binary (@('skill', 'setup') + $skillOptions) }
            catch { throw "Skill setup was incomplete; the binary remains installed. Inspect memix skill status. $($_.Exception.Message)" }
            Write-Host "`nDone. Read the operating instructions with:`n  memix skill show"
        } else {
            Write-Host "`nSkipped skill setup. You can run memix skill setup later."
        }
    } finally {
        if ($stage -and (Test-Path -LiteralPath $stage)) { Remove-Item -LiteralPath $stage -Recurse -Force }
        [Net.ServicePointManager]::SecurityProtocol = $oldProtocol
    }
}

# Dot-sourcing exposes functions without starting an installation (also used by tests).
if ($MyInvocation.InvocationName -ne '.') {
    try { Invoke-MemixInstaller @PSBoundParameters }
    catch { Write-Error $_ -ErrorAction Continue; exit 1 }
}
