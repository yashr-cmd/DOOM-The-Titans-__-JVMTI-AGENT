#ifndef DTT_CONFIG_H
#define DTT_CONFIG_H

/* ============================================================================
 * dtt_config.h - all "policy knobs" for the integrity agent live here so
 * scoring behavior can be tuned without hunting through the logic files.
 * Every value below can also be overridden at runtime (agent options
 * string at load time, or the JNI bridge's configureThresholds() call
 * from the Java-side DTT runtime agent).
 * ============================================================================ */

/* Score tiers. A score >= a tier's threshold and < the next tier's
 * threshold is reported as that tier's status. */
#define DTT_DEFAULT_THRESHOLD_SUSPICIOUS 20   /* NORMAL     -> SUSPICIOUS */
#define DTT_DEFAULT_THRESHOLD_HIGH_RISK  50   /* SUSPICIOUS -> HIGH_RISK  */
#define DTT_DEFAULT_THRESHOLD_CRITICAL   80   /* HIGH_RISK  -> CRITICAL   */
#define DTT_SCORE_MIN                    0
#define DTT_SCORE_MAX                    1000

/* Point values for specific observed behaviors. Kept as named constants
 * so the policy is legible and easy to retune in one place. */

/* An external actor attempted to redefine/retransform bytecode of a
 * protected DTT class without going through the authorized-transform
 * window that the Java-side DTT runtime agent opens for its own
 * legitimate patches. */
#define DTT_POINTS_UNAUTHORIZED_REDEFINE        25

/* We successfully substituted our own cached, known-good bytecode in
 * place of the tampered class, so the *outcome* is contained - but we
 * still log it and still count the attempt above. This constant is 0
 * on purpose: the risk was already scored by DTT_POINTS_UNAUTHORIZED_REDEFINE,
 * this just documents that a successful block does not add *extra*
 * penalty beyond the detection itself. */
#define DTT_POINTS_TAMPER_BLOCKED_OK             0

/* We detected tampering but could NOT safely substitute original
 * bytecode (e.g. no capability, no cached copy, or an internal error
 * during Allocate). Extra points reflect the additional uncertainty of
 * an attempt that got through. We still never crash the JVM for this -
 * see dtt_callbacks.c. */
#define DTT_POINTS_REDEFINE_COULD_NOT_BLOCK     15

/* A class with the exact same fully-qualified name as one of our
 * protected classes was observed being prepared by a different
 * ClassLoader instance than the one that first loaded it - a classic
 * "shadow class" pattern used to smuggle in a fake replacement. */
#define DTT_POINTS_SHADOW_CLASSLOADER            10

/* Consecutive-offense escalation: repeated tampering attempts against
 * the same protected class in a short window are treated as more
 * deliberate than an isolated event, so we add a small, capped bonus
 * per consecutive hit (see dtt_score.c for the exact accrual logic). */
#define DTT_REPEAT_OFFENSE_BONUS_PER_HIT          2
#define DTT_REPEAT_OFFENSE_BONUS_CAP              20

/* How many reason-code entries the ring buffer keeps. Oldest entries
 * are overwritten once full - this is diagnostic history, not an
 * audit log requiring unbounded retention. */
#define DTT_REASON_LOG_CAPACITY 128
#define DTT_REASON_LINE_MAX     256

/* Bytecode cache: how many distinct protected classes we keep a
 * "known good" copy of at once. Generous headroom above the actual
 * class count of Transfinity Improved + Chaos Mobs + the runtime
 * agent combined. */
#define DTT_CACHE_CAPACITY 512
#define DTT_CACHE_NAME_MAX 512

/* Refuse to cache (or restore) anything absurdly large - protects
 * against a hostile actor trying to exhaust native heap by feeding
 * huge class files through the hook. A real .class file for this
 * project is at most a few hundred KB. */
#define DTT_CACHE_MAX_CLASS_BYTES (8 * 1024 * 1024)

#endif /* DTT_CONFIG_H */
