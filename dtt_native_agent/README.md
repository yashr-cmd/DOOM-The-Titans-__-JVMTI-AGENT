# DTT Native Integrity Agent

A pure-C JVMTI native agent that watches for tampering with **Doom & The
Titans' (Transfinity Improved) and Chaos Mobs'** own protected classes, and
reports what it sees to the existing Java-side `runtime-agent-NoNative`
project. It never inspects, scores, or interferes with anything outside
those two mods' packages and its own companion `runtime.*` package.

## What it does

- Standard JVMTI agent lifecycle: `Agent_OnLoad` (`-agentpath:`),
  `Agent_OnAttach` (dynamic attach, matching `AttachHelper.java`), and
  `Agent_OnUnload`.
- Requests exactly one JVMTI capability (`can_generate_all_class_hook_events`)
  and only the events it needs (`VMInit`, `VMDeath`, `ClassPrepare`,
  `ClassFileLoadHook`).
- Watches class loads/redefinitions for classes under:
  - `net/mcreator/transfinityimproved/`
  - `net/mcreator/chaosmobs/`
  - `runtime/` (the Java runtime agent itself, so it can't be neutered first)
- Maintains a configurable integrity score with named reason codes
  (`DTT-000` info, `DTT-101`/`DTT-102`/`DTT-103` tamper events, `DTT-201`
  shadow-classloader events).
- Four status tiers: `NORMAL` / `SUSPICIOUS` / `HIGH_RISK` / `CRITICAL`,
  with configurable thresholds (agent options at load time, or
  `NativeIntegrityBridge.configureThresholds(...)` at runtime).
- On an **unauthorized** redefinition of a protected class, it tries to
  restore the last known-good bytecode via the `ClassFileLoadHook`
  `new_class_data` mechanism - isolating the tampering without throwing,
  crashing, or aborting anything. If it can't do that safely, it just
  scores the event and lets the JVM continue.
- The DTT Java runtime agent can mark its **own** legitimate patches as
  authorized (`NativeIntegrityBridge.runAuthorized(...)`) so they're never
  mistaken for an attack.
- The mere presence of another JVMTI agent, Java agent, or transformation
  service is never treated as suspicious by itself - only concrete
  behavior against DTT's own protected classes moves the score.
- Every failure path (missing capability, failed callback registration,
  failed allocation, etc.) disables just that piece of functionality and
  logs a warning. `Agent_OnLoad`/`Agent_OnAttach` **always** return
  `JNI_OK` - this agent can never prevent Minecraft from launching.

## Layout

```
include/            Public headers, one per module
  dtt_platform.h     OS/arch detection, mutex/TLS/clock shims
  dtt_config.h       All tunable constants (thresholds, point values, sizes)
  dtt_log.h          Tiny leveled logger
  dtt_protected.h    The DTT-owned package whitelist
  dtt_auth.h         Thread-local "authorized transform" flag
  dtt_cache.h        Known-good bytecode cache
  dtt_score.h        Integrity score + reason-code ring buffer
  dtt_callbacks.h    JVMTI event handlers
  dtt_bridge.h       JNI native methods exposed to Java
  dtt_agent.h        Shared accessors (is_active, platform string)
src/                 One .c per header above, plus:
  dtt_agent.c         Agent_OnLoad / Agent_OnAttach / Agent_OnUnload
java/runtime/
  NativeIntegrityBridge.java   Java-side native method declarations
CMakeLists.txt        Cross-platform build script
```

## Building

Requires a JDK (for `jni.h`/`jvmti.h`) and CMake 3.10+. No C++ or Rust
toolchain needed anywhere.

```bash
mkdir build && cd build
cmake .. -DJAVA_HOME="$JAVA_HOME"
cmake --build . --config Release
```

Produces, per platform:

| OS      | Output              |
|---------|---------------------|
| Windows | `dtt_agent.dll`     |
| Linux   | `libdtt_agent.so`   |
| macOS   | `libdtt_agent.dylib`|

(No compiled binaries are included in this delivery - source only, per
request. Run the build yourself on each target platform.)

## Loading it

Either at JVM launch:

```
-agentpath:/path/to/dtt_agent.dll=suspicious=20,highrisk=50,critical=80,loglevel=info
```

or dynamically via the Attach API from `AttachHelper.java`, calling
`VirtualMachine#loadAgentPath` with the same options string format.

## Wiring up the Java side

1. Copy `java/runtime/NativeIntegrityBridge.java` into
   `runtime-agent-NoNative`'s `runtime` package.
2. Early in `RuntimeAgent`'s startup, resolve the correct native library
   for the current OS/arch and call `NativeIntegrityBridge.tryLoad(path)`.
   If it returns `false`, just keep running with Java-only protections -
   don't treat it as fatal.
3. Wrap any of `RuntimePatch`'s own legitimate redefinitions of DTT
   classes with `NativeIntegrityBridge.runAuthorized("net/mcreator/...",
   () -> { /* your existing redefine call */ })` so they're never scored
   as tampering.
4. Poll `getIntegrityScore()` / `getIntegrityStatus()` / `getReasonCodes(n)`
   from wherever `HostileRegistry`/`TrustChecker` already aggregate signals,
   the same way you already combine other integrity signals today.

## Notes on scope

`dtt_protected.c` is the single source of truth for "is this even our
business" - every other module routes through `dtt_is_protected_class()`.
If you add new packages to either mod, add the prefix there and nothing
else needs to change. Classes outside those prefixes (any other mod,
any other agent, Forge/NeoForge/Fabric internals, the JDK itself) are
never inspected, scored, or touched by this agent.
