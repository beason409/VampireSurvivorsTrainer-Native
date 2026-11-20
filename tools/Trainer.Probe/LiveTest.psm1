function Get-TrainerSnapshot {
    param([string]$LogPath='artifacts\stat-full-session.jsonl')
    $latest=$null
    foreach($line in @(Get-Content -LiteralPath $LogPath -Tail 12 -ErrorAction SilentlyContinue)) {
        try { $item=$line|ConvertFrom-Json -Depth 100; if($item.response.state) {$latest=$item} } catch { }
    }
    return $latest
}
function Send-TrainerTestCommand {
    param([Parameter(Mandatory)][hashtable]$Command,[string]$LogPath='artifacts\stat-full-session.jsonl',[string]$InboxPath='artifacts\stat-full-inbox.jsonl')
    $request=$Command.Clone();$request.id=[Guid]::NewGuid().ToString('N')
    $request|ConvertTo-Json -Compress|Add-Content -LiteralPath $InboxPath -Encoding utf8
    $deadline=[DateTime]::UtcNow.AddSeconds(5)
    do {
        foreach($line in @(Get-Content -LiteralPath $LogPath -Tail 16 -ErrorAction SilentlyContinue)) {
            try {$item=$line|ConvertFrom-Json -Depth 100} catch {continue}
            if($item.response.id -eq $request.id) {
                if(!$item.response.ok){throw $item.response.error}
                return $item
            }
        }
        Start-Sleep -Milliseconds 100
    }while([DateTime]::UtcNow -lt $deadline)
    throw 'Test command acknowledgment timed out.'
}
Export-ModuleMember -Function Get-TrainerSnapshot,Send-TrainerTestCommand
