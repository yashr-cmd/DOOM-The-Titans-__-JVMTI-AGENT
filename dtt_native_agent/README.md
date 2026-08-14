# DTT Native Integrity Agent

A pure-C JVMTI native agent that watches for anything trying to touch
**Doom & The Titans' (Transfinity Improved) and Chaos Mobs'** own
protected classes, and then **absorbs or reflects the offending thread** -
never killing, suspending, stopping, or interrupting any thread. It never
inspects, scores, or interferes with anything outside those two mods'
packages and its own companion `runtime.*` package.

## What it does

Single job: watch DTT's own classes for tampering, and when a thread is
caught touching them without authorization, absorb it (silently neutralize
every attempt it makes) or reflect it (bounce a repeat offender's attack
back onto its own thread). Everything else - other mods' classes, other
mods' threads, other agents - keeps running untouched.

- Standard JVMTI agent lifecycle: `Agent_OnLoad` (`-agentpath:`),
  `Agent_OnAttach` (dynamic attach, matching `AttachHelper.java`), and
  `Agent_OnUnload`.
- Requests three JVMTI capabilities, each negotiated and degraded
  independently: `can_generate_all_class_hook_events` (catches tampering
  that goes through official `RedefineClasses`/`RetransformClasses`),
  `can_get_bytecodes` and `can_redefine_classes` (power the live-bytecode
  watchdog below, which catches tampering that does *not* go through
  those APIs). Events used: `VMInit`, `VMDeath`, `ClassPrepare`,
  `ClassFileLoadHook`.
- Watches class loads/redefinitions for classes under:
  - `net/mcreator/transfinityimproved/`
  - `net/mcreator/chaosmobs/`
  - `runtime/` (the Java runtime agent itself, so it can't be neutered first)
- **No score system.** There is no integrity score, no status tier, no
  reason-code ring buffer. There is only thread-level action:

  - **Detection** - three independent layers spot "something trying to
    touch my mod":
    1. `ClassFileLoadHook` fires on every official
       `RedefineClasses`/`RetransformClasses` of a protected class.
    2. `ClassPrepare` + a loader-identity table spot *shadow class* loads
       (the same protected class name defined by a second ClassLoader).
    3. A background **live bytecode integrity watchdog** (a proper JVMTI
       agent thread via `RunAgentThread`) periodically re-reads the
       *current* bytecode of every protected class's methods and compares
       it against a known-good hash, catching tampering that bypasses
       redefinition entirely (e.g. a hostile mod using `sun.misc.Unsafe`
       to overwrite a method's compiled bytecode in the JVM's internal
       structures).

  - **Absorb the thread** - the first time an unauthorized thread touches
    a protected class, it is flagged by identity (`java.lang.Thread`
    object). From then on *every* attempt that thread makes against the
    mod is silently neutralized: the tampered bytecode is replaced with
    the cached known-good version via the hook's `new_class_data` output.
    The hostile thread keeps running normally, unaware its payload was
    absorbed. Shadow-class loads are absorbed the same way, substituting
    known-good bytecode for the smuggled version before it is defined.

  - **Reflect the thread** - if the *same* thread keeps hitting the *same*
    protected class (default: `reflectafter=2` offenses), the response
    escalates: the hook hands the JVM deliberately-invalid class data, so
    the hostile redefinition call fails with a `ClassFormatError` **thrown
    on the offending thread itself** - its own attack bounced back at it.

  - **Never kill.** At no point does this agent call suspend, stop,
    interrupt, or terminate on any thread (DTT's own or anyone else's).
    A reflected thread simply experiences its own failed redefinition call;
    whether its code catches that exception is entirely up to it.

- The watchdog "reflects" live bytecode drift the same way: it pushes the
  cached known-good bytecode back in via `RedefineClasses` when it has the
  capability and a baseline; otherwise it reports the drift and keeps
  running.
- The DTT Java runtime agent can mark its **own** legitimate patches as
  authorized (`NativeIntegrityBridge.runAuthorized(...)`) so they're never
  mistaken for an attack and never flag DTT's own threads; the watchdog
  does the same internally for its own repairs.
- **Hard scope guarantee:** this agent never inspects, reads bytecode of,
  or modifies any class outside the three prefixes above. It never
  suspends, stops, interrupts, or otherwise touches *any* thread. The mere
  presence of another JVMTI agent, Java agent, or transformation service
  is never treated as hostile by itself - only concrete behavior against
  DTT's own protected classes is, and the response is always absorb-then-
  reflect on the offending thread, never interference with the offending
  mod's classes or threads.
- Every failure path (missing capability, failed callback registration,
  failed allocation, etc.) disables just that piece of functionality and
  logs a warning. `Agent_OnLoad`/`Agent_OnAttach` **always** return
  `JNI_OK` - this agent can never prevent Minecraft from launching.

## Layout

```
include/            Public headers, one per module
  dtt_platform.h     OS/arch detection, mutex/TLS/clock shims
  dtt_config.h       All tunable constants (reflect threshold, sizes)
  dtt_log.h          Tiny leveled logger
  dtt_protected.h    The DTT-owned package whitelist
  dtt_auth.h         Thread-local "authorized transform" flag
  dtt_cache.h        Known-good bytecode cache
  dtt_threads.h      Hostile-thread registry: absorb/reflect actions
  dtt_callbacks.h    JVMTI event handlers
  dtt_bridge.h       JNI native methods exposed to Java
  dtt_agent.h        Shared accessors (is_active, platform string, capabilities)
  dtt_watchdog.h     Live bytecode integrity watchdog (out-of-band tamper detection)
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
-agentpath:/path/to/dtt_agent.dll=reflectafter=2,scaninterval=4000,loglevel=info
```

or dynamically via the Attach API from `AttachHelper.java`, calling
`VirtualMachine#loadAgentPath` with the same options string format.

Options (all optional):

- `reflectafter=N` - how many times the same thread may hit the same
  protected class before the response escalates from absorb to reflect
  (default 2).
- `scaninterval=MS` - live-bytecode watchdog poll interval (default 4000,
  minimum 500).
- `loglevel=error|warn|info|debug` - logging verbosity (default info).

## Wiring up the Java side

1. Copy `java/runtime/NativeIntegrityBridge.java` into
   `runtime-agent-NoNative`'s `runtime` package.
2. Early in `RuntimeAgent`'s startup, resolve the correct native library
   for the current OS/arch and call `NativeIntegrityBridge.tryLoad(path)`.
   If it returns `false`, just keep running with Java-only protections -
   don't treat it as fatal.
3. Wrap any of `RuntimePatch`'s own legitimate redefinitions of DTT
   classes with `NativeIntegrityBridge.runAuthorized("net/mcreator/...",
   () -> { /* your existing redefine call */ })` so they're never mistaken
   for tampering and never flag DTT's own threads.
4. Poll `getAbsorbedThreadCount()` / `getReflectedThreadCount()` from
   wherever `HostileRegistry`/`TrustChecker` already aggregate signals,
   the same way you already combine other integrity signals today.

## Notes on scope

`dtt_protected.c` is the single source of truth for "is this even our
business" - every other module routes through `dtt_is_protected_class()`.
If you add new packages to either mod, add the prefix there and nothing
else needs to change. Classes outside those prefixes (any other mod, any
other agent, Forge/NeoForge/Fabric internals, the JDK itself) are never
inspected, absorbed, reflected, or touched by this agent.
