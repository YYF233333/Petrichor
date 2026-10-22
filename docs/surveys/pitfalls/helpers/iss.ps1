param([string]$repo, [int[]]$nums)
$owner, $name = $repo.Split('/')
foreach ($n in $nums) {
  $query = 'query($owner: String!, $name: String!, $n: Int!) { repository(owner: $owner, name: $name) { issueOrPullRequest(number: $n) { ... on Issue { number title state stateReason closedAt reactions { totalCount } comments { totalCount } labels(first: 10) { nodes { name } } timelineItems(first: 100, itemTypes: [MARKED_AS_DUPLICATE_EVENT, CROSS_REFERENCED_EVENT, CLOSED_EVENT]) { totalCount nodes { __typename ... on MarkedAsDuplicateEvent { duplicate { ... on Issue { number } } canonical { ... on Issue { number } } } ... on CrossReferencedEvent { source { __typename ... on Issue { number title } ... on PullRequest { number title merged } } } ... on ClosedEvent { closer { __typename ... on PullRequest { number title } ... on Commit { oid } } } } } } ... on PullRequest { number title state merged mergedAt reactions { totalCount } comments { totalCount } } } } }'
  $j = gh api graphql -f query=$query -f owner=$owner -f name=$name -F n=$n | ConvertFrom-Json -DateKind String
  $i = $j.data.repository.issueOrPullRequest
  "#{0} {1}/{2} closed={3} {4}r {5}c [{6}] {7}" -f $i.number, $i.state, $i.stateReason, $i.closedAt, $i.reactions.totalCount, $i.comments.totalCount, (($i.labels.nodes | ForEach-Object { $_.name }) -join ','), $i.title
  if ($i.merged -ne $null) { "  PR merged=$($i.merged) at $($i.mergedAt)" }
  $dupOf = @(); $dups = @(); $xi = 0; $xp = @(); $closer = @()
  foreach ($t in $i.timelineItems.nodes) {
    if ($t.__typename -eq 'MarkedAsDuplicateEvent') { if ($t.duplicate.number -eq $n) { $dupOf += $t.canonical.number } else { $dups += $t.duplicate.number } }
    elseif ($t.__typename -eq 'CrossReferencedEvent') { if ($t.source.__typename -eq 'Issue') { $xi++ } elseif ($t.source.__typename -eq 'PullRequest') { $xp += "$($t.source.number)(merged=$($t.source.merged))" } }
    elseif ($t.__typename -eq 'ClosedEvent') { if ($t.closer.__typename -eq 'PullRequest') { $closer += "PR#$($t.closer.number)" } elseif ($t.closer.oid) { $closer += "commit $($t.closer.oid.Substring(0,9))" } }
  }
  "  dupOf=[$($dupOf -join ',')] dupsMarked=[$($dups -join ',')] xrefIssues=$xi xrefPRs=[$($xp -join ',')] closer=[$($closer -join ',')] timelineTotal=$($i.timelineItems.totalCount)"
}
