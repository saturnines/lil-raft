# lil-raft

bare bones implementation of raft. no deps. bring your own network/storage.

## features

- leader election
- log replication
- snapshots
- prevote

## TODO
- membership changes
- finish Lazy-ALR support for linearizable reads

## build

```
cmake -B build && cmake --build build
```


MIT