# Dashboard

- 2026-10-29T14:15:26Z Open bug: a freed object in a non-null T passed to the engine (argument, property write, member the engine reads) reaches it as a stale pointer; official 4.7.2 can then corrupt the heap (0xC0000374). Spec §3.3; fix not decided.
