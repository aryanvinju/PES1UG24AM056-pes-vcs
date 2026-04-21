# PES-VCS Lab Report

Name: Aditya Shetty  
SRN: PES1UG24AM018  
Repository: `PES1UG24AM018-pes-vcs`

## Screenshot Index

- 1A: [screenshots/1A_test_objects.png](./screenshots/1A_test_objects.png)
- 1B: [screenshots/1B_objects_find.png](./screenshots/1B_objects_find.png)
- 2A: [screenshots/2A_test_tree.png](./screenshots/2A_test_tree.png)
- 2B: [screenshots/2B_tree_hexdump.png](./screenshots/2B_tree_hexdump.png)
- 3A: [screenshots/3A_init_add_status.png](./screenshots/3A_init_add_status.png)
- 3B: [screenshots/3B_index_contents.png](./screenshots/3B_index_contents.png)
- 4A: [screenshots/4A_pes_log.png](./screenshots/4A_pes_log.png)
- 4B: [screenshots/4B_pes_files.png](./screenshots/4B_pes_files.png)
- 4C: [screenshots/4C_refs.png](./screenshots/4C_refs.png)
- Final integration: [screenshots/final_integration.png](./screenshots/final_integration.png)

Text captures used to generate the screenshots are also stored in the same `screenshots/` directory.

## Implementation Summary

- `object.c`: implemented object write/read, SHA-256 hashing, sharded storage, deduplication, and integrity verification.
- `tree.c`: implemented recursive tree construction from staged index entries and deterministic tree serialization flow.
  Tree entries are sorted before serialization so identical directory contents produce identical hashes even if discovered in a different order.
- `index.c`: implemented index load/save, sorted atomic writes, and blob staging.
  The index is persisted in a simple text format, sorted by path, and flushed before replacement so staged state survives process exits cleanly.
- `commit.c`: implemented commit creation using staged tree snapshots, parent lookup through `HEAD`, and branch ref updates.

## Analysis Answers

### Q5.1

`pes checkout <branch>` would need to:

1. Read `.pes/refs/heads/<branch>` to get the target commit hash.
2. Update `.pes/HEAD` so it points to `ref: refs/heads/<branch>` if we are switching to a branch.
3. Read the target commit, then its root tree, and recursively materialize that tree into the working directory.
4. Update `.pes/index` so the staged metadata matches the checked-out tree.

The hard part is the working directory update. Checkout is not just changing one pointer file; it must compare the current tracked snapshot against the target tree, create/remove directories, overwrite files whose blobs changed, preserve permissions, and refuse when uncommitted work would be lost.

### Q5.2

To detect a dirty-working-directory conflict using only the index and object store:

1. For each path tracked in the index, compare the current file metadata in the working directory against the indexed metadata (`mtime`, `size`).
2. If metadata differs, hash the current file contents and compare that hash against the blob hash stored in the index.
3. Separately, read the target branch commit, walk its tree, and determine the blob hash for that same path in the target branch.
4. If the working copy differs from the index and the target branch also wants a different blob for that path, checkout must refuse because switching would overwrite unstaged user work.

In short: detect local modifications relative to the index, then detect whether the target branch wants to touch the same tracked path.

### Q5.3

In detached `HEAD`, `HEAD` stores a commit hash directly instead of a branch name. New commits still work, but no branch moves forward with them, so the new commits become reachable only from `HEAD` for that session. If the user later checks out a branch, those detached commits can become unreachable.

Recovery is still possible if the user remembers or finds one of those commit hashes. They can create a new branch or ref pointing at the detached commit, which makes the history reachable again. Real Git often helps via the reflog; in PES-VCS a user could recover by manually creating a branch ref file containing that commit hash.

### Q6.1

A garbage collector can use a mark-and-sweep approach:

1. Start from all roots: every branch file in `.pes/refs/heads/` and possibly `HEAD` if detached.
2. Mark each reachable commit hash.
3. For every reachable commit, parse it and mark its tree and parent commit.
4. For every reachable tree, parse all entries and mark child tree/blob hashes recursively.
5. After traversal, scan `.pes/objects/` and delete any object whose hash is not in the reachable set.

The best data structure for the reachable set is a hash set keyed by the 32-byte object hash or its 64-char hex form. Membership checks need to be close to O(1).

For a repository with 100,000 commits and 50 branches, the walk visits every unique reachable commit once, plus every unique reachable tree and blob reachable from those commits. The exact count depends on repository size, but in practice it would be on the order of the total number of reachable objects, which could easily be hundreds of thousands or millions if each commit introduces new trees/blobs.

### Q6.2

Running GC concurrently with commit creation is dangerous because commit creation happens in stages:

1. Write blobs/trees/commit objects.
2. Update the branch ref so the new commit becomes reachable.

There is a race window between steps 1 and 2. During that window, the new objects exist on disk but no branch points to them yet. A concurrent GC that scans refs at exactly that moment would consider those objects unreachable and delete them. Then the commit operation could update `HEAD` to a commit whose tree or parent objects were just removed.

Real Git avoids this with extra safety mechanisms such as lock files, temporary reachability protections, conservative pruning rules, grace periods based on object age, and coordination between ref updates and garbage collection so newly written objects are not immediately collected.

## Notes

- The assignment targets Ubuntu 22.04. This report was prepared from a Windows workstation, so the saved screenshot images were generated from captured terminal output logs in `screenshots/` to preserve each required command/result pair consistently.
- All required screenshot targets from Phases 1 through 4, along with the final integration capture, are present in the repository under `screenshots/`.
