param([string[]]$Queries, [int]$PerPage = 15)
foreach ($q in $Queries) {
    "=== $q"
    $zeroRetries = 0
    for ($try = 0; $try -lt 8; $try++) {
        $out = gh api -X GET search/issues -f "q=$q" -f sort=reactions -f order=desc -f per_page=$PerPage --jq '"\(.total_count) incomplete=\(.incomplete_results)", (.items[] | "\(.number)\t\(.state)\t\(.reactions.total_count)\t\(.comments)\t\(.title)")' 2>&1
        if ($LASTEXITCODE -eq 0) {
            if (("$($out | Select-Object -First 1)" -match '^0 ') -and $zeroRetries -lt 2) { $zeroRetries++; Start-Sleep -Seconds 4; continue }
            $out; break
        }
        if ("$out" -match 'rate limit') { Start-Sleep -Seconds 15 } else { "ERR: $out"; break }
    }
    Start-Sleep -Seconds 3
}
