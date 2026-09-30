# ReXGlue patches

Fixes to [ReXGlue](https://github.com/rexglue/rexglue-sdk) v0.10.0 (`c94f5eb`) that this port
needs. Apply them to the SDK source before building it (see [BUILDING.md](../../BUILDING.md)):

```
cd rexglue-sdk
git apply <path-to-this-repo>/patches/rexglue/0001-input-system-lock.patch
git apply <path-to-this-repo>/patches/rexglue/0002-codegen-vpk-unsigned-alias.patch
```

| Patch | Symptom without it | Cause |
|---|---|---|
| `0001-input-system-lock.patch` | Game hangs on the loading screen after pressing Start (Debug builds show "vector iterators incompatible" in `rexruntimed.dll`). | `InputSystem` has no locking; the game polls input from two threads while `RefreshDevices()` rebuilds the device list. Adds a mutex. |
| `0002-codegen-vpk-unsigned-alias.patch` | Intro movies show green/orange striped corruption. | Codegen for `vpkuwus`/`vpkuhus` writes the destination element by element, corrupting the result when it is also a source register (as in the VP6 decoder). Builds into a temporary instead. Rerun codegen after applying. |

Upstream: the movie bug is [rexglue-sdk#364](https://github.com/rexglue/rexglue-sdk/issues/364);
[rexglue-sdk#426](https://github.com/rexglue/rexglue-sdk/pull/426) includes an equivalent fix.
Remove a patch once a ReXGlue release contains its fix.
