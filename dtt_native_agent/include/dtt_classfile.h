#ifndef DTT_CLASSFILE_H
#define DTT_CLASSFILE_H

#include <jvmti.h>

/*
 * dtt_classfile: a minimal, purpose-built .class file surgeon.
 *
 * This is NOT a general bytecode library. It does exactly one thing:
 * prepend a short "guard prologue" to the start of a named method's
 * Code attribute, in a way that keeps the class verifiable under
 * Java 7+ split verification (StackMapTable-aware).
 *
 * Scope / known limitations (be aware of these before trusting this
 * on anything you can't afford to crash):
 *   - Only handles methods whose Code attribute we can find directly
 *     (no nested/inner-class weirdness beyond normal method lookup).
 *   - Local variable slots for the synthesized leading stack-map frame
 *     are derived from the method descriptor + static/instance-ness
 *     only. It assumes the method's initial locals are exactly
 *     "this (if instance) + declared parameters" with nothing else
 *     live at offset 0 - true for every method we target, but NOT a
 *     safe assumption for arbitrary methods.
 *   - Array-typed locals are supported via a CONSTANT_Class entry
 *     whose name is the raw array descriptor (this matches real
 *     classfile semantics), but multi-dimensional/generic edge cases
 *     have not been exhaustively fuzzed. Test before relying on it.
 *   - If the target method already has NO StackMapTable at all
 *     (rare - only true for genuinely branch-free methods), a new
 *     one is synthesized from scratch with a single FULL_FRAME entry.
 *
 * On any doubt/unsupported shape, injection functions return a NULL
 * data pointer and the caller MUST fall back to the original,
 * untouched bytecode. Never partially patch.
 */

typedef struct {
    unsigned char *data; /* malloc'd - caller must free() after copying into a JVMTI Allocate() buffer */
    jint len;
} dtt_cf_result_t;

typedef enum {
    DTT_GUARD_CANCEL_BOOLEAN = 0, /* if guard(arg0)==true: return false (0) immediately */
    DTT_GUARD_CANCEL_VOID    = 1, /* if guard(arg0)==true: return (void) immediately */
    DTT_GUARD_ENTITY_DATA_SET = 2 /* SynchedEntityData.set(accessor,value) family:
                                      call helper(this,arg1,arg2) -> Object, store result
                                      back into local slot 2, then fall through to original
                                      code (never early-returns). */
} dtt_guard_style_t;

/*
 * Attempts to inject a guard prologue into method_name/method_desc inside
 * the given class bytes.
 *
 * guard_owner/guard_name/guard_desc describe the static helper method to
 * call, e.g. owner="net/mcreator/transfinityimproved/coremod/PreatorGodHelper",
 * name="isGod", desc="(Lnet/minecraft/world/entity/LivingEntity;)Z".
 *
 * For CANCEL_BOOLEAN/CANCEL_VOID, the guard is called with ALOAD 0 (i.e.
 * "this") as its sole argument - correct for isGod(LivingEntity)/
 * isGodEntity(Entity) style checks on instance methods.
 *
 * For ENTITY_DATA_SET, guard_desc must accept exactly the method's own
 * three arguments in order (this, arg1, arg2) and return Ljava/lang/Object;
 * matching PreatorGodHelper.interceptEntityDataSet's shape.
 *
 * Returns {NULL,0} if the method wasn't found, already looks patched
 * (idempotency guard against double-injection on re-load), or the class
 * shape wasn't something this simple patcher understands.
 */
dtt_cf_result_t dtt_cf_inject_guard(const unsigned char *class_data, jint class_data_len,
                                     const char *method_name, const char *method_desc,
                                     const char *guard_owner, const char *guard_name,
                                     const char *guard_desc, dtt_guard_style_t style);

void dtt_cf_result_free(dtt_cf_result_t *result);

#endif /* DTT_CLASSFILE_H */
