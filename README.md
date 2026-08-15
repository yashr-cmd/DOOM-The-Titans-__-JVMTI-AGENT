**DTT Native Integrity Agent (dtt_agent)**

A pure-C JVMTI native agent that protects the DOOM The Titans mod and its companion Java runtime agent from in-process tampering by hostile mods.

Loaded at JVM boot via -agentpath/-javaagent on a freshly spawned child process — before ModLauncher, mod discovery, or any third-party coremod runs — the agent watches only its own classes 
(net.mcreator.transfinityimproved.*, net.mcreator.chaosmobs.*, runtime.*) through two independent detection layers: a ClassFileLoadHook that catches tampering via the official class-redefinition APIs, and a 
background bytecode-integrity watchdog that periodically re-reads live method bytecode to catch out-of-band tampering (e.g. direct memory patching via sun.misc.Unsafe, as used by known hostile mods).

Rather than maintaining an accumulating risk score, the agent is purely reactive: it sits idle until a tamper attempt against a protected class is actually observed, at which point it immediately restores the class's 
known-good bytecode where possible and reports the incident to the Java-side runtime agent. It never inspects, blocks, or interferes with any class or thread outside its own three protected packages, and the mere 
presence of another agent or transformer is never itself treated as suspicious. Cross-platform (Windows/Linux/macOS, x64/arm64), fail-safe by design — a missing capability or failed initialization disables just that 
feature and lets Minecraft continue rather than blocking JVM startup.

By "Hostile mods" we mean mods which intentionally try to harm other mods classes, threads and techniques (eg:Pig2, some versions of Kanade's kill & madness entities)
