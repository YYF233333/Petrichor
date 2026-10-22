param([string]$repo, [string]$label, [string]$out)
$owner, $name = $repo.Split('/')
$query = 'query($owner: String!, $name: String!, $label: String!, $cursor: String) { repository(owner: $owner, name: $name) { issues(labels: [$label], first: 100, after: $cursor) { totalCount pageInfo { hasNextPage endCursor } nodes { number title state stateReason createdAt closedAt reactions { totalCount } comments { totalCount } } } } }'
$cursor = $null
$rows = New-Object System.Collections.Generic.List[string]
do {
  if ($cursor) { $raw = gh api graphql -f query=$query -f owner=$owner -f name=$name -f label=$label -f cursor=$cursor }
  else { $raw = gh api graphql -f query=$query -f owner=$owner -f name=$name -f label=$label }
  $json = $raw | ConvertFrom-Json
  $iss = $json.data.repository.issues
  foreach ($it in $iss.nodes) {
    $closed = if ($it.closedAt) { ([string]$it.closedAt).Substring(0,10) } else { "" }
    $rows.Add(("{0}`t{1}`t{2}`t{3}`t{4}`t{5}`t{6}`t{7}" -f $it.number, $it.state, $it.stateReason, $it.reactions.totalCount, $it.comments.totalCount, ([string]$it.createdAt).Substring(0,10), $closed, ($it.title -replace "`t", " ")))
  }
  $cursor = $iss.pageInfo.endCursor
} while ($iss.pageInfo.hasNextPage)
$rows | Set-Content -Encoding utf8 $out
"total reported: $($iss.totalCount); dumped: $($rows.Count)"
