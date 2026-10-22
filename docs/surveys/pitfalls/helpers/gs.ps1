param([string]$q, [int]$n = 30)
$query = 'query($q: String!, $n: Int!) { search(query: $q, type: ISSUE, first: $n) { issueCount nodes { ... on Issue { number title state stateReason createdAt closedAt reactions { totalCount } comments { totalCount } } ... on PullRequest { number title state createdAt reactions { totalCount } comments { totalCount } } } } }'
$json = gh api graphql -f query=$query -f q=$q -F n=$n | ConvertFrom-Json -DateKind String
"total: " + $json.data.search.issueCount
foreach ($it in $json.data.search.nodes) {
  "{0}`t{1}/{2}`t{3}r`t{4}c`t{5}`t{6}" -f $it.number, $it.state, $it.stateReason, $it.reactions.totalCount, $it.comments.totalCount, ([string]$it.createdAt).Substring(0,10), $it.title
}
