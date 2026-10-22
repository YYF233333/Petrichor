param([int[]]$nums, [string]$repo = 'godotengine/godot')
$owner, $name = $repo.Split('/')
foreach ($n in $nums) {
  $query = 'query($owner: String!, $name: String!, $n: Int!) { repository(owner: $owner, name: $name) { issueOrPullRequest(number: $n) { ... on Issue { number title state stateReason closedAt reactions { totalCount } comments { totalCount } timelineItems(first: 100, itemTypes: [MARKED_AS_DUPLICATE_EVENT, CROSS_REFERENCED_EVENT]) { totalCount nodes { __typename ... on MarkedAsDuplicateEvent { duplicate { ... on Issue { number } } canonical { ... on Issue { number } } } ... on CrossReferencedEvent { source { __typename ... on Issue { number } ... on PullRequest { number merged } } } } } } } } }'
  $j = gh api graphql -f query=$query -f owner=$owner -f name=$name -F n=$n | ConvertFrom-Json -DateKind String
  $i = $j.data.repository.issueOrPullRequest
  $dupOf = @(); $dups = @(); $xi = 0; $xp = @()
  foreach ($t in $i.timelineItems.nodes) {
    if ($t.__typename -eq 'MarkedAsDuplicateEvent') { if ($t.duplicate.number -eq $n) { $dupOf += $t.canonical.number } else { $dups += $t.duplicate.number } }
    elseif ($t.__typename -eq 'CrossReferencedEvent') { if ($t.source.__typename -eq 'Issue') { $xi++ } elseif ($t.source.__typename -eq 'PullRequest') { $xp += "$($t.source.number)(m=$($t.source.merged))" } }
  }
  "#{0} {1}/{2} {3}r {4}c dupOf=[{5}] dupsMarked=[{6}] xrefIssues={7} xrefPRs=[{8}] {9}" -f $i.number, $i.state, $i.stateReason, $i.reactions.totalCount, $i.comments.totalCount, ($dupOf -join ','), ($dups -join ','), $xi, ($xp -join ','), $i.title
}
