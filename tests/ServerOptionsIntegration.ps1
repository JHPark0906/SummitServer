param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ServerPath
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ProtocolTestReader.ps1')
$ServerPath = (Resolve-Path -LiteralPath $ServerPath).Path
$testDirectory = Join-Path ([System.IO.Path]::GetTempPath()) `
    ('SummitServerOptions-' + [System.Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $testDirectory
$ownedProcesses = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
$serverLaunches = @{}
$ownedClients = [System.Collections.Generic.List[System.Net.Sockets.TcpClient]]::new()
$logPaths = [System.Collections.Generic.List[string]]::new()
$script:launchIndex = 0
$completed = $false

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Start-OwnedServer {
    param([string[]]$Arguments)
    $script:launchIndex++
    $stdout = Join-Path $testDirectory "$script:launchIndex.stdout.log"
    $stderr = Join-Path $testDirectory "$script:launchIndex.stderr.log"
    $logPaths.Add($stdout)
    $logPaths.Add($stderr)
    # Avoid Start-Process rebuilding an inherited environment containing Path/PATH.
    # Process.Start also retains the original process handle for reliable cleanup.
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo.FileName = $ServerPath
    $process.StartInfo.Arguments = $Arguments -join ' '
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    Assert-True ($process.Start()) 'Could not start the loopback server.'
    $ownedProcesses.Add($process)
    [System.IO.File]::WriteAllText($stdout, '')
    $launch = [pscustomobject]@{
        Process = $process; Stdout = $stdout; Stderr = $stderr
        StdoutRead = $process.StandardOutput.ReadLineAsync()
        StderrRead = $process.StandardError.ReadToEndAsync()
    }
    $serverLaunches[$process.Id] = $launch
    return $launch
}

function Receive-ServerOutput {
    param($Launch)
    # No callbacks or PowerShell jobs: consume only already-completed line reads.
    while ($null -ne $Launch.StdoutRead -and $Launch.StdoutRead.IsCompleted) {
        $line = $Launch.StdoutRead.GetAwaiter().GetResult()
        if ($null -eq $line) {
            $Launch.StdoutRead = $null
            break
        }
        [System.IO.File]::AppendAllText($Launch.Stdout, $line + [Environment]::NewLine)
        $Launch.StdoutRead = $Launch.Process.StandardOutput.ReadLineAsync()
    }
}

function Stop-OwnedServer {
    param([System.Diagnostics.Process]$Process)
    # Retain the actual started process object; never select a process by port or name.
    if (-not $Process.HasExited) {
        $Process.Kill()
        Assert-True ($Process.WaitForExit(3000)) "Owned server PID $($Process.Id) did not exit."
    }
    if ($serverLaunches.ContainsKey($Process.Id)) {
        $launch = $serverLaunches[$Process.Id]
        while ($null -ne $launch.StdoutRead) {
            Assert-True ($launch.StdoutRead.Wait(3000)) 'Server stdout did not finish after process exit.'
            Receive-ServerOutput -Launch $launch
        }
        Assert-True ($launch.StderrRead.Wait(3000)) 'Server stderr did not finish after process exit.'
        [System.IO.File]::WriteAllText($launch.Stderr, $launch.StderrRead.Result)
        $serverLaunches.Remove($Process.Id)
    }
}

function Assert-CommandExit {
    param([string[]]$Arguments, [int]$ExpectedExitCode)
    # Windows PowerShell's Start-Process -PassThru may lose the exit status of
    # an already-finished command. Process.Start retains its original handle.
    $script:launchIndex++
    $stdout = Join-Path $testDirectory "$script:launchIndex.stdout.log"
    $stderr = Join-Path $testDirectory "$script:launchIndex.stderr.log"
    $logPaths.Add($stdout)
    $logPaths.Add($stderr)
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo.FileName = $ServerPath
    # These test arguments contain only fixed switches and numeric/ASCII tokens.
    $process.StartInfo.Arguments = $Arguments -join ' '
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    Assert-True ($process.Start()) 'Could not start the CLI command.'
    $ownedProcesses.Add($process)
    $launch = [pscustomobject]@{ Process = $process; Stdout = $stdout; Stderr = $stderr }
    # Drain both pipes concurrently, including before WaitForExit, so output
    # cannot fill one redirected pipe while the other is being read.
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    try {
        Assert-True ($launch.Process.WaitForExit(3000)) `
            "Command did not exit: $($Arguments -join ' ')"
        Assert-True ($stdoutTask.Wait(3000) -and $stderrTask.Wait(3000)) `
            'CLI command output did not finish after process exit.'
        Assert-True ($launch.Process.ExitCode -eq $ExpectedExitCode) `
            "Expected exit $ExpectedExitCode, got $($launch.Process.ExitCode): $($Arguments -join ' ')"
        if ($ExpectedExitCode -eq 0) {
            $help = $stdoutTask.Result
            Assert-True ($help -match '--max-sessions' -and $help -match '--port') `
                'Help did not describe the session limit and listening port.'
            Assert-True ($help -match '16 joined players / 64 connections') `
                'Help did not preserve the production default capacities.'
        }
    }
    finally {
        Stop-OwnedServer -Process $launch.Process
        if ($stdoutTask.Wait(3000)) {
            [System.IO.File]::WriteAllText($stdout, $stdoutTask.Result)
        }
        if ($stderrTask.Wait(3000)) {
            [System.IO.File]::WriteAllText($stderr, $stderrTask.Result)
        }
    }
}

function Start-ReadyServer {
    param([int]$MaximumSessions, [string[]]$BudgetArguments = @())
    # A released ephemeral-port probe is inherently racy. Require our process's
    # flushed startup line before connecting, and retry with a fresh port on failure.
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        $probe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
        try {
            $probe.Start()
            $port = ([System.Net.IPEndPoint]$probe.LocalEndpoint).Port
        }
        finally {
            $probe.Stop()
        }

        $launch = Start-OwnedServer -Arguments (@('--address', '127.0.0.1',
            '--port', "$port", '--max-sessions', "$MaximumSessions") + $BudgetArguments)
        $deadline = [System.Diagnostics.Stopwatch]::StartNew()
        while ($deadline.ElapsedMilliseconds -lt 3000 -and -not $launch.Process.HasExited) {
            Receive-ServerOutput -Launch $launch
            $output = Get-Content -LiteralPath $launch.Stdout -Raw
            $ready = "SummitServer listening on 127.0.0.1:$port (players $MaximumSessions, connections $MaximumSessions)"
            if ($null -ne $output -and $output.Contains($ready)) {
                return [pscustomobject]@{ Process = $launch.Process; Port = $port; Launch = $launch }
            }
            Start-Sleep -Milliseconds 25
        }
        Stop-OwnedServer -Process $launch.Process
    }
    throw "SummitServer could not start with --max-sessions $MaximumSessions. Logs: $testDirectory"
}

function Connect-TestClient {
    param($Server)
    Assert-True (-not $Server.Process.HasExited) 'Owned server exited before client connection.'
    $client = [System.Net.Sockets.TcpClient]::new()
    $ownedClients.Add($client)
    $connected = $client.ConnectAsync('127.0.0.1', $Server.Port)
    Assert-True ($connected.Wait(3000)) 'Timed out connecting to the owned loopback server.'
    $client.NoDelay = $true
    $client.ReceiveTimeout = 3000
    $client.SendTimeout = 3000
    return $client
}

function Send-Frame {
    param([System.Net.Sockets.TcpClient]$Client, [string]$Json)
    [byte[]]$body = [System.Text.Encoding]::UTF8.GetBytes($Json)
    [byte[]]$header = [System.BitConverter]::GetBytes([uint32]$body.Length)
    $stream = $Client.GetStream()
    $stream.Write($header, 0, $header.Length)
    $stream.Write($body, 0, $body.Length)
    $stream.Flush()
}

function Read-Exactly {
    param([System.Net.Sockets.NetworkStream]$Stream, [byte[]]$Buffer)
    $offset = 0
    while ($offset -lt $Buffer.Length) {
        $read = $Stream.Read($Buffer, $offset, $Buffer.Length - $offset)
        Assert-True ($read -gt 0) 'Peer closed before a complete frame arrived.'
        $offset += $read
    }
}

function Read-Frame {
    param([System.Net.Sockets.TcpClient]$Client)
    return (Read-SummitGameFrame -Client $Client)
}

function Join-TestClient {
    param([System.Net.Sockets.TcpClient]$Client, [string]$Name, [int]$Capacity)
    $request = @{ type = 'Join'; body = @{ schemaVersion = 6; name = $Name; c = 0 } }
    Send-Frame -Client $Client -Json ($request | ConvertTo-Json -Depth 3 -Compress)
    $accepted = Read-Frame -Client $Client
    Assert-True ($accepted.type -eq 'JoinAccepted' -and $accepted.body.schemaVersion -eq 6) `
        "$Name did not receive a v6 JoinAccepted."
    Assert-True ($accepted.body.name -eq $Name -and $accepted.body.capacity -eq $Capacity -and
        -not [string]::IsNullOrEmpty([string]$accepted.body.id)) `
        "$Name received an incorrect approved profile or capacity."
    return $accepted
}

function Assert-CapacityClosed {
    param([System.Net.Sockets.TcpClient]$Client)
    [byte[]]$buffer = [byte[]]::new(1)
    try {
        Assert-True ($Client.GetStream().Read($buffer, 0, 1) -eq 0) `
            'Third connection received application data instead of being rejected by the transport limit.'
    }
    catch [System.IO.IOException] {
        # Windows may report a reset instead of FIN. A read timeout is not a rejection.
        $socketError = $_.Exception.InnerException
        if ($socketError -isnot [System.Net.Sockets.SocketException] -or
            $socketError.SocketErrorCode -ne [System.Net.Sockets.SocketError]::ConnectionReset) {
            throw
        }
    }
}

try {
    foreach ($invalid in @('0', '-1', '65537', 'abc', '4294967296')) {
        Assert-CommandExit -Arguments @('--max-sessions', $invalid) -ExpectedExitCode 2
    }
    Assert-CommandExit -Arguments @('--max-sessions') -ExpectedExitCode 2
    foreach ($invalid in @('0', '-1', '65536', 'abc', '4294967296')) {
        Assert-CommandExit -Arguments @('--port', $invalid) -ExpectedExitCode 2
    }
    Assert-CommandExit -Arguments @('--port') -ExpectedExitCode 2
    foreach ($budget in @('--state-bytes-per-tick', '--total-state-bytes-per-tick',
            '--control-bytes-per-tick', '--total-control-bytes-per-tick')) {
        foreach ($invalid in @('0', '1023', '-1', '16777217', 'abc', '4294967296')) {
            Assert-CommandExit -Arguments @($budget, $invalid) -ExpectedExitCode 2
        }
        Assert-CommandExit -Arguments @($budget) -ExpectedExitCode 2
        Assert-CommandExit -Arguments @($budget, '1024', '--help') -ExpectedExitCode 0
        Assert-CommandExit -Arguments @($budget, '16777216', '--help') -ExpectedExitCode 0
    }
    Assert-CommandExit -Arguments @('--help') -ExpectedExitCode 0
    # Help parses the endpoints without reserving a privileged or developer port.
    Assert-CommandExit -Arguments @('--max-sessions', '1', '--port', '1', '--help') -ExpectedExitCode 0
    Assert-CommandExit -Arguments @('--max-sessions', '65536', '--port', '65535', '--help') -ExpectedExitCode 0

    $limited = Start-ReadyServer -MaximumSessions 2 -BudgetArguments @(
        '--state-bytes-per-tick', '1024', '--total-state-bytes-per-tick', '2048',
        '--control-bytes-per-tick', '4096', '--total-control-bytes-per-tick', '8192')
    $settingsDeadline = [System.Diagnostics.Stopwatch]::StartNew()
    do {
        Receive-ServerOutput -Launch $limited.Launch
        $settingsOutput = Get-Content -LiteralPath $limited.Launch.Stdout -Raw
        if ($settingsOutput.Contains('state/client=1024, state/total=2048, directory-AOI/client=4096, directory-AOI/total=8192')) { break }
        Start-Sleep -Milliseconds 10
    } while ($settingsDeadline.ElapsedMilliseconds -lt 3000)
    Assert-True ($settingsOutput.Contains('state/client=1024, state/total=2048, directory-AOI/client=4096, directory-AOI/total=8192')) `
        'Configured tick budgets were not applied to the running server.'
    $first = Connect-TestClient -Server $limited
    $firstAccepted = Join-TestClient -Client $first -Name 'LimitFirst' -Capacity 2
    $second = Connect-TestClient -Server $limited
    $secondAccepted = Join-TestClient -Client $second -Name 'LimitSecond' -Capacity 2
    Assert-True ($firstAccepted.body.id -ne $secondAccepted.body.id) 'Accepted clients share an id.'
    $existing = Read-Frame -Client $second
    $joined = Read-Frame -Client $first
    Assert-True ($existing.type -eq 'PlayerJoined' -and $existing.body.id -eq $firstAccepted.body.id -and
        $joined.type -eq 'PlayerJoined' -and $joined.body.id -eq $secondAccepted.body.id) `
        'The two admitted sessions did not exchange membership events.'

    $third = Connect-TestClient -Server $limited
    Assert-CapacityClosed -Client $third
    $third.Dispose()
    Send-Frame -Client $first -Json '{"type":"Chat","body":{"text":"limit connections remain alive"}}'
    foreach ($client in @($first, $second)) {
        $chat = Read-Frame -Client $client
        Assert-True ($chat.type -eq 'ChatMessage' -and $chat.body.id -eq $firstAccepted.body.id -and
            $chat.body.name -eq 'LimitFirst' -and $chat.body.text -eq 'limit connections remain alive') `
            'Rejecting the third connection disrupted an existing joined session.'
    }
    $first.Dispose()
    $second.Dispose()
    Stop-OwnedServer -Process $limited.Process

    $maximum = Start-ReadyServer -MaximumSessions 65536
    $maximumClient = Connect-TestClient -Server $maximum
    $maximumAccepted = Join-TestClient -Client $maximumClient -Name 'MaximumCapacity' -Capacity 65536
    Send-Frame -Client $maximumClient -Json '{"type":"Chat","body":{"text":"maximum capacity is configured"}}'
    $maximumChat = Read-Frame -Client $maximumClient
    Assert-True ($maximumChat.type -eq 'ChatMessage' -and
        $maximumChat.body.id -eq $maximumAccepted.body.id -and
        $maximumChat.body.text -eq 'maximum capacity is configured') `
        'The maximum-capacity server did not complete an authenticated message round trip.'
    Assert-True (-not $maximum.Process.HasExited) 'The maximum-capacity server exited unexpectedly.'
    $completed = $true
    Write-Output 'SummitServer CLI and session-capacity integration tests passed.'
}
finally {
    foreach ($client in $ownedClients) {
        $client.Dispose()
    }
    foreach ($process in $ownedProcesses) {
        Stop-OwnedServer -Process $process
        $process.Dispose()
    }
    if ($completed) {
        foreach ($logPath in $logPaths) {
            if (Test-Path -LiteralPath $logPath -PathType Leaf) {
                Remove-Item -LiteralPath $logPath
            }
        }
        Remove-Item -LiteralPath $testDirectory
    }
    else {
        Write-Warning "SummitServer CLI test logs retained at $testDirectory"
    }
}
