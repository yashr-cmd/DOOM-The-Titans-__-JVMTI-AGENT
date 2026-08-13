#ifndef DTT_PROTECTED_H
#define DTT_PROTECTED_H

/* ============================================================================
 * dtt_protected.h
 *
 * Defines the ONLY set of classes this agent is allowed to care about.
 * Everything else in the JVM - other mods, other Forge/NeoForge/Fabric
 * services, unrelated Java agents, the game engine itself - is
 * completely invisible to the scoring/blocking logic. This file is the
 * single source of truth for "is this even our business", and every
 * other module must route its "should I look at this class" decision
 * through dtt_is_protected_class() rather than re-implementing prefix
 * checks locally.
 *
 * The prefixes below mirror the real package layout of the two mods
 * this agent ships with, plus the companion Java runtime agent:
 *
 *   net/mcreator/transfinityimproved/   Transfinity Improved ("DOOM & The
 *                                       Titans") - coremod bootstrap/
 *                                       transform layer, Praetor armor
 *                                       lock system, procedures, etc.
 *   net/mcreator/chaosmobs/             Chaos Mobs - sibling project,
 *                                       TOAA sky overlay coremod,
 *                                       ChaosImmortalityHelper, etc.
 *   runtime/                            The Java-side "runtime-agent-
 *                                       NoNative" companion agent this
 *                                       native layer reports to
 *                                       (RuntimeAgent, ArmorLockGuard,
 *                                       GodHelper, RuntimePatch,
 *                                       HostileRegistry, TrustChecker...).
 *                                       Protecting it too stops a
 *                                       hostile actor from disabling the
 *                                       watchdog before going after the
 *                                       mod classes themselves.
 * ============================================================================ */

/* Returns 1 if the given JVM-internal class name (slash-separated, no
 * leading 'L' or trailing ';', e.g.
 * "net/mcreator/transfinityimproved/coremod/TIBootstrapService") falls
 * under a package this agent protects. Returns 0 for absolutely
 * everything else, including NULL input.
 *
 * IMPORTANT: this function only ever returns 1 for classes that belong
 * to *this* project. It is never used to decide whether some other
 * mod/class is "hostile" - only whether a given class is one of OURS
 * that deserves tamper-detection. */
int dtt_is_protected_class(const char *jvm_class_name);

/* Strips a JVMTI class *signature* ("Lpkg/pkg/Class;") down to the
 * plain internal name ("pkg/pkg/Class") so it can be passed to
 * dtt_is_protected_class(). Writes into out_buf (size out_buf_len) and
 * always NUL-terminates. Safe on non-object signatures (arrays,
 * primitives) - simply copies them through unmodified since they will
 * never match a protected prefix anyway. */
void dtt_strip_class_signature(const char *signature, char *out_buf, int out_buf_len);

#endif /* DTT_PROTECTED_H */
