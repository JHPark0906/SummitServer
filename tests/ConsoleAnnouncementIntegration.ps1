param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ServerPath
)

$ErrorActionPreference = 'Stop'
$ServerPath = (Resolve-Path -LiteralPath $ServerPath).Path
$utf8 = [System.Text.UTF8Encoding]::new($false, $true)
$ownedClients = [System.Collections.Generic.List[System.Net.Sockets.TcpClient]]::new()
$server = $null
$stdoutRead = $null
$stderrRead = $null

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Read-PlayersOutput([string]$Output) {
    $lines = $Output -split '\r?\n'
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if (-not $lines[$index].StartsWith('Players:')) { continue }
        $header = [regex]::Match($lines[$index], '^Players: ([0-9]+) \((joined|no joined players)\)$')
        Assert-True $header.Success 'Malformed console player-count header.'
        [int]$total = [int]::Parse($header.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
        Assert-True (($total -eq 0 -and $header.Groups[2].Value -ceq 'no joined players') -or
            ($total -gt 0 -and $header.Groups[2].Value -ceq 'joined')) 'Player-count header does not distinguish an empty list.'
        Assert-True ($total -le $lines.Count - $index - 1) 'Player listing ended before all rows arrived.'
        $players = [System.Collections.Generic.List[object]]::new()
        $rawRows = [System.Collections.Generic.List[string]]::new()
        for ($row = 0; $row -lt $total; $row++) {
            $line = $lines[++$index]
            # Each nickname must remain in one complete JSON line; no log/header may interrupt it.
            Assert-True ($line.TrimStart().StartsWith('{')) 'Console player rows were interleaved or truncated.'
            $players.Add(($line | ConvertFrom-Json -ErrorAction Stop))
            $rawRows.Add($line)
        }
        [pscustomobject]@{
            raw = $rawRows -join "`n"
            body = [pscustomobject]@{ kind = 'players'; total = $total; players = $players.ToArray() }
        }
    }
}

function Assert-Players($Snapshot, [object[]]$Expected) {
    Assert-True ($null -ne $Snapshot -and $Snapshot.kind -ceq 'players') 'Missing console player snapshot.'
    Assert-True ($Snapshot.total -isnot [string] -and $Snapshot.total -eq $Expected.Count) 'Console player total is incorrect.'
    Assert-True ($Snapshot.players -is [System.Array] -and $Snapshot.players.Count -eq $Expected.Count) 'Console player list is not the expected JSON array.'
    [uint64]$previousId = 0
    for ($index = 0; $index -lt $Expected.Count; $index++) {
        $actual = $Snapshot.players[$index]
        $expectedPlayer = $Expected[$index]
        Assert-True ($actual.id -is [string] -and $actual.id -cmatch '^[1-9][0-9]*$') 'Player ID must be an unsigned decimal string.'
        [uint64]$numericId = [uint64]::Parse($actual.id, [Globalization.CultureInfo]::InvariantCulture)
        Assert-True ($numericId -gt $previousId) 'Console players are not in ascending numeric ID order.'
        $previousId = $numericId
        Assert-True ($actual.id -ceq $expectedPlayer.id -and $actual.name -ceq $expectedPlayer.name) 'Console player identity does not match the approved profile.'
        Assert-True ($actual.character -isnot [string] -and $actual.character -eq $expectedPlayer.character -and
            $actual.characterName -ceq $expectedPlayer.characterName) 'Console character does not match the server-approved character.'
    }
}

function Send-Frame($Client, [string]$Type, $Body) {
    [byte[]]$payload = $utf8.GetBytes((@{ type = $Type; body = $Body } | ConvertTo-Json -Compress -Depth 8))
    [byte[]]$prefix = [System.BitConverter]::GetBytes([uint32]$payload.Length)
    $stream = $Client.GetStream()
    $stream.Write($prefix, 0, $prefix.Length)
    $stream.Write($payload, 0, $payload.Length)
}

function Read-Exactly($Stream, [byte[]]$Buffer) {
    $offset = 0
    while ($offset -lt $Buffer.Length) {
        $count = $Stream.Read($Buffer, $offset, $Buffer.Length - $offset)
        if ($count -eq 0) { throw 'Server closed before a complete frame.' }
        $offset += $count
    }
}

function Read-Frame($Client) {
    $stream = $Client.GetStream()
    [byte[]]$prefix = [byte[]]::new(4)
    Read-Exactly $stream $prefix
    $length = [System.BitConverter]::ToUInt32($prefix, 0)
    Assert-True ($length -gt 0 -and $length -le 8192) 'Invalid server frame length.'
    [byte[]]$payload = [byte[]]::new([int]$length)
    Read-Exactly $stream $payload
    return ($utf8.GetString($payload) | ConvertFrom-Json)
}

function Read-Type($Client, [string]$Type) {
    for ($index = 0; $index -lt 20; $index++) {
        $message = Read-Frame $Client
        if ($message.type -eq $Type) { return $message }
    }
    throw "Expected $Type was not received."
}

function Complete-Directory($Client) {
    for ($index = 0; $index -lt 20; $index++) {
        $message = Read-Frame $Client
        if ($message.type -eq 'DirectoryReady') { return }
        if ($message.type -eq 'DirectoryPage') {
            Assert-True ($null -ne $message.body.players -and $message.body.cursor -is [string]) 'Invalid directory page.'
            Send-Frame $Client 'DirectoryAck' @{ cursor = $message.body.cursor }
        }
    }
    throw 'Initial directory did not complete.'
}

function Read-Announcement($Client, [string]$Expected) {
    for ($index = 0; $index -lt 20; $index++) {
        $message = Read-Frame $Client
        if ($message.type -eq 'ServerNotice' -and $message.body.kind -eq 'announcement') {
            Assert-True ($message.body.text -ceq $Expected) 'Announcement text was changed or an invalid command was broadcast.'
            Assert-True ($null -ne $message.body.t) 'Announcement is missing its server timestamp.'
            return
        }
    }
    throw 'Expected server announcement was not received.'
}

function Write-Input([byte[]]$Bytes) {
    $inputStream = $server.StandardInput.BaseStream
    $inputStream.Write($Bytes, 0, $Bytes.Length)
    $inputStream.Flush()
}

function Connect-Client([int]$Port) {
    $client = [System.Net.Sockets.TcpClient]::new()
    $ownedClients.Add($client)
    $client.Connect('127.0.0.1', $Port)
    $client.NoDelay = $true
    $client.ReceiveTimeout = 3000
    $client.SendTimeout = 3000
    return $client
}

try {
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        $probe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
        $probe.Start()
        $port = ([System.Net.IPEndPoint]$probe.LocalEndpoint).Port
        $probe.Stop()
        # Retain the original Process handle; avoid Start-Process's Path/PATH environment rebuilding.
        $server = [System.Diagnostics.Process]::new()
        $server.StartInfo.FileName = $ServerPath
        $server.StartInfo.Arguments = "--address 127.0.0.1 --port $port"
        $server.StartInfo.UseShellExecute = $false
        $server.StartInfo.CreateNoWindow = $true
        $server.StartInfo.RedirectStandardInput = $true
        $server.StartInfo.RedirectStandardOutput = $true
        $server.StartInfo.RedirectStandardError = $true
        $server.StartInfo.StandardOutputEncoding = $utf8
        $server.StartInfo.StandardErrorEncoding = $utf8
        Assert-True ($server.Start()) 'Could not start the server.'
        $stderrRead = $server.StandardError.ReadToEndAsync()
        $stdoutRead = $server.StandardOutput.ReadLineAsync()
        $ready = $false
        $deadline = [DateTime]::UtcNow.AddSeconds(3)
        while ([DateTime]::UtcNow -lt $deadline -and -not $server.HasExited) {
            if ($stdoutRead.IsCompleted) {
                $line = $stdoutRead.GetAwaiter().GetResult()
                if ($null -eq $line) { break }
                if ($line -like "SummitServer listening on 127.0.0.1:$port *") { $ready = $true; break }
                $stdoutRead = $server.StandardOutput.ReadLineAsync()
            }
            Start-Sleep -Milliseconds 10
        }
        if ($ready) {
            break
        }
        if (-not $server.HasExited) { $server.Kill(); $null = $server.WaitForExit(3000) }
        $server.Dispose()
        $server = $null
    }
    Assert-True ($null -ne $server -and $ready) 'Server did not become ready on its ephemeral port.'
    $unjoined = Connect-Client $port
    Write-Input ($utf8.GetBytes("/players`n"))
    $initialPlayers = $null
    $stdoutRead = $server.StandardOutput.ReadLineAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    while ([DateTime]::UtcNow -lt $deadline -and -not $server.HasExited) {
        if ($stdoutRead.IsCompleted) {
            $line = $stdoutRead.GetAwaiter().GetResult()
            if ($null -eq $line) { break }
            if ($line.StartsWith('Players:')) {
                $initialPlayers = Read-PlayersOutput $line
                break
            }
            $stdoutRead = $server.StandardOutput.ReadLineAsync()
        }
        Start-Sleep -Milliseconds 10
    }
    Assert-True ($null -ne $initialPlayers) 'The empty /players command did not return its count header.'
    Assert-Players $initialPlayers.body @()
    # Keep draining stdout while TCP is read; even valid console/log output must not fill the pipe.
    $stdoutRead = $server.StandardOutput.ReadToEndAsync()
    $first = Connect-Client $port
    Send-Frame $first 'Join' @{ schemaVersion = 6; name = 'NoticeFirst'; c = 0 }
    $firstJoin = Read-Type $first 'JoinAccepted'
    Complete-Directory $first
    $second = Connect-Client $port
    Send-Frame $second 'Join' @{ schemaVersion = 6; name = 'NoticeSecond'; c = 1 }
    $secondJoin = Read-Type $second 'JoinAccepted'
    Complete-Directory $second

    # Both commands share the console queue and JobRunner. The announcement is a completion barrier
    # for its preceding snapshot, so later profile/leave mutations cannot race the expected list.
    Write-Input ($utf8.GetBytes("/players`n/announce players-initial-barrier`n"))
    Read-Announcement $first 'players-initial-barrier'
    Read-Announcement $second 'players-initial-barrier'
    $escapedName = 'Name "quoted"\' + [char]0xD55C + [char]::ConvertFromUtf32(0x1F600)
    Send-Frame $first 'SetProfile' @{ name = $escapedName; c = 4 }
    $changed = Read-Type $first 'ProfileChanged'
    Assert-True ($changed.body.id -ceq $firstJoin.body.id -and $changed.body.name -ceq $escapedName -and $changed.body.c -eq 4) 'Profile update was not approved as requested.'
    $null = Read-Type $second 'ProfileChanged'
    Write-Input ($utf8.GetBytes("/players`n/announce players-profile-barrier`n"))
    Read-Announcement $first 'players-profile-barrier'
    Read-Announcement $second 'players-profile-barrier'

    $departing = Connect-Client $port
    $specialName = ([char]0xC0C1).ToString() + [char]0xC5EC + [char]0xC790
    Send-Frame $departing 'Join' @{ schemaVersion = 6; name = $specialName; c = 2 }
    $departingJoin = Read-Type $departing 'JoinAccepted'
    Assert-True ($departingJoin.body.c -eq 5) 'Special nickname did not receive the server-selected tdw character.'
    Complete-Directory $departing
    Write-Input ($utf8.GetBytes("/players`n/announce players-joined-barrier`n"))
    Read-Announcement $first 'players-joined-barrier'
    Read-Announcement $second 'players-joined-barrier'
    Read-Announcement $departing 'players-joined-barrier'
    $departing.Dispose()
    $left = Read-Type $first 'PlayerLeft'
    Assert-True ($left.body.id -ceq $departingJoin.body.id) 'The departing player was not removed before the next snapshot.'
    $null = Read-Type $second 'PlayerLeft'
    Write-Input ($utf8.GetBytes("/players`n/announce players-left-barrier`n"))
    Read-Announcement $first 'players-left-barrier'
    Read-Announcement $second 'players-left-barrier'

    # Build non-ASCII text by code point so Windows PowerShell also reads this script without a BOM.
    $korean = ([char]0xD55C).ToString() + [char]0xAE00 + ' ' + [char]0xACF5 + [char]0xC9C0
    $firstText = 'NOTICE ' + $korean + ' ' + [char]::ConvertFromUtf32(0x1F600)
    [byte[]]$command = $utf8.GetBytes("/announce $firstText`r`n")
    foreach ($value in $command) { Write-Input ([byte[]]@($value)) }
    Read-Announcement $first $firstText
    Read-Announcement $second $firstText
    Assert-True ($unjoined.Available -eq 0) 'An unjoined connection received the announcement.'

    Write-Input ($utf8.GetBytes("/announce`n/announce    `n/announce " + ('x' * 513) + "`n"))
    Write-Input ($utf8.GetBytes('/announce invalid') + [byte[]]@(1, 10))
    Write-Input ($utf8.GetBytes('/announce invalid') + [byte[]]@(0xC0, 0xAF, 10))
    Write-Input ($utf8.GetBytes("/announcex spoof`n/players extra`n" + ('x' * 2200) + "`n/announce after-invalid`n"))
    Read-Announcement $first 'after-invalid'
    Read-Announcement $second 'after-invalid'

    $boundary = 'b' * 512
    Write-Input ($utf8.GetBytes("/announce $boundary`n/announce final-at-eof"))
    # Bytes were written directly; close that pipe without involving a text writer's encoder.
    $server.StandardInput.BaseStream.Close()
    Read-Announcement $first $boundary
    Read-Announcement $second $boundary
    Read-Announcement $first 'final-at-eof'
    Read-Announcement $second 'final-at-eof'
    Assert-True (-not $server.HasExited) 'Redirected stdin EOF stopped the server.'
    Send-Frame $first 'Chat' @{ text = 'server-still-running-after-eof' }
    $chat = Read-Type $first 'ChatMessage'
    Assert-True ($chat.body.text -eq 'server-still-running-after-eof') 'Server did not process requests after stdin EOF.'
    Assert-True ($unjoined.Available -eq 0) 'The pre-Join connection received a console-only listing or announcement.'

    # The EOF/liveness checks above are complete. Stop only this owned process to finish the
    # asynchronous capture and inspect every snapshot, including absence of /players-extra output.
    $server.Kill()
    Assert-True ($server.WaitForExit(3000)) 'The owned server did not exit during test cleanup.'
    Assert-True ($stdoutRead.Wait(3000)) 'Console output did not finish after the owned server exited.'
    $consoleOutput = $stdoutRead.GetAwaiter().GetResult()
    $snapshots = @(Read-PlayersOutput $consoleOutput)
    Assert-True ($snapshots.Count -eq 4) 'Expected exactly four joined-player snapshots; an invalid command may have been accepted.'
    $firstExpected = @{ id = $firstJoin.body.id; name = 'NoticeFirst'; character = 0; characterName = 'A' }
    $secondExpected = @{ id = $secondJoin.body.id; name = 'NoticeSecond'; character = 1; characterName = 'B' }
    Assert-Players $snapshots[0].body @($firstExpected, $secondExpected)
    $firstExpected = @{ id = $firstJoin.body.id; name = $escapedName; character = 4; characterName = 'E' }
    Assert-Players $snapshots[1].body @($firstExpected, $secondExpected)
    Assert-True ($snapshots[1].raw.Contains('\"') -and $snapshots[1].raw.Contains('\\')) 'Quoted/backslash nicknames were not escaped inside one JSON line.'
    $departingExpected = @{ id = $departingJoin.body.id; name = $specialName; character = 5; characterName = 'tdw' }
    Assert-Players $snapshots[2].body @($firstExpected, $secondExpected, $departingExpected)
    Assert-Players $snapshots[3].body @($firstExpected, $secondExpected)
    Write-Output 'SummitServer console announcement and player listing integration passed.'
}
finally {
    foreach ($client in $ownedClients) { $client.Dispose() }
    if ($null -ne $server) {
        if (-not $server.HasExited) {
            $server.Kill()
            $null = $server.WaitForExit(3000)
        }
        if ($null -ne $stdoutRead -and $stdoutRead.Wait(3000)) { $null = $stdoutRead.GetAwaiter().GetResult() }
        if ($null -ne $stderrRead -and $stderrRead.Wait(3000)) {
            $diagnostics = $stderrRead.GetAwaiter().GetResult()
            if ($diagnostics) { Write-Verbose $diagnostics }
        }
        $server.Dispose()
    }
}
