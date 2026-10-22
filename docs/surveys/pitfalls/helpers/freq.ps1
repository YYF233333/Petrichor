param([string[]]$queries, [int]$maxPages = 4)
foreach ($q in $queries) {
  $enc = [uri]::EscapeDataString($q)
  $so = @()
  foreach ($tag in 'godot','gdscript') {
    try { $r = Invoke-RestMethod -Uri "https://api.stackexchange.com/2.3/search/advanced?q=$enc&tagged=$tag&site=stackoverflow&filter=total" -TimeoutSec 20; $so += "$tag=$($r.total)" } catch { $so += "$tag=ERR" }
  }
  $topics = @{}
  $more = $false
  for ($p = 1; $p -le $maxPages; $p++) {
    try { $f = Invoke-RestMethod -Uri "https://forum.godotengine.org/search.json?q=$enc&page=$p" -TimeoutSec 20 } catch { break }
    foreach ($t in $f.topics) { $topics[$t.id] = $t.title }
    $more = [bool]$f.grouped_search_result.more_full_page_results
    if (-not $more) { break }
    Start-Sleep -Milliseconds 400
  }
  $cap = if ($more) { "+ (capped)" } else { "" }
  "[$q] SO: $($so -join ' ') | forum topics: $($topics.Count)$cap"
  $topics.Values | Select-Object -First 6 | ForEach-Object { "    - $_" }
}
