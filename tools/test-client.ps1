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
    while ($read -lt 4) { $read += $stream.Read($header, $read, 4 - $read) }
    $len = ($header[0] -shl 24) -bor ($header[1] -shl 16) -bor ($header[2] -shl 8) -bor $header[3]
    $body = New-Object byte[] $len
    $read = 0
    while ($read -lt $len) { $read += $stream.Read($body, $read, $len - $read) }
    return [System.Text.Encoding]::UTF8.GetString($body)
}

$client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 7777)
$stream = $client.GetStream()