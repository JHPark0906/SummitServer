param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ServerPath
)

$ErrorActionPreference = 'Stop'
$ServerPath = (Resolve-Path -LiteralPath $ServerPath).Path
$utf8 = [System.Text.UTF8Encoding]::new($false, $true)
$peers = [System.Collections.Generic.List[object]]::new()
$udpSockets = [System.Collections.Generic.List[System.Net.Sockets.UdpClient]]::new()
$server = $null
$stdoutRead = $null
$stderrRead = $null

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Send-Tcp($Peer, [string]$Type, $Body) {
    [byte[]]$bytes = $utf8.GetBytes((@{type=$Type; body=$Body} | ConvertTo-Json -Compress -Depth 10))
    [byte[]]$prefix = [BitConverter]::GetBytes([uint32]$bytes.Length)
    $stream = $Peer.tcp.GetStream()
    $stream.Write($prefix, 0, 4)
    $stream.Write($bytes, 0, $bytes.Length)
}

function Read-Exactly($Stream, [byte[]]$Bytes) {
    $used = 0
    while ($used -lt $Bytes.Length) {
        $count = $Stream.Read($Bytes, $used, $Bytes.Length - $used)
        if ($count -eq 0) { throw 'TCP closed in the middle of a test frame.' }
        $used += $count
    }
}

function New-Udp([int]$Port) {
    $socket = [System.Net.Sockets.UdpClient]::new([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, 0))
    $socket.Connect('127.0.0.1', $Port)
    $udpSockets.Add($socket)
    return $socket
}

function Encode-Datagram([byte[]]$Token, [uint64]$Sequence, [byte[]]$Payload) {
    [byte[]]$bytes = [byte[]]::new(28 + $Payload.Length)
    [Array]::Copy([Text.Encoding]::ASCII.GetBytes('SMU1'), 0, $bytes, 0, 4)
    [Array]::Copy($Token, 0, $bytes, 4, 16)
    [byte[]]$number = [BitConverter]::GetBytes($Sequence)
    if ([BitConverter]::IsLittleEndian) { [Array]::Reverse($number) }
    [Array]::Copy($number, 0, $bytes, 20, 8)
    [Array]::Copy($Payload, 0, $bytes, 28, $Payload.Length)
    return ,$bytes
}

function Send-Udp($Peer, [string]$Type, $Body, [uint64]$Sequence = 0, $Socket = $null) {
    if ($Sequence -eq 0) { $Peer.sequence++; $Sequence = $Peer.sequence }
    if ($null -eq $Socket) { $Socket = $Peer.udp }
    [byte[]]$payload = $utf8.GetBytes((@{type=$Type;body=$Body} | ConvertTo-Json -Compress -Depth 10))
    [byte[]]$bytes = Encode-Datagram $Peer.token $Sequence $payload
    Assert-True ($Socket.Send($bytes, $bytes.Length) -eq $bytes.Length) 'UDP test send was partial.'
}

function State([uint64]$Q, [double]$X, [double]$Velocity = 0) {
    # Decimal q exercises exact integer input while the server still owns profile c.
    return @{q=$Q.ToString([Globalization.CultureInfo]::InvariantCulture);x=$X;y=4;vx=$Velocity;vy=0;f=1;s='idle';c=5}
}

function Capture-State($Peer, $State, [string]$Transport) {
    Assert-True ($State.id -is [string] -and $State.r -is [string] -and $State.r -cmatch '^(0|[1-9][0-9]*)$') 'Movement ID/revision lost its exact string representation.'
    $null = [uint64]::Parse($State.r, [Globalization.CultureInfo]::InvariantCulture)
    Assert-True ($State.id -cne $Peer.id) 'Server echoed self movement.'
    $Peer.states.Add([pscustomobject]@{state=$State;transport=$Transport;time=[DateTime]::UtcNow})
}

function Pump {
    foreach ($peer in $peers) {
        if ($peer.closed) { continue }
        $stream = $peer.tcp.GetStream()
        for ($batch = 0; $batch -lt 32 -and $stream.DataAvailable; $batch++) {
            [byte[]]$prefix = [byte[]]::new(4)
            Read-Exactly $stream $prefix
            $length = [BitConverter]::ToUInt32($prefix, 0)
            Assert-True ($length -gt 0 -and $length -le 8192) 'TCP frame exceeded its body limit.'
            [byte[]]$bytes = [byte[]]::new([int]$length)
            Read-Exactly $stream $bytes
            $message = $utf8.GetString($bytes) | ConvertFrom-Json
            $peer.events.Add($message)
            switch ($message.type) {
                'JoinAccepted' {
                    Assert-True ($message.body.schemaVersion -eq 6 -and @($message.body.players).Count -eq 0) 'Join schema or paged-directory contract changed.'
                    $peer.id = [string]$message.body.id
                    $peer.accepted = $message.body
                }
                'DirectoryPage' { Send-Tcp $peer 'DirectoryAck' @{cursor=$message.body.cursor} }
                'DirectoryReady' { $peer.directoryReady = $true }
                'VisibilityEnter' { foreach ($state in $message.body.players) { Capture-State $peer $state 'enter' } }
                'StateBatch' {
                    Assert-True (-not $peer.negotiated) 'Negotiated movement fell back to TCP.'
                    foreach ($state in $message.body.states) { Capture-State $peer $state 'tcp' }
                }
                'JoinRejected' { throw 'A valid UDP integration profile was rejected.' }
            }
        }
        if ($null -eq $peer.udp) { continue }
        for ($batch = 0; $batch -lt 32 -and $peer.udp.Available -gt 0; $batch++) {
            $endpoint = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
            [byte[]]$bytes = $peer.udp.Receive([ref]$endpoint)
            Assert-True ($bytes.Length -gt 28 -and $bytes.Length -le 1200) 'UDP frame exceeded its datagram budget.'
            Assert-True ([Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ceq 'SMU1') 'UDP magic changed.'
            for ($index = 0; $index -lt 16; $index++) { Assert-True ($bytes[4+$index] -eq $peer.token[$index]) 'Server used another session token.' }
            [byte[]]$number = $bytes[20..27]
            if ([BitConverter]::IsLittleEndian) { [Array]::Reverse($number) }
            Assert-True ([BitConverter]::ToUInt64($number, 0) -gt 0) 'Server sent sequence zero.'
            $message = $utf8.GetString($bytes, 28, $bytes.Length - 28) | ConvertFrom-Json
            if ($message.type -eq 'UdpReady') { $peer.ready = $true }
            elseif ($message.type -eq 'StateBatch') {
                foreach ($state in $message.body.states) { Capture-State $peer $state 'udp' }
            }
            else { throw 'A reliable control message was sent over UDP.' }
        }
    }
}

function Wait-Until([scriptblock]$Predicate, [string]$Message, [int]$Milliseconds = 4000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    do {
        Pump
        if (& $Predicate) { return }
        Start-Sleep -Milliseconds 5
    } while ([DateTime]::UtcNow -lt $deadline)
    throw $Message
}

function Settle([int]$Milliseconds = 650) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    do { Pump; Start-Sleep -Milliseconds 5 } while ([DateTime]::UtcNow -lt $deadline)
}

function Has-State($Peer, [string]$Id, [uint64]$Q, [string]$Transport = '') {
    foreach ($item in $Peer.states) {
        if ($item.state.id -ceq $Id -and [uint64]$item.state.q -eq $Q -and
            ($Transport -eq '' -or $item.transport -ceq $Transport)) { return $true }
    }
    return $false
}

function Join-Peer([string]$Name, [int]$Character, [bool]$Udp) {
    $tcp = [System.Net.Sockets.TcpClient]::new()
    $tcp.NoDelay = $true
    $tcp.ReceiveTimeout = 2000
    $tcp.SendTimeout = 2000
    $peer = [pscustomobject]@{tcp=$tcp;udp=$null;token=$null;sequence=[uint64]0;ready=$false;negotiated=$Udp;
        directoryReady=$false;id='';accepted=$null;closed=$false;
        events=[System.Collections.Generic.List[object]]::new();states=[System.Collections.Generic.List[object]]::new()}
    $peers.Add($peer)
    $tcp.Connect('127.0.0.1', $port)
    $body = @{schemaVersion=6;name=$Name;c=$Character}
    if ($Udp) { $body.movementTransport = 'udp' }
    Send-Tcp $peer 'Join' $body
    Wait-Until { $null -ne $peer.accepted -and $peer.directoryReady } 'Join/directory did not complete.'
    if ($Udp) {
        Assert-True ($peer.accepted.udp.port -eq $port -and $peer.accepted.udp.token -cmatch '^[0-9a-f]{32}$') 'Missing/invalid UDP negotiation or different TCP/UDP port.'
        [byte[]]$token = [byte[]]::new(16)
        for ($index=0; $index -lt 16; $index++) { $token[$index] = [Convert]::ToByte($peer.accepted.udp.token.Substring($index*2,2),16) }
        $peer.token = $token
        $peer.udp = New-Udp $port
        # A real response is mandatory; simply sending Hello is not readiness.
        for ($attempt=0; $attempt -lt 8 -and -not $peer.ready; $attempt++) {
            Send-Udp $peer 'UdpHello' @{}
            Settle 100
        }
        Assert-True $peer.ready 'UDP Hello never received UdpReady.'
    }
    else { Assert-True ($null -eq $peer.accepted.udp) 'Legacy TCP peer was forced into UDP.' }
    return $peer
}

try {
    for ($attempt=0; $attempt -lt 3; $attempt++) {
        $probe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback,0)
        $probe.Start()
        $port = ([System.Net.IPEndPoint]$probe.LocalEndpoint).Port
        $udpProbe = $null
        try { $udpProbe = [System.Net.Sockets.UdpClient]::new([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback,$port)) }
        catch { $probe.Stop(); continue }
        $udpProbe.Dispose()
        $probe.Stop()
        $server = [System.Diagnostics.Process]::new()
        $server.StartInfo.FileName = $ServerPath
        $server.StartInfo.Arguments = "--address 127.0.0.1 --port $port --max-sessions 16"
        $server.StartInfo.UseShellExecute = $false
        $server.StartInfo.CreateNoWindow = $true
        $server.StartInfo.RedirectStandardInput = $true
        $server.StartInfo.RedirectStandardOutput = $true
        $server.StartInfo.RedirectStandardError = $true
        $server.StartInfo.StandardOutputEncoding = $utf8
        $server.StartInfo.StandardErrorEncoding = $utf8
        Assert-True ($server.Start()) 'Could not start owned test server.'
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
        if ($ready) { $stdoutRead = $server.StandardOutput.ReadToEndAsync(); break }
        if (-not $server.HasExited) { $server.Kill(); $null = $server.WaitForExit(3000) }
        $server.Dispose(); $server = $null
    }
    Assert-True ($null -ne $server -and $ready) 'Test server never bound its temporary TCP/UDP port.'
    $a = Join-Peer 'UdpA' 0 $true
    $b = Join-Peer 'UdpB' 1 $true
    $legacy = Join-Peer 'TcpLegacy' 2 $false
    Assert-True ($a.accepted.udp.token -cne $b.accepted.udp.token) 'Sessions reused their UDP token.'
    Send-Udp $a 'PlayerState' (State 0 0)
    Send-Udp $b 'PlayerState' (State 0 1)
    Send-Tcp $legacy 'PlayerState' (State 0 2)
    Wait-Until { (Has-State $b $a.id 0 'enter') -and (Has-State $a $b.id 0 'enter') -and
        (Has-State $legacy $a.id 0 'enter') } 'Initial q=0 positions did not establish reliable visibility baselines.'
    Send-Udp $a 'PlayerState' (State 1 0 1)
    Wait-Until { (Has-State $b $a.id 1 'udp') -and (Has-State $legacy $a.id 1 'tcp') } 'Mixed UDP/TCP movement did not reach the appropriate transport.'
    $moving = @($b.states | Where-Object { $_.state.id -ceq $a.id -and [uint64]$_.state.q -eq 1 })[-1]
    Assert-True ($moving.state.c -eq 0) 'UDP PlayerState spoofed its approved character.'
    $baseline = @($b.states | Where-Object { $_.state.id -ceq $a.id -and $_.transport -ceq 'enter' })[-1]
    Assert-True ([uint64]$moving.state.r -gt [uint64]$baseline.state.r) 'UDP revision did not advance beyond reliable Enter.'

    # Neither a replayed packet, stale q, nor TCP movement may replace UDP state.
    Send-Udp $a 'PlayerState' (State 999 0) 1
    Send-Udp $a 'PlayerState' (State 0 0)
    Send-Tcp $a 'PlayerState' (State 998 0)
    Settle
    Assert-True (-not (Has-State $b $a.id 999) -and -not (Has-State $b $a.id 998)) 'Replay or negotiated TCP movement was applied.'
    Assert-True (@($b.states | Where-Object { $_.state.id -ceq $a.id -and [uint64]$_.state.q -eq 0 -and
        [uint64]$_.state.r -gt [uint64]$moving.state.r }).Count -eq 0) 'A stale upstream q rolled the latest state back.'
    Send-Udp $a 'PlayerState' (State 2 0)
    Wait-Until { Has-State $b $a.id 2 'udp' } 'A stale q disturbed the next valid UDP state.'

    # Malformed JSON and unknown message types must not consume high packet sequence.
    [byte[]]$malformed = Encode-Datagram $a.token 100000 ($utf8.GetBytes('{'))
    $null = $a.udp.Send($malformed,$malformed.Length)
    Send-Udp $a 'Chat' @{text='UDP must not carry chat'} 100001
    Send-Udp $a 'PlayerState' (State 3 0)
    Wait-Until { Has-State $b $a.id 3 'udp' } 'Malformed high sequence poisoned later valid UDP traffic.'

    # Quiet-source healing repeats the same r, so dropping its first delivery is recoverable.
    $stop = @($b.states | Where-Object { $_.state.id -ceq $a.id -and [uint64]$_.state.q -eq 3 -and $_.transport -ceq 'udp' })[-1]
    Assert-True ([uint64]$stop.state.r -gt [uint64]$moving.state.r -and $stop.state.vx -eq 0) 'Stopped movement did not acquire a newer revision.'
    Wait-Until { @($b.states | Where-Object { $_.state.id -ceq $a.id -and [uint64]$_.state.q -eq 3 -and
        $_.state.r -ceq $stop.state.r -and $_.time -gt $stop.time.AddMilliseconds(350) }).Count -gt 0 } 'Stopped state was not resent with the same revision.' 2500

    # An authenticated new source port takes ownership; old lower-sequence traffic cannot steal it.
    $oldSocket = $b.udp
    $b.udp = New-Udp $port
    $b.ready = $false
    Send-Udp $b 'UdpHello' @{}
    Wait-Until { $b.ready } 'Authenticated endpoint rebind did not return UdpReady.'
    while ($oldSocket.Available -gt 0) { $endpoint=[System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any,0); $null=$oldSocket.Receive([ref]$endpoint) }
    Send-Udp $b 'UdpHello' @{} 1 $oldSocket
    Send-Udp $a 'PlayerState' (State 4 0)
    Wait-Until { Has-State $b $a.id 4 'udp' } 'Old endpoint replay stole the current binding.'
    Settle 100
    Assert-True ($oldSocket.Available -eq 0) 'Server returned movement to an obsolete endpoint.'

    $outsider = New-Udp $port
    [byte[]]$unknown = [byte[]]::new(16)
    [byte[]]$attack = Encode-Datagram $unknown 999 ($utf8.GetBytes('{"type":"UdpHello","body":{}}'))
    $null = $outsider.Send($attack,$attack.Length)
    $attack[0]=0; $null = $outsider.Send($attack,$attack.Length)
    [byte[]]$oversized = [byte[]]::new(1201); $null = $outsider.Send($oversized,$oversized.Length)
    Settle 150
    Assert-True ($outsider.Available -eq 0) 'Unknown/malformed UDP traffic elicited a response.'
    Send-Tcp $b 'Heartbeat' @{}
    Send-Tcp $b 'Chat' @{text='UDP integration still alive'}
    Wait-Until { @($legacy.events | Where-Object { $_.type -eq 'ChatMessage' -and $_.body.text -ceq 'UDP integration still alive' }).Count -eq 1 } 'UDP validation damaged the reliable control channel.'

    $departedId = $a.id
    $a.tcp.Dispose(); $a.closed = $true
    Wait-Until { @($b.events | Where-Object { $_.type -eq 'PlayerLeft' -and $_.body.id -ceq $departedId }).Count -eq 1 } 'TCP close did not remove its UDP session.'
    while ($a.udp.Available -gt 0) { $endpoint=[System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any,0); $null=$a.udp.Receive([ref]$endpoint) }
    Send-Udp $a 'UdpHello' @{} 200000
    Settle 150
    Assert-True ($a.udp.Available -eq 0) 'Closed TCP session token still authenticated UDP.'
    $replacement = Join-Peer 'UdpReplacement' 3 $true
    Assert-True ($replacement.accepted.udp.token -cne $a.accepted.udp.token) 'Reconnect reused the closed token.'
    Assert-True (-not $server.HasExited) 'Server died during the UDP integration checks.'
    Write-Output 'PASS: UDP negotiation, exact revisions, legacy TCP, replay, healing, rebind, authentication and token lifetime.'
}
finally {
    foreach ($peer in $peers) { $peer.tcp.Dispose() }
    foreach ($socket in $udpSockets) { $socket.Dispose() }
    # This Process object is created above; never enumerate or stop other servers.
    if ($null -ne $server) {
        if (-not $server.HasExited) { $server.Kill(); $null = $server.WaitForExit(3000) }
        if ($null -ne $stderrRead -and $stderrRead.IsCompleted) {
            $errorText = $stderrRead.GetAwaiter().GetResult()
            if ($errorText) { Write-Output $errorText }
        }
        $server.Dispose()
    }
}
