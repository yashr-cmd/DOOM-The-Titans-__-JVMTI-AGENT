#ifndef DTT_VANILLA_GUARD_H
#define DTT_VANILLA_GUARD_H

#include <jvmti.h>
#include "dtt_classfile.h"

/*
 * dtt_vanilla_guard: intercepts vanilla MC class loading to inject
 * damage/death/remove guard prologues into entity classes.
 *
 * This is the "Layer 1" structural defense: instead of hooking a single
 * caller or a single damage path, we patch the SHARED CHOKE POINTS
 * (LivingEntity.hurt, .die, .kill, Entity.remove, SynchedEntityData.set,
 * etc.) so that ANY caller - including code from other mods that we've
 * never seen - must pass through our guard before the operation takes
 * effect.
 *
 * The guard prologue calls a static Java helper (PreatorGodHelper) that
 * returns true if the entity is protected. If so, the operation is
 * cancelled (for hurt/die/kill/remove) or the value is intercepted
 * (for SynchedEntityData.set).
 *
 * Usage: called from dtt_on_class_file_load_hook when the loading class
 * is NOT one of our own protected mod classes. If the class matches a
 * vanilla guard target, we inject the prologue into the class bytes
 * before the JVM sees them.
 *
 * IMPORTANT: The guard method (e.g. PreatorGodHelper.isGod) must be
 * loaded before the vanilla classes it protects. In a Forge coremod
 * context this is typically satisfied because coremod classes load
 * before Minecraft classes. If this assumption is wrong, INVOKESTATIC
 * resolution will fail at class verification time and crash the JVM.
 */

/*
 * Guard entry: defines a single vanilla method to patch.
 *
 * class_name / method_name / method_desc identify the target method.
 *
 * style determines the prologue shape:
 *   CANCEL_BOOLEAN - for methods returning boolean (e.g. hurt)
 *   CANCEL_VOID    - for methods returning void (e.g. die, kill, remove)
 *   ENTITY_DATA_SET - for SynchedEntityData.set (intercepts the value)
 *
 * guard_owner / guard_name / guard_desc describe the static Java method
 * called by the prologue. For CANCEL_BOOLEAN/CANCEL_VOID, the guard
 * receives ALOAD 0 (this) and returns boolean. For ENTITY_DATA_SET,
 * the guard receives (this, arg1, arg2) and returns Object.
 */
typedef struct {
    const char *class_name;
    const char *method_name;
    const char *method_desc;
    dtt_guard_style_t style;
    const char *guard_owner;
    const char *guard_name;
    const char *guard_desc;
} dtt_vanilla_guard_entry_t;

/*
 * Initializes the vanilla guard table. Call once during agent init.
 */
void dtt_vanilla_guard_init(void);

/*
 * Attempts to inject guard prologues into the given class bytes.
 *
 * Returns {NULL,0} if class_name doesn't match any guard target, or
 * if injection failed. Returns patched class data if successful.
 *
 * The caller must copy the result into a JVMTI Allocate()-d buffer
 * and free the original via dtt_cf_result_free().
 */
dtt_cf_result_t dtt_vanilla_guard_intercept(const char *class_name,
                                             const unsigned char *class_data,
                                             jint class_data_len);

#endif /* DTT_VANILLA_GUARD_H */
