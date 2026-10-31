# Offline tests with real filesystem/checksum operations and mocked terminal,
# release transport, and native process calls. No Pester/module dependencies.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../install.ps1')

function Assert-True($Value, [string] $Message) {
    if (-not $Value) { throw "Assertion failed: $Message" }
}
function Assert-Fails([scriptblock] $Action, [string] $Message) {
    $caught = $null
    try { & $Action } catch { $caught = $_ }
    Assert-True ($null -ne $caught) "expected failure: $Message"
    Assert-True ($caught.Exception.Message -like "*$Message*") "unexpected error: $caught"
}

# Check native platform detection too, before replacing it for offline fixtures.
if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
    Assert-True ((Get-MemixPlatform).Asset -eq 'memix-windows-x86_64.exe') 'native Windows detection'
    $oldArchitecture = $env:PROCESSOR_ARCHITECTURE
    $oldWow64 = $env:PROCESSOR_ARCHITEW6432
    try {
        $env:PROCESSOR_ARCHITEW6432 = ''
        $env:PROCESSOR_ARCHITECTURE = 'ARM64'
        Assert-Fails { Get-MemixPlatform } 'No release binary'
    } finally {
        $env:PROCESSOR_ARCHITECTURE = $oldArchitecture
        $env:PROCESSOR_ARCHITEW6432 = $oldWow64
    }
} else {
    Assert-Fails { Get-MemixPlatform } 'Use install.sh'
}
if ([Console]::IsInputRedirected) {
    Assert-Fails { Assert-MemixInteractive } 'interactive terminal'
}

function Get-MemixPlatform { return @{ Asset = 'memix-windows-x86_64.exe'; Description = 'Windows / x86_64' } }
function Assert-MemixInteractive { if ($script:NoTerminal) { throw 'An interactive terminal is required.' } }
function Get-MemixDownloadMethod { return 'web' }
function Read-MemixResponse([string] $Prompt) {
    $script:Prompts.Add($Prompt)
    if ($script:Answers.Count -eq 0) { throw "Unexpected confirmation: $Prompt" }
    return $script:Answers.Dequeue()
}
function Find-MemixBinary([string] $Destination) {
    if (Test-Path -LiteralPath $Destination -PathType Leaf) { return $Destination }
    return $null
}
function Invoke-RestMethod($Uri, $Headers) {
    Assert-True ($Uri -eq 'https://api.github.com/repos/MrZLeo/memix/releases/latest') 'latest release endpoint'
    $script:MetadataRequests++
    return @{ tag_name = 'v0.7.0' }
}
function Invoke-WebRequest($Uri, $OutFile, $TimeoutSec, [switch] $UseBasicParsing) {
    $name = $Uri.Split('/')[-1]
    $script:Downloads.Add($Uri)
    Assert-True ($Uri -eq "https://github.com/MrZLeo/memix/releases/download/v0.7.0/$name") 'pinned release download'
    if ($script:FailDownload -eq $name) { throw 'Download failed.' }
    if ($name -eq 'SHA256SUMS') {
        $digest = (Get-FileHash -LiteralPath $script:Fixture -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($script:BadChecksum) { $digest = '0' * 64 }
        if ($script:MalformedChecksum) { $digest = 'not-a-checksum' }
        $manifest = "$digest  memix-windows-x86_64.exe`n"
        if ($script:DuplicateChecksum) { $manifest += $manifest }
        [IO.File]::WriteAllText($OutFile, $manifest)
    } else {
        [IO.File]::WriteAllBytes($OutFile, [IO.File]::ReadAllBytes($script:Fixture))
    }
}
function Invoke-Memix([string] $Binary, [string[]] $Arguments) {
    $script:Calls.Add(($Arguments -join ' '))
    Assert-True (Test-Path -LiteralPath $Binary -PathType Leaf) 'binary exists before execution'
    if ($Arguments[0] -eq '--version') {
        if ($script:BadBinary) { throw 'Binary failed.' }
        return 'memix 0.7.0'
    }
    if (($Arguments -join ' ') -eq 'skill --help') {
        if ($script:OldBinary) { throw 'Missing skill command.' }
        return
    }
    Assert-True ($Arguments[0] -eq 'skill' -and $Arguments[1] -eq 'setup') 'only skill setup is invoked'
    Assert-True ($Arguments -notcontains '--force') 'no forced replacement'
    if ($Arguments -contains '--dry-run') {
        if ($script:PreviewFailure) { throw 'Preview failed.' }
        return
    }
    $script:SetupRan = $true
    if ($script:SetupFailure) { throw 'Setup failed.' }
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('memix-ps-installer-test-' + [Guid]::NewGuid().ToString('N'))
$originalPath = $env:PATH
$originalProtocol = [Net.ServicePointManager]::SecurityProtocol
New-Item -ItemType Directory -Path $root | Out-Null
$script:Fixture = Join-Path $root 'fixture'
[IO.File]::WriteAllText($script:Fixture, 'downloaded binary fixture')
$passed = 0

function Reset-Fixture([object[]] $Responses) {
    $script:InstallDir = Join-Path $root ([Guid]::NewGuid().ToString('N') + ' bin with spaces')
    $script:Destination = Join-Path $script:InstallDir 'memix.exe'
    $script:Answers = [Collections.Generic.Queue[object]]::new()
    foreach ($response in $Responses) { $script:Answers.Enqueue($response) }
    $script:Prompts = [Collections.Generic.List[string]]::new()
    $script:Downloads = [Collections.Generic.List[string]]::new()
    $script:Calls = [Collections.Generic.List[string]]::new()
    $script:MetadataRequests = 0
    $script:NoTerminal = $false
    $script:FailDownload = ''
    $script:BadChecksum = $false
    $script:MalformedChecksum = $false
    $script:DuplicateChecksum = $false
    $script:BadBinary = $false
    $script:OldBinary = $false
    $script:PreviewFailure = $false
    $script:SetupFailure = $false
    $script:SetupRan = $false
}
function Install-OldFixture {
    New-Item -ItemType Directory -Path $script:InstallDir | Out-Null
    [IO.File]::WriteAllText($script:Destination, 'previous installation')
}
function Assert-CleanStage {
    if (Test-Path -LiteralPath $script:InstallDir) {
        Assert-True (@(Get-ChildItem -LiteralPath $script:InstallDir -Force -Filter '.memix-install-*').Count -eq 0) 'staging directory cleaned'
    }
    Assert-True ($env:PATH -eq $originalPath) 'PATH unchanged'
    Assert-True ([Net.ServicePointManager]::SecurityProtocol -eq $originalProtocol) 'TLS setting restored'
}
function Test-Case([string] $Name, [scriptblock] $Body) {
    & $Body
    Assert-CleanStage
    $script:passed++
    Write-Host "PASS: $Name"
}

try {
    Test-Case 'default yes installs and sets up with two prompts' {
        Reset-Fixture @('', '')
        Invoke-MemixInstaller -InstallDir $script:InstallDir
        Assert-True ($script:Prompts.Count -eq 2) 'two confirmations'
        Assert-True ((Get-Content -LiteralPath $script:Destination -Raw) -eq 'downloaded binary fixture') 'binary installed'
        Assert-True $script:SetupRan 'setup applied'
        Assert-True ($script:Downloads.Count -eq 2) 'checksum and binary downloaded'
    }
    Test-Case 'decline without existing binary' {
        Reset-Fixture @('n')
        Invoke-MemixInstaller -InstallDir $script:InstallDir
        Assert-True (-not (Test-Path -LiteralPath $script:InstallDir)) 'no directory created'
        Assert-True ($script:Downloads.Count -eq 0 -and -not $script:SetupRan) 'no downloads or setup'
    }
    Test-Case 'decline update and independently accept setup' {
        Reset-Fixture @('n', 'y')
        Install-OldFixture
        Invoke-MemixInstaller -InstallDir $script:InstallDir
        Assert-True ((Get-Content -LiteralPath $script:Destination -Raw) -eq 'previous installation') 'old binary preserved'
        Assert-True $script:SetupRan 'existing binary used for setup'
        Assert-True ($script:Downloads.Count -eq 0 -and $script:MetadataRequests -eq 0) 'no network needed when installation is declined'
    }
    Test-Case 'decline both steps' {
        Reset-Fixture @('n', 'n')
        Install-OldFixture
        Invoke-MemixInstaller -InstallDir $script:InstallDir
        Assert-True (-not $script:SetupRan) 'setup skipped'
    }
    Test-Case 'replace existing binary but decline setup' {
        Reset-Fixture @('y', 'n')
        Install-OldFixture
        Invoke-MemixInstaller -InstallDir $script:InstallDir
        Assert-True ((Get-Content -LiteralPath $script:Destination -Raw) -eq 'downloaded binary fixture') 'old binary replaced'
        Assert-True (-not $script:SetupRan) 'setup skipped'
    }
    Test-Case 'EOF before installation is not consent' {
        Reset-Fixture @($null)
        Assert-Fails { Invoke-MemixInstaller -InstallDir $script:InstallDir } 'EOF'
        Assert-True (-not (Test-Path -LiteralPath $script:InstallDir)) 'no directory created'
    }
    Test-Case 'EOF at setup leaves approved binary but does not set up' {
        Reset-Fixture @('y', $null)
        Assert-Fails { Invoke-MemixInstaller -InstallDir $script:InstallDir } 'EOF'
        Assert-True (Test-Path -LiteralPath $script:Destination) 'approved binary remains'
        Assert-True (-not $script:SetupRan) 'setup not approved'
    }
    Test-Case 'invalid answers are retried' {
        Reset-Fixture @('maybe', 'n')
        Invoke-MemixInstaller -InstallDir $script:InstallDir
        Assert-True ($script:Prompts.Count -eq 2) 'prompt repeated'
    }
    Test-Case 'noninteractive installation is refused' {
        Reset-Fixture @()
        $script:NoTerminal = $true
        Assert-Fails { Invoke-MemixInstaller -InstallDir $script:InstallDir } 'interactive terminal'
        Assert-True ($script:MetadataRequests -eq 0 -and $script:Downloads.Count -eq 0) 'no network access'
    }
    foreach ($failure in @('BadChecksum', 'MalformedChecksum', 'DuplicateChecksum', 'BadBinary', 'OldBinary', 'ManifestDownload', 'BinaryDownload')) {
        Test-Case "failure preserves existing binary: $failure" {
            Reset-Fixture @('y')
            Install-OldFixture
            switch ($failure) {
                'ManifestDownload' { $script:FailDownload = 'SHA256SUMS' }
                'BinaryDownload' { $script:FailDownload = 'memix-windows-x86_64.exe' }
                default { Set-Variable -Scope Script -Name $failure -Value $true }
            }
            Assert-Fails { Invoke-MemixInstaller -InstallDir $script:InstallDir } ''
            Assert-True ((Get-Content -LiteralPath $script:Destination -Raw) -eq 'previous installation') 'old binary preserved'
            Assert-True (-not $script:SetupRan) 'no setup after failed binary verification'
        }
    }
    Test-Case 'directory destination is protected' {
        Reset-Fixture @()
        New-Item -ItemType Directory -Path $script:Destination -Force | Out-Null
        Assert-Fails { Invoke-MemixInstaller -InstallDir $script:InstallDir } 'directory or symlink'
    }
    Test-Case 'pinned release skips latest lookup and passes setup options unchanged' {
        Reset-Fixture @('y', 'y')
        Invoke-MemixInstaller -InstallDir $script:InstallDir -Version v0.7.0 -Agent pi,codex -Project
        Assert-True ($script:MetadataRequests -eq 0) 'no latest lookup'
        Assert-True ($script:Calls -contains 'skill setup --dry-run --agent pi --agent codex --project') 'preview flags'
        Assert-True ($script:Calls -contains 'skill setup --agent pi --agent codex --project') 'setup flags'
    }
    Test-Case 'partial setup failure is not reported as success' {
        Reset-Fixture @('y', 'y')
        $script:PreviewFailure = $true
        $script:SetupFailure = $true
        Assert-Fails { Invoke-MemixInstaller -InstallDir $script:InstallDir } 'incomplete'
        Assert-True (Test-Path -LiteralPath $script:Destination) 'binary remains installed'
    }
    Test-Case 'help works without terminal or platform detection' {
        Reset-Fixture @()
        $script:NoTerminal = $true
        Invoke-MemixInstaller -Help
        Assert-True ($script:MetadataRequests -eq 0 -and $script:Prompts.Count -eq 0) 'help is read-only'
    }
    Test-Case 'invalid tag is rejected before confirmation' {
        Reset-Fixture @()
        Assert-Fails { Invoke-MemixInstaller -InstallDir $script:InstallDir -Version '../wrong' } 'Invalid release tag'
        Assert-True ($script:Prompts.Count -eq 0) 'no prompt'
    }
    Write-Host "`n$passed installer tests passed."
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
