# lil-raft

bare bones implementation of  raft. no deps. bring your own network/storage.

## features

- leader election
- log replication
- linearizable reads (lazy-alr) ()

## not included (out of scope)

- snapshots
- membership changes
- prevote

## build

```
cmake -B build && cmake --build build
```


MIT