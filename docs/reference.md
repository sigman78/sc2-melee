# The C reference

This codebase is a port, and it cites its source. Comments carry references
like `process.c:361-627`, `tactrans.c:757-770`, `ilwrath.c:141-148` — file and
line into the original Ur-Quan Masters C tree.

**Line numbers only mean something against a fixed revision.** They resolve
against:

| | |
| --- | --- |
| Repository | https://github.com/sigman78/sc2-uqm (fork of https://git.code.sf.net/p/sc2/uqm) |
| Revision | `d86ae1e3ec64a86054a767f7f71f2f05d70215e0` |
| Path | `sc2/src/uqm/` for gameplay, `sc2/src/libs/` for engine support |

Keep that tree checked out beside this one. The build's `UQM2_CONTENT_DIR`
already defaults to `../sc2-uqm/sc2/content`, so a sibling checkout serves
both purposes: content for the tests that need it, and the C for reading.

	cd ..
	git clone https://github.com/sigman78/sc2-uqm.git

If you update that checkout past the revision above, citations may drift.
Re-pin this file when you do.

## Commit hashes in the review log

`src/docs/review-001..009` cite commit hashes from before the extraction
(`a5aaad8f0`, `c0b98eebe`, and so on). Those hashes do **not** exist in this
repository — extracting the history rewrote every commit id. They resolve in
`sc2-uqm`, which keeps the full pre-extraction history and remains the
archive for it.

The rewrite's own history is here in full, back to its first commit; only the
identifiers changed.
