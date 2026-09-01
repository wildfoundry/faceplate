# Faceplate status sources

Faceplate is a read-only consumer. It never opens a publisher socket, loads publisher code,
executes a source, or writes a response. Sources are registered by root-owned `*.source`
files in `/usr/lib/faceplate/sources.d` and publish bounded snapshots to their own runtime
directories below `/run`.

Each declaration contains exactly:

```ini
kind=dataplicity
path=/run/dataplicity/faceplate.status
user=dpagent
stale_after_ms=30000
```

The declaration must be a regular, root-owned, non-writable file. The snapshot must be a
regular file owned by the declared account, have one link, fit the schema size limit, and
must not be group/world writable. Paths outside `/run` are rejected. Publishers should
verify `tmpfs` with `fstatfs`, write to a same-directory temporary file using `O_NOFOLLOW`,
then atomically rename it into place.

Kinds and fields are allowlisted in Faceplate. Unknown kinds, keys, values, malformed
timestamps and unsafe files are ignored. Missing means not applicable; stale means the
source is displayed as `Unknown`.

Faceplate ships the isolated `system` collector. Dataplicity is an external publisher using
the same contract. Additional integrations require a new allowlisted schema kind rather
than arbitrary labels or remotely supplied display text.
