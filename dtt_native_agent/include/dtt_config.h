#ifndef DTT_CONFIG_H
#define DTT_CONFIG_H

/* ============================================================================
 * dtt_config.h - all "policy knobs" for the integrity agent live here so
 * the absorb/reflect behavior can be tuned without hunting through the
 * logic files. There is no score system anymore: the only policy
 * questions left are "how many times may the same thread hit the same
 * protected class before we stop absorbing and start reflecting" and the
 * watchdog's poll cadence.
 * ============================================================================ */

/* ----------------------------------------------------------------------
 * Absorb / reflect threshold.
 *
 * The first time an unauthorized thread touches a protected DTT class it
 * is flagged ("absorbed"): every attempt it makes is silently replaced
 * with known-good bytecode and the thread keeps running normally. If the
 * SAME thread keeps hitting the SAME protected class at least
 * DTT_REFLECT_AFTER_HITS times, the response escalates to "reflect": the
 * hostile redefinition call is bounced back onto the offending thread
 * itself (it fails with a ClassFormatError on that thread). Threads are
 * never killed, suspended, stopped, or interrupted at any point.
 */
#define DTT_REFLECT_AFTER_HITS 2

/* How many distinct hostile threads we track at once, and how many
 * distinct protected classes we remember per hostile thread. Both are
 * capped so a hostile actor can never exhaust native heap by spamming
 * new threads/classes at the agent. When the thread table is full, new
 * offenders are still absorbed per-event, just not remembered by
 * identity. */
#define DTT_THREAD_TRACK_CAPACITY  128
#define DTT_THREAD_CLASS_OFFENSES  8
#define DTT_THREAD_NAME_MAX        64

/* ----------------------------------------------------------------------
 * Known-good bytecode cache.
 * --------------------------------------------------------------------- */
#define DTT_CACHE_CAPACITY 512
#define DTT_CACHE_NAME_MAX 512

/* Refuse to cache (or restore) anything absurdly large - protects
 * against a hostile actor trying to exhaust native heap by feeding
 * huge class files through the hook. A real .class file for this
 * project is at most a few hundred KB. */
#define DTT_CACHE_MAX_CLASS_BYTES (8 * 1024 * 1024)

/* ----------------------------------------------------------------------
 * Live bytecode integrity watchdog.
 *
 * ClassFileLoadHook only fires for tampering that goes through the
 * official JVMTI RedefineClasses/RetransformClasses path. Some hostile
 * mods instead use sun.misc.Unsafe to overwrite a method's bytecode
 * directly in the JVM's internal Method/ConstMethod structures,
 * completely bypassing that hook. The watchdog periodically re-reads
 * the live bytecode of every tracked method belonging to a protected
 * class and compares it against a known-good hash, catching this class
 * of attack regardless of the mechanism used. On drift it reflects the
 * tampering away by pushing the known-good bytecode back in via
 * RedefineClasses - the same isolate-don't-crash philosophy as the
 * absorb path, reached through a different API.
 */
#define DTT_WATCHDOG_DEFAULT_INTERVAL_MS 4000
#define DTT_WATCHDOG_MIN_INTERVAL_MS      500
#define DTT_METHOD_TRACK_CAPACITY        2048

#endif /* DTT_CONFIG_H */
