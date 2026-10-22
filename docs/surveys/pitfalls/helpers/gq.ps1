param([string]$q, [int]$n = 25)
$query = @'
query($q: String!, $n: Int!) { search(query: $q, type: ISSUE, first: $n) { issueCount nodes { ... on Issue { number state stateReason title comments { totalCount } reactions { totalCount } } ... on PullRequest { number state title comments { totalCount } reactions { totalCount } } } } }
'@
$full = "$q sort:reactions-desc"
for ($i = 0; $i -lt 3; $i++) {
  $out = gh api graphql -f query=$query -f q=$full -F n=$n --jq '.data.search.issueCount, (.data.search.nodes[] | "\(.number)\t\(.state)\t\(.stateReason)\t\(.reactions.totalCount)\t\(.comments.totalCount)\t\(.title)")'
  if ($out -and $out[0] -ne '0') { break }
  Start-Sleep -Seconds 2
}
"=== $q"
$out
