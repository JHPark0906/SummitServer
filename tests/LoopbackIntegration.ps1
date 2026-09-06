param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ServerPath
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ProtocolTestReader.ps1')

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Send-Frame {
    param(
        [System.Net.Sockets.TcpClient]$Client,
        [string]$Json
    )

    Send-BytesFrame -Client $Client -Body ([System.Text.Encoding]::UTF8.GetBytes($Json))
}

function Send-BytesFrame {
    param(
        [System.Net.Sockets.TcpClient]$Client,
        [byte[]]$Body
    )

    [byte[]]$header = [System.BitConverter]::GetBytes([uint32]$Body.Length)
    $stream = $Client.GetStream()
    $stream.Write($header, 0, $header.Length)
    $stream.Write($Body, 0, $Body.Length)
    $stream.Flush()
}

function Read-Exactly {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [byte[]]$Buffer
    )

    $offset = 0
    while ($offset -lt $Buffer.Length) {
        $read = $Stream.Read($Buffer, $offset, $Buffer.Length - $offset)
        if ($read -eq 0) {
            throw 'Peer closed before a complete frame arrived.'
        }
        $offset += $read
    }
}

function Read-Frame {
    param([System.Net.Sockets.TcpClient]$Client, [switch]$State)
    return (Read-SummitGameFrame -Client $Client -State:$State | ConvertTo-Json -Depth 8 -Compress)
}

function Read-CorrelatedState {
    param([System.Net.Sockets.TcpClient]$Client, [string]$Id, [uint64]$Sequence)
    # Control reads can queue earlier AOI states. Match this request's q rather
    # than assuming the next buffered state was produced after ProfileChanged.
    # Matching on character would hide precisely the server bug under test.
    for ($attempt = 0; $attempt -lt 256; $attempt++) {
        $message = (Read-Frame -Client $Client -State) | ConvertFrom-Json
        if ([string]$message.body.id -ceq $Id -and [uint64]$message.body.q -eq $Sequence) {
            return $message
        }
    }
    throw "No state matched player $Id sequence $Sequence."
}

function Connect-TestClient {
    param(
        [int]$Port,
        [System.Diagnostics.Process]$ServerProcess
    )

    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        if ($ServerProcess.HasExited) {
            throw "SummitServer exited during startup with code $($ServerProcess.ExitCode)."
        }

        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $client.Connect('127.0.0.1', $Port)
            $client.NoDelay = $true
            $client.ReceiveTimeout = 3000
            $client.SendTimeout = 3000
            return $client
        }
        catch {
            $client.Dispose()
            Start-Sleep -Milliseconds 50
        }
    }

    throw 'SummitServer did not accept a loopback connection.'
}

$serverProcess = $null
$first = $null
$second = $null
$third = $null
$preJoin = $null
$legacy = $null
try {
    # Releasing a port probe before the server binds has an unavoidable race.
    # Retry the complete startup with a fresh candidate if another process wins
    # it, while avoiding a hard-coded developer port.
    for ($startupAttempt = 0; $startupAttempt -lt 5; $startupAttempt++) {
        $portProbe = [System.Net.Sockets.TcpListener]::new(
            [System.Net.IPAddress]::Loopback, 0)
        $portProbe.Start()
        $testPort = ([System.Net.IPEndPoint]$portProbe.LocalEndpoint).Port
        $portProbe.Stop()

        $candidateProcess = Start-Process -FilePath $ServerPath `
            -ArgumentList @('--address', '127.0.0.1', '--port', "$testPort") `
            -WindowStyle Hidden `
            -PassThru
        try {
            $candidateClient = Connect-TestClient `
                -Port $testPort `
                -ServerProcess $candidateProcess
            $serverProcess = $candidateProcess
            $first = $candidateClient
            break
        }
        catch {
            if (-not $candidateProcess.HasExited) {
                Stop-Process -Id $candidateProcess.Id
                $candidateProcess.WaitForExit()
            }
            if ($startupAttempt -eq 4) {
                throw
            }
        }
    }

    Assert-True ($null -ne $serverProcess -and $null -ne $first) `
        'SummitServer could not be started for the loopback test.'
    $legacy = Connect-TestClient -Port $testPort -ServerProcess $serverProcess
    Send-Frame -Client $legacy -Json '{"type":"Join","body":{"schemaVersion":4,"c":0,"name":"Legacy"}}'
    $legacyRejection = (Read-Frame -Client $legacy) | ConvertFrom-Json
    Assert-True ($legacyRejection.type -eq 'JoinRejected' -and $legacyRejection.error.code -eq 'schema_mismatch') 'A v4 client was not rejected by schema negotiation.'
    [byte[]]$afterLegacy = [byte[]]::new(1)
    Assert-True ($legacy.GetStream().Read($afterLegacy, 0, 1) -eq 0) 'Legacy schema rejection did not close the connection.'
    $legacy.Dispose()
    $legacy = $null
    $preJoin = Connect-TestClient -Port $testPort -ServerProcess $serverProcess
    Send-Frame -Client $preJoin -Json '{"type":"Chat","body":{"text":"too early"}}'
    $preJoinRejection = (Read-Frame -Client $preJoin) | ConvertFrom-Json
    Assert-True ($preJoinRejection.type -eq 'JoinRejected' -and $preJoinRejection.error.code -eq 'protocol_violation') 'Pre-Join chat was not a terminal protocol violation.'
    [byte[]]$afterPreJoin = [byte[]]::new(1)
    Assert-True ($preJoin.GetStream().Read($afterPreJoin, 0, 1) -eq 0) 'Pre-Join chat rejection did not close the connection.'
    $preJoin.Dispose()
    $preJoin = $null
    Send-Frame -Client $first `
        -Json '{"type":"Join","body":{"schemaVersion":6,"c":0,"name":"Alice"}}'
    $firstAccepted = (Read-Frame -Client $first) | ConvertFrom-Json
    Assert-True ($firstAccepted.type -eq 'JoinAccepted') `
        'First client did not receive JoinAccepted.'
    Assert-True (-not [string]::IsNullOrEmpty([string]$firstAccepted.body.id)) `
        'First JoinAccepted did not contain an id.'

    $second = Connect-TestClient -Port $testPort -ServerProcess $serverProcess
    Send-Frame -Client $second `
        -Json '{"type":"Join","body":{"schemaVersion":6,"c":0,"name":"Alice"}}'
    $duplicate = (Read-Frame -Client $second) | ConvertFrom-Json
    Assert-True ($duplicate.type -eq 'JoinRejected') `
        'Duplicate nickname did not produce JoinRejected.'
    Assert-True ($duplicate.error.code -eq 'name_duplicate') `
        'Duplicate nickname did not use name_duplicate.'

    Send-Frame -Client $second `
        -Json '{"type":"Join","body":{"schemaVersion":6,"c":0,"name":"Bob"}}'
    $secondAccepted = (Read-Frame -Client $second) | ConvertFrom-Json
    Assert-True ($secondAccepted.type -eq 'JoinAccepted') `
        'Same-connection nickname retry was not accepted.'
    Assert-True (-not [string]::IsNullOrEmpty([string]$secondAccepted.body.id)) `
        'Second JoinAccepted did not contain an id.'

    $existingForSecond = (Read-Frame -Client $second) | ConvertFrom-Json
    Assert-True ($existingForSecond.type -eq 'PlayerJoined') `
        'PlayerJoined did not follow the second JoinAccepted.'
    Assert-True ($existingForSecond.body.name -eq 'Alice') `
        'Second client received the wrong existing player.'

    $joinedForFirst = (Read-Frame -Client $first) | ConvertFrom-Json
    Assert-True ($joinedForFirst.type -eq 'PlayerJoined') `
        'First client did not receive PlayerJoined.'
    Assert-True ($joinedForFirst.body.name -eq 'Bob') `
        'First client received the wrong joined player.'

    # Queue the burst before reading either response; echo is the authoritative acceptance.
    Send-Frame -Client $first -Json '{"type":"Chat","body":{"text":"  \uc548\ub155 \ud83d\udc4b  ","id":"spoofed","name":"Spoofed","t":0}}'
    Send-Frame -Client $first -Json '{"type":"Chat","seq":"burst","body":{"text":"too soon"}}'
    $firstChat = (Read-Frame -Client $first) | ConvertFrom-Json
    $chatRateRejected = (Read-Frame -Client $first) | ConvertFrom-Json
    $otherChat = (Read-Frame -Client $second) | ConvertFrom-Json
    $unicodeChat = ('"  \uc548\ub155 \ud83d\udc4b  "' | ConvertFrom-Json)
    foreach ($chat in @($firstChat, $otherChat)) {
        Assert-True ($chat.type -eq 'ChatMessage' -and $chat.body.id -eq $firstAccepted.body.id -and $chat.body.name -eq 'Alice' -and $chat.body.text -eq $unicodeChat -and $null -ne $chat.body.t) 'Chat did not preserve Unicode/spaces or trusted client identity/name/time.'
    }
    Assert-True ($chatRateRejected.type -eq 'ChatRejected' -and $chatRateRejected.error.code -eq 'chat_rate_limited' -and $chatRateRejected.seq -eq 'burst') 'Chat burst did not receive a correlated non-terminal rejection.'

    foreach ($invalidChat in @(
        '{"type":"Chat","body":{"text":""}}',
        '{"type":"Chat","body":{"text":"   "}}',
        '{"type":"Chat","body":{"text":"hello\nworld"}}',
        '{"type":"Chat","body":{"text":"hello\u007f"}}',
        '{"type":"Chat","body":{"text":123}}',
        ('{"type":"Chat","body":{"text":"' + ('x' * 513) + '"}}')
    )) {
        Send-Frame -Client $second -Json $invalidChat
        $invalidRejected = (Read-Frame -Client $second) | ConvertFrom-Json
        Assert-True ($invalidRejected.type -eq 'ChatRejected' -and $invalidRejected.error.code -eq 'chat_invalid') 'Invalid chat was accepted or closed the connection.'
    }
    # 170 Korean characters plus two ASCII characters are exactly 512 UTF-8 bytes.
    $maximumChat = (('\ud55c' * 170) + 'ab')
    Send-Frame -Client $second -Json ('{"type":"Chat","body":{"text":"' + $maximumChat + '"}}')
    foreach ($client in @($first, $second)) {
        $boundaryChat = (Read-Frame -Client $client) | ConvertFrom-Json
        Assert-True ($boundaryChat.type -eq 'ChatMessage' -and $boundaryChat.body.id -eq $secondAccepted.body.id -and [System.Text.Encoding]::UTF8.GetByteCount($boundaryChat.body.text) -eq 512) 'Exact UTF-8 byte boundary failed, an invalid request consumed cooldown, or one session limited another.'
    }

    # Both viewers need a real position before AOI can establish visibility.
    Send-Frame -Client $second -Json '{"type":"PlayerState","body":{"x":8,"y":4,"vx":0,"vy":0,"f":1,"s":"idle","c":0}}'
    Send-Frame -Client $first `
        -Json '{"type":"PlayerState","body":{"x":1.5,"y":3.0,"vx":0.25,"vy":-2.0,"f":-1,"s":"jump","c":1}}'
    $firstRelay = (Read-Frame -Client $second -State) | ConvertFrom-Json
    Assert-True ($firstRelay.type -eq 'PlayerState') `
        'First PlayerState was not relayed.'
    Assert-True ([string]$firstRelay.body.id -eq [string]$firstAccepted.body.id) `
        'First PlayerState relay used the wrong id.'
    Assert-True ($null -ne $firstRelay.body.t) `
        'First PlayerState relay did not contain server time.'

    # This creates a causal no-self-echo assertion without relying on a short
    # timeout: if the first state had been echoed, it would be the next frame
    # read by the first client instead of Bob's state.
    $secondRelay = (Read-Frame -Client $first -State) | ConvertFrom-Json
    Assert-True ($secondRelay.type -eq 'PlayerState') `
        'Second PlayerState was not relayed.'
    Assert-True ([string]$secondRelay.body.id -eq [string]$secondAccepted.body.id) `
        'PlayerState was echoed to its sender or used the wrong id.'

    Assert-True ($secondAccepted.body.players[0].name -eq 'Alice' -and $secondAccepted.body.players[0].c -eq 0) 'Join roster is missing the initial profile.'
    Send-Frame -Client $first -Json '{"type":"SetProfile","body":{"name":"NewAlice","c":1}}'
    $selfChanged = (Read-Frame -Client $first) | ConvertFrom-Json
    $otherChanged = (Read-Frame -Client $second) | ConvertFrom-Json
    foreach ($change in @($selfChanged, $otherChanged)) {
        Assert-True ($change.type -eq 'ProfileChanged' -and $change.body.name -eq 'NewAlice' -and $change.body.c -eq 1 -and $change.body.id -eq $firstAccepted.body.id) 'Approved profile was not broadcast to everyone.'
    }
    Start-Sleep -Milliseconds 510
    Send-Frame -Client $first -Json '{"type":"Chat","body":{"text":"after profile change"}}'
    foreach ($client in @($first, $second)) {
        $renamedChat = (Read-Frame -Client $client) | ConvertFrom-Json
        Assert-True ($renamedChat.type -eq 'ChatMessage' -and $renamedChat.body.name -eq 'NewAlice' -and $renamedChat.body.id -eq $firstAccepted.body.id -and [long]$renamedChat.body.t -ge ([long]$firstChat.body.t + 500)) 'Recovered chat did not use the newly approved profile.'
    }
    Assert-True ($firstChat.body.name -eq 'Alice') 'An earlier chat did not retain its name snapshot.'
    Send-Frame -Client $first -Json '{"type":"SetProfile","body":{"name":"Bob","c":0}}'
    $profileRejected = (Read-Frame -Client $first) | ConvertFrom-Json
    Assert-True ($profileRejected.type -eq 'ProfileRejected' -and $profileRejected.error.code -eq 'name_duplicate') 'Duplicate profile was not rejected.'
    # The next state must carry the previously approved character despite a stale client c.
    Send-Frame -Client $first -Json '{"type":"PlayerState","body":{"x":1,"y":3,"vx":0,"vy":0,"f":1,"s":"idle","c":0,"q":2}}'
    $approvedState = Read-CorrelatedState -Client $second -Id $firstAccepted.body.id -Sequence 2
    Assert-True ($approvedState.type -eq 'PlayerState' -and $approvedState.body.c -eq 1) 'Rejected profile or stale state changed approved character.'

    foreach ($character in @(2, 3, 4)) {
        $profile = @{ type = 'SetProfile'; body = @{ name = 'NewAlice'; c = $character } }
        Send-Frame -Client $first -Json ($profile | ConvertTo-Json -Compress)
        foreach ($client in @($first, $second)) {
            $change = (Read-Frame -Client $client) | ConvertFrom-Json
            Assert-True ($change.type -eq 'ProfileChanged' -and $change.body.c -eq $character) 'C/D/E profile selection did not reach everyone.'
        }
    }
    # JSON escapes keep this script portable to Windows PowerShell's legacy source encoding.
    Send-Frame -Client $first -Json '{"type":"SetProfile","body":{"name":"\uc0c1\uc5ec\uc790","c":4}}'
    $specialName = ('"\uc0c1\uc5ec\uc790"' | ConvertFrom-Json)
    foreach ($client in @($first, $second)) {
        $change = (Read-Frame -Client $client) | ConvertFrom-Json
        Assert-True ($change.type -eq 'ProfileChanged' -and $change.body.name -eq $specialName -and $change.body.c -eq 5) 'Special nickname did not automatically select tdw.'
    }
    Send-Frame -Client $first -Json '{"type":"SetProfile","body":{"name":"NewAlice","c":5}}'
    $tdwRejected = (Read-Frame -Client $first) | ConvertFrom-Json
    Assert-True ($tdwRejected.type -eq 'ProfileRejected' -and $tdwRejected.error.code -eq 'character_invalid') 'Direct tdw profile selection was accepted.'
    Send-Frame -Client $first -Json '{"type":"PlayerState","body":{"x":1,"y":3,"vx":0,"vy":0,"f":1,"s":"idle","c":5,"q":3}}'
    $tdwState = Read-CorrelatedState -Client $second -Id $firstAccepted.body.id -Sequence 3
    Assert-True ($tdwState.type -eq 'PlayerState' -and $tdwState.body.c -eq 5) 'tdw movement failed or a rejected profile changed it.'

    $third = Connect-TestClient -Port $testPort -ServerProcess $serverProcess
    Send-Frame -Client $third -Json '{"type":"Join","body":{"schemaVersion":6,"name":"\uc2ec\uc2ec\uc774\uc2ec\uc154","c":5}}'
    $directJoinRejected = (Read-Frame -Client $third) | ConvertFrom-Json
    Assert-True ($directJoinRejected.type -eq 'JoinRejected' -and $directJoinRejected.error.code -eq 'character_invalid') 'Direct tdw Join selection was accepted.'
    Send-Frame -Client $third -Json '{"type":"Join","body":{"schemaVersion":6,"name":"\uc2ec\uc2ec\uc774\uc2ec\uc154","c":3}}'
    $thirdAccepted = (Read-Frame -Client $third) | ConvertFrom-Json
    Assert-True ($thirdAccepted.type -eq 'JoinAccepted' -and $thirdAccepted.body.c -eq 5 -and $thirdAccepted.body.schemaVersion -eq 6) 'Special nickname Join did not normalize to tdw.'
    $tdwRoster = @($thirdAccepted.body.players | Where-Object { $_.id -eq $firstAccepted.body.id })
    Assert-True ($tdwRoster.Count -eq 1 -and $tdwRoster[0].name -eq $specialName -and $tdwRoster[0].c -eq 5) 'Late Join roster lost the approved tdw profile.'
    $existingEvents = @((Read-Frame -Client $third | ConvertFrom-Json), (Read-Frame -Client $third | ConvertFrom-Json))
    $tdwExisting = @($existingEvents | Where-Object { $_.body.id -eq $firstAccepted.body.id })
    Assert-True ($tdwExisting.Count -eq 1 -and $tdwExisting[0].type -eq 'PlayerJoined' -and $tdwExisting[0].body.c -eq 5) 'Existing-player event lost the approved tdw character.'
    foreach ($client in @($first, $second)) {
        $joined = (Read-Frame -Client $client) | ConvertFrom-Json
        Assert-True ($joined.type -eq 'PlayerJoined' -and $joined.body.id -eq $thirdAccepted.body.id -and $joined.body.c -eq 5) 'New tdw profile did not reach existing clients.'
    }
    Send-Frame -Client $third -Json '{"type":"Chat","body":{"text":"new arrival"}}'
    foreach ($client in @($first, $second, $third)) {
        $lateChat = (Read-Frame -Client $client) | ConvertFrom-Json
        Assert-True ($lateChat.type -eq 'ChatMessage' -and $lateChat.body.text -eq 'new arrival' -and $lateChat.body.id -eq $thirdAccepted.body.id -and $lateChat.body.name -eq $thirdAccepted.body.name) 'A late joiner received chat history or a new chat failed to reach all current players.'
    }
    $third.Dispose()
    $third = $null
    foreach ($client in @($first, $second)) {
        $left = (Read-Frame -Client $client) | ConvertFrom-Json
        Assert-True ($left.type -eq 'PlayerLeft' -and $left.body.id -eq $thirdAccepted.body.id) 'tdw disconnect did not release membership.'
    }
    Send-Frame -Client $first -Json '{"type":"SetProfile","body":{"name":"NewAlice","c":4}}'
    foreach ($client in @($first, $second)) {
        $change = (Read-Frame -Client $client) | ConvertFrom-Json
        Assert-True ($change.type -eq 'ProfileChanged' -and $change.body.name -eq 'NewAlice' -and $change.body.c -eq 4) 'Leaving the special nickname did not restore the ordinary choice.'
    }
    Send-Frame -Client $first -Json '{"type":"PlayerState","body":{"x":1,"y":3,"vx":0,"vy":0,"f":1,"s":"idle","c":5,"q":4}}'
    $ordinaryState = Read-CorrelatedState -Client $second -Id $firstAccepted.body.id -Sequence 4
    Assert-True ($ordinaryState.type -eq 'PlayerState' -and $ordinaryState.body.c -eq 4) 'A stale tdw state bypassed the approved ordinary profile.'
    # An authenticated client may not Join twice. The rejection is terminal:
    # ServerCore must drain the final frame and then deliver FIN.
    Send-Frame -Client $second `
        -Json '{"type":"Join","body":{"schemaVersion":6,"c":0,"name":"Again"}}'
    $protocolRejection = (Read-Frame -Client $second) | ConvertFrom-Json
    Assert-True ($protocolRejection.type -eq 'JoinRejected') `
        'Repeated Join did not produce a final JoinRejected.'
    Assert-True ($protocolRejection.error.code -eq 'protocol_violation') `
        'Repeated Join did not use protocol_violation.'

    [byte[]]$afterFinalFrame = [byte[]]::new(1)
    $bytesAfterFinalFrame = $second.GetStream().Read($afterFinalFrame, 0, 1)
    Assert-True ($bytesAfterFinalFrame -eq 0) `
        'Connection did not deliver FIN after the final rejection frame.'

    $leftForFirst = (Read-Frame -Client $first) | ConvertFrom-Json
    Assert-True ($leftForFirst.type -eq 'PlayerLeft') `
        'Remaining client did not receive PlayerLeft.'
    Assert-True ([string]$leftForFirst.body.id -eq [string]$secondAccepted.body.id) `
        'PlayerLeft used the wrong id.'

    # A malformed UTF-8 envelope is rejected by ServerCore before the Chat handler can run.
    [byte[]]$invalidUtf8 = [System.Text.Encoding]::ASCII.GetBytes('{"type":"Chat","body":{"text":"') +
        [byte[]]@(0xc0, 0xaf) + [System.Text.Encoding]::ASCII.GetBytes('"}}')
    Send-BytesFrame -Client $first -Body $invalidUtf8
    [byte[]]$afterInvalidUtf8 = [byte[]]::new(1)
    Assert-True ($first.GetStream().Read($afterInvalidUtf8, 0, 1) -eq 0) 'Malformed UTF-8 JSON reached a game handler or left the connection open.'

    $serverProcess.Refresh()
    Assert-True (-not $serverProcess.HasExited) `
        'SummitServer exited unexpectedly before test cleanup.'
    Write-Output "SummitServer loopback test passed on port $testPort."
}
finally {
    if ($null -ne $first) {
        $first.Dispose()
    }
    if ($null -ne $second) {
        $second.Dispose()
    }
    if ($null -ne $third) {
        $third.Dispose()
    }
    if ($null -ne $preJoin) {
        $preJoin.Dispose()
    }
    if ($null -ne $legacy) {
        $legacy.Dispose()
    }
    if ($null -ne $serverProcess -and -not $serverProcess.HasExited) {
        Stop-Process -Id $serverProcess.Id
        $serverProcess.WaitForExit()
    }
}
