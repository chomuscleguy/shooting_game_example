function Send-Framed($stream, $message) {
    $body = [System.Text.Encoding]::UTF8.GetBytes($message)
    $len = $body.Length
    $header = [byte[]]@(
        (($len -shr 24) -band 0xFF),
        (($len -shr 16) -band 0xFF),
        (($len -shr 8) -band 0xFF),
        ($len -band 0xFF)
    )
    $stream.Write($header, 0, 4)
    $stream.Write($body, 0, $body.Length)
    $stream.Flush()
}

function Read-Framed($stream) {
    $header = New-Object byte[] 4
    $read = 0
    while ($read -lt 4) {
        $n = $stream.Read($header, $read, 4 - $read)
        if ($n -le 0) { throw "connection closed" }  
        $read += $n
    }
    $len = ($header[0] -shl 24) -bor ($header[1] -shl 16) -bor ($header[2] -shl 8) -bor $header[3]
    if ($len -le 0 -or $len -gt 1048576) { 
        throw "frame desync (len=$len) - 이 연결은 더 못 씁니다"
    }
    $body = New-Object byte[] $len
    $read = 0
    while ($read -lt $len) {
        $n = $stream.Read($body, $read, $len - $read)
        if ($n -le 0) { throw "connection closed" }   
        $read += $n
    }
    return [System.Text.Encoding]::UTF8.GetString($body)
}

# 브로드캐스트는 요청 없이 서버가 먼저 보낸다. Read-Framed는 올 때까지 막히므로,
# "지금 도착해 있는 것만 전부" 꺼내는 함수가 따로 필요하다.
function Read-Available($stream, $waitMs = 300) {
    Start-Sleep -Milliseconds $waitMs
    try {
        while ($stream.DataAvailable) { Read-Framed $stream }
    } catch {
        Write-Warning "읽기 중단: $($_.Exception.Message)"
        $stream.Close()   
    }
}

# 한 창에서 여러 연결을 다루기 위한 것. 브로드캐스트를 확인하려면
# 보내는 쪽과 받는 쪽이 동시에 살아있어야 한다.
$script:clients = @()
function New-Client($hostname = "127.0.0.1", $port = 7777) {
    $c = New-Object System.Net.Sockets.TcpClient($hostname, $port)
    $script:clients += $c
    $s = $c.GetStream()
    $s.ReadTimeout = 3000   
    return $s
}

function Close-Clients {
    foreach ($c in $script:clients) { $c.Close() }
    $script:clients = @()
}
