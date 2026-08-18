#include "../include/dtt_vanilla_guard.h"
#include "../include/dtt_protected.h"
#include "../include/dtt_log.h"

#include <string.h>

/*
 * Guard target table.
 *
 * Each entry defines one vanilla method to patch. The classfile patcher
 * (dtt_cf_inject_guard) prepends a guard prologue that calls a static
 * Java method. If that method returns true, the vanilla operation is
 * cancelled (hurt/die/kill/remove) or the value is intercepted
 * (SynchedEntityData.set).
 *
 * The guard_owner must be your mod's guard class. The guard methods
 * must exist BEFORE any vanilla entity class is loaded (coremod load
 * order guarantees this in Forge).
 *
 * VERIFICATION: You MUST check these method descriptors against YOUR
 * MC version's mappings (MCP/Yarn/Mojang). The descriptors below are
 * representative of MC 1.20.1 Forge with Mojang mappings. If your
 * mappings differ, update them - a wrong descriptor means the patcher
 * silently skips the method (returns NULL) and the entity remains
 * unguarded.
 *
 * Format note on LivingEntity.actuallyHurt:
 *   In some 1.20.x builds the signature is (DamageSource, float)V.
 *   In others it is (ServerPlayer, DamageSource, float)V.
 *   Check your actual decompiled source and update accordingly.
 */

/* ---- guard method references (your Java-side guard class) ---- */

#define GUARD_OWNER "net/mcreator/transfinityimproved/coremod/PreatorGodHelper"

/* isGod(LivingEntity)Z - returns true if the entity is protected */
#define GUARD_NAME_LIVING   "isGod"
#define GUARD_DESC_LIVING   "(Lnet/minecraft/world/entity/LivingEntity;)Z"

/* isGodEntity(Entity)Z - returns true if the entity is protected (Entity supertype) */
#define GUARD_NAME_ENTITY   "isGodEntity"
#define GUARD_DESC_ENTITY   "(Lnet/minecraft/world/entity/Entity;)Z"

/* interceptEntityDataSet(SynchedEntityData, EntityDataAccessor, Object)Object
 * Intercepts SynchedEntityData.set calls. If the accessor targets health
 * on a protected entity, returns the clamped/capped value instead.
 * For non-health accessors or unprotected entities, returns the original
 * value unchanged. */
#define GUARD_NAME_SETDATA  "interceptEntityDataSet"
#define GUARD_DESC_SETDATA  "(Lnet/minecraft/network/syncher/SynchedEntityData;Lnet/minecraft/network/syncher/EntityDataAccessor;Ljava/lang/Object;)Ljava/lang/Object;"

/* ---- the guard table ---- */

static const dtt_vanilla_guard_entry_t G_GUARD_TABLE[] = {
    /*
     * LivingEntity damage pipeline.
     *
     * hurt() is the main entry point for all damage in MC. Any code that
     * wants to damage an entity calls hurt(). Patching this catches
     * everything: mobs, players, commands, other mods' damage calls.
     *
     * actuallyHurt() is the internal method that actually reduces HP.
     * Patching BOTH is belt-and-suspenders: hurt() catches callers,
     * actuallyHurt() catches anything that somehow bypasses hurt().
     *
     * die() fires when health reaches zero. Patching this prevents the
     * death animation/state transition even if damage somehow got through.
     *
     * kill() is the instant-kill command path (used by /kill and similar).
     */

    /* LivingEntity.hurt(DamageSource, float)Z */
    {
        "net/minecraft/world/entity/LivingEntity",
        "hurt",
        "(Lnet/minecraft/world/damagesource/DamageSource;F)Z",
        DTT_GUARD_CANCEL_BOOLEAN,
        GUARD_OWNER,
        GUARD_NAME_LIVING,
        GUARD_DESC_LIVING
    },

    /* LivingEntity.actuallyHurt(DamageSource, float)V
     * NOTE: verify signature against your MC version's mappings.
     * Some builds: (ServerPlayer, DamageSource, float)V */
    {
        "net/minecraft/world/entity/LivingEntity",
        "actuallyHurt",
        "(Lnet/minecraft/world/damagesource/DamageSource;F)V",
        DTT_GUARD_CANCEL_VOID,
        GUARD_OWNER,
        GUARD_NAME_LIVING,
        GUARD_DESC_LIVING
    },

    /* LivingEntity.die(DamageSource)V */
    {
        "net/minecraft/world/entity/LivingEntity",
        "die",
        "(Lnet/minecraft/world/damagesource/DamageSource)V",
        DTT_GUARD_CANCEL_VOID,
        GUARD_OWNER,
        GUARD_NAME_LIVING,
        GUARD_DESC_LIVING
    },

    /* LivingEntity.kill()V */
    {
        "net/minecraft/world/entity/LivingEntity",
        "kill",
        "()V",
        DTT_GUARD_CANCEL_VOID,
        GUARD_OWNER,
        GUARD_NAME_LIVING,
        GUARD_DESC_LIVING
    },

    /*
     * ServerPlayer death override.
     *
     * ServerPlayer.die() overrides LivingEntity.die(). We patch both
     * because the JVM resolves the call site against the declared type.
     * If a caller has a ServerPlayer reference, it dispatches to
     * ServerPlayer.die(), not LivingEntity.die(). Both must be guarded.
     */

    /* ServerPlayer.die(DamageSource)V */
    {
        "net/minecraft/server/level/ServerPlayer",
        "die",
        "(Lnet/minecraft/world/damagesource/DamageSource)V",
        DTT_GUARD_CANCEL_VOID,
        GUARD_OWNER,
        GUARD_NAME_LIVING,
        GUARD_DESC_LIVING
    },

    /*
     * Entity removal / discard.
     *
     * Some hostile code paths remove or discard entities rather than
     * damaging them. Patching Entity.remove() and Entity.discard()
     * prevents those too.
     *
     * NOTE: Entity.discard() is a Mojang-mapping name. In MCP it may
     * be called differently (e.g. setRemoved or discard). Check your
     * mappings.
     */

    /* Entity.remove(Entity.RemovalReason)V */
    {
        "net/minecraft/world/entity/Entity",
        "remove",
        "(Lnet/minecraft/world/entity/Entity$RemovalReason;)V",
        DTT_GUARD_CANCEL_VOID,
        GUARD_OWNER,
        GUARD_NAME_ENTITY,
        GUARD_DESC_ENTITY
    },

    /* Entity.discard()V */
    {
        "net/minecraft/world/entity/Entity",
        "discard",
        "()V",
        DTT_GUARD_CANCEL_VOID,
        GUARD_OWNER,
        GUARD_NAME_ENTITY,
        GUARD_DESC_ENTITY
    },

    /*
     * SynchedEntityData.set - the data watcher.
     *
     * This is the ONLY way health is written in modern MC (1.9+).
     * The health "field" isn't a field at all - it's stored inside
     * SynchedEntityData's internal keyed storage and written via
     * entityData.set(DATA_HEALTH_ID, value).
     *
     * The ENTITY_DATA_SET guard style doesn't cancel the call - it
     * INTERCEPTS it. The Java-side interceptEntityDataSet() method
     * receives (this, accessor, value), checks if the accessor is
     * the health accessor for a protected entity, and if so, replaces
     * the value with the protected health value (e.g. max health).
     * For non-health accessors, it passes through unchanged.
     *
     * This catches ANY code that tries to set health via the data
     * watcher, regardless of caller - including reflection-based
     * attacks that bypass LivingEntity.hurt() entirely.
     */

    /* SynchedEntityData.set(EntityDataAccessor, Object)V */
    {
        "net/minecraft/network/syncher/SynchedEntityData",
        "set",
        "(Lnet/minecraft/network/syncher/EntityDataAccessor;Ljava/lang/Object;)V",
        DTT_GUARD_ENTITY_DATA_SET,
        GUARD_OWNER,
        GUARD_NAME_SETDATA,
        GUARD_DESC_SETDATA
    },
};

#define G_GUARD_TABLE_SIZE (sizeof(G_GUARD_TABLE) / sizeof(G_GUARD_TABLE[0]))

void dtt_vanilla_guard_init(void) {
    dtt_log(DTT_LOG_INFO,
            "vanilla guard: %d method targets registered across %d classes",
            (int)G_GUARD_TABLE_SIZE, 5);
}

dtt_cf_result_t dtt_vanilla_guard_intercept(const char *class_name,
                                             const unsigned char *class_data,
                                             jint class_data_len) {
    dtt_cf_result_t fail = { NULL, 0 };
    int i;

    if (class_name == NULL || class_data == NULL || class_data_len <= 0) {
        return fail;
    }

    /* Don't touch our own mod classes - those are handled by the existing
     * tamper-detection/absorb/reflect pipeline. */
    if (dtt_is_protected_class(class_name)) {
        return fail;
    }

    for (i = 0; i < (int)G_GUARD_TABLE_SIZE; i++) {
        const dtt_vanilla_guard_entry_t *entry = &G_GUARD_TABLE[i];

        if (strcmp(class_name, entry->class_name) != 0) {
            continue;
        }

        /* Found a matching class - try to inject into the target method.
         * dtt_cf_inject_guard handles: method not found, already patched,
         * unsupported class shape - all return {NULL,0} which is safe. */
        {
            dtt_cf_result_t result = dtt_cf_inject_guard(
                class_data, class_data_len,
                entry->method_name, entry->method_desc,
                entry->guard_owner, entry->guard_name,
                entry->guard_desc, entry->style);

            if (result.data != NULL) {
                dtt_log(DTT_LOG_INFO,
                        "vanilla guard: injected %s.%s prologue (style=%d) - "
                        "guard calls %s.%s",
                        entry->class_name, entry->method_name,
                        (int)entry->style,
                        entry->guard_owner, entry->guard_name);
                return result;
            }
            /* If injection failed for this method, don't bail - try the
             * next table entry that matches the same class (e.g. if a
             * class has multiple methods to guard). */
        }
    }

    return fail; /* no guard targets matched, or all injections failed */
}
