# Gameplay regressions assert individual profile/state events. This reader adapts
# schema 6's paged directory and AOI batches while validating raw boundaries,
# presence ordering, and no-self-state. AOI-specific tests inspect the wire itself.
$script:summitTestPeers = @{}

function Get-TestPeer {
    param([System.Net.Sockets.TcpClient]$Client)
    $key = $Client.GetHashCode()
    if (-not $script:summitTestPeers.ContainsKey($key)) {
        $script:summitTestPeers[$key] = [pscustomobject]@{
            Id = ''; Accepted = $null; Profiles = @{}; Visible = @{}
            Directory = [System.Collections.Generic.List[object]]::new()
            Events = [System.Collections.Generic.Queue[object]]::new()
            States = [System.Collections.Generic.Queue[object]]::new()
            Cursor = [uint64]0; Through = [uint64]0; Ready = $false
        }
    }
    return $script:summitTestPeers[$key]
}

function Read-SummitRawFrame {
    param([System.Net.Sockets.TcpClient]$Client)
    $stream = $Client.GetStream()
    [byte[]]$header = [byte[]]::new(4)
    Read-Exactly -Stream $stream -Buffer $header
    $length = [System.BitConverter]::ToUInt32($header, 0)
    Assert-True ($length -gt 0 -and $length -le 8192) 'Server frame violated the 8 KiB bound.'
    [byte[]]$body = [byte[]]::new([int]$length)
    Read-Exactly -Stream $stream -Buffer $body
    return ([System.Text.UTF8Encoding]::new($false, $true).GetString($body) | ConvertFrom-Json)
}

function Assert-TestNotice {
    param($Message, [string]$Kind = '', [string]$Id = '')
    Assert-True ($Message.type -eq 'ServerNotice' -and
        $Message.body.kind -in @('announcement', 'join', 'leave') -and
        -not [string]::IsNullOrWhiteSpace($Message.body.text) -and
        [System.Text.Encoding]::UTF8.GetByteCount($Message.body.text) -le 512 -and
        $null -ne $Message.body.t) 'Invalid server notice.'
    if ($Kind) {
        Assert-True ($Message.body.kind -eq $Kind -and $Message.body.id -eq $Id -and
            -not [string]::IsNullOrEmpty($Message.body.name)) 'Presence notice did not follow the real membership event.'
    }
}

function Read-SummitGameFrame {
    param([System.Net.Sockets.TcpClient]$Client, [switch]$State)
    $peer = Get-TestPeer -Client $Client
    while ($true) {
        if ($State -and $peer.States.Count -gt 0) { return $peer.States.Dequeue() }
        if (-not $State -and $null -eq $peer.Accepted -and $peer.Events.Count -gt 0) {
            return $peer.Events.Dequeue()
        }
        $message = Read-SummitRawFrame -Client $Client
        switch ($message.type) {
            'JoinAccepted' {
                Assert-True ($message.body.schemaVersion -eq 6 -and @($message.body.players).Count -eq 0) 'Unexpected Join schema/roster.'
                $peer.Id = [string]$message.body.id
                $peer.Through = [uint64]$message.body.directoryThroughId
                $peer.Accepted = $message
            }
            'DirectoryPage' {
                Assert-True (-not $peer.Ready -and $null -ne $peer.Accepted) 'Directory page outside initialization.'
                $cursor = [uint64]$message.body.cursor
                Assert-True ($cursor -gt $peer.Cursor -and $cursor -le $peer.Through -and
                    @($message.body.players).Count -gt 0 -and @($message.body.players).Count -le 32) 'Invalid directory page cursor/size.'
                foreach ($profile in $message.body.players) {
                    Assert-True ($profile.id -ne $peer.Id -and [uint64]$profile.id -gt $peer.Cursor -and
                        [uint64]$profile.id -le $cursor) 'Unordered/self directory profile.'
                    $peer.Cursor = [uint64]$profile.id
                    $peer.Profiles[[string]$profile.id] = $profile
                    $peer.Directory.Add($profile)
                    $peer.Events.Enqueue([pscustomobject]@{ type = 'PlayerJoined'; body = $profile })
                }
                $peer.Cursor = $cursor
                Send-Frame -Client $Client -Json (@{type='DirectoryAck'; body=@{cursor=[string]$message.body.cursor}} | ConvertTo-Json -Compress)
            }
            'DirectoryReady' {
                Assert-True (-not $peer.Ready -and $null -ne $peer.Accepted) 'Duplicate/unexpected DirectoryReady.'
                $peer.Ready = $true
                $accepted = $peer.Accepted
                $accepted.body.players = $peer.Directory.ToArray()
                $peer.Accepted = $null
                return $accepted
            }
            'ServerNotice' { Assert-TestNotice -Message $message }
            'VisibilityReady' { Assert-True $peer.Ready 'Visibility became ready before directory.' }
            'VisibilityEnter' {
                Assert-True ($peer.Ready -and @($message.body.players).Count -in 1..32) 'Invalid visibility enter batch.'
                foreach ($stateBody in $message.body.players) {
                    $id = [string]$stateBody.id
                    Assert-True ($id -ne $peer.Id -and $peer.Profiles.ContainsKey($id) -and
                        -not $peer.Visible.ContainsKey($id)) 'Unknown/self/duplicate visibility enter.'
                    $peer.Visible[$id] = $true
                    $peer.States.Enqueue([pscustomobject]@{type='PlayerState'; body=$stateBody})
                }
            }
            'VisibilityExit' {
                foreach ($id in $message.body.ids) {
                    Assert-True ($peer.Visible.ContainsKey([string]$id)) 'Visibility exit without enter.'
                    $peer.Visible.Remove([string]$id)
                }
            }
            'StateBatch' {
                Assert-True (@($message.body.states).Count -in 1..32) 'Invalid state batch size.'
                foreach ($stateBody in $message.body.states) {
                    $id = [string]$stateBody.id
                    Assert-True ($id -ne $peer.Id -and $peer.Visible.ContainsKey($id)) 'State without enter or self echo.'
                    $peer.States.Enqueue([pscustomobject]@{type='PlayerState'; body=$stateBody})
                }
            }
            'PlayerJoined' {
                $peer.Profiles[[string]$message.body.id] = $message.body
                Assert-TestNotice -Message (Read-SummitRawFrame -Client $Client) -Kind 'join' -Id $message.body.id
                $peer.Events.Enqueue($message)
            }
            'ProfileChanged' {
                $peer.Profiles[[string]$message.body.id] = $message.body
                $peer.Events.Enqueue($message)
            }
            'PlayerLeft' {
                $peer.Profiles.Remove([string]$message.body.id)
                $peer.Visible.Remove([string]$message.body.id)
                Assert-TestNotice -Message (Read-SummitRawFrame -Client $Client) -Kind 'leave' -Id $message.body.id
                $peer.Events.Enqueue($message)
            }
            default { $peer.Events.Enqueue($message) }
        }
    }
}
