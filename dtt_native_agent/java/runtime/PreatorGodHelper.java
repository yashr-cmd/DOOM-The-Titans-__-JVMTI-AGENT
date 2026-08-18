package net.mcreator.transfinityimproved.coremod;

import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.network.syncher.SynchedEntityData;
import net.minecraft.network.syncher.EntityDataAccessor;

/**
 * Guard methods called by the native agent's injected bytecode prologues.
 *
 * When the native agent patches a vanilla class (LivingEntity, Entity,
 * SynchedEntityData), it prepends a guard prologue to specific methods
 * (hurt, die, kill, remove, discard, set). That prologue calls into
 * one of the static methods below via INVOKESTATIC.
 *
 * IMPORTANT LOAD ORDER: This class MUST be loaded before any vanilla
 * entity class. In a Forge coremod context, the coremod's
 * TransformerBundle/PreTransformer classes load before Minecraft classes,
 * and this class is referenced from there, so the JVM resolves it
 * lazily on first use. But if you move this class to a runtime
 * package that loads late, you'll get NoClassDefFoundError at the
 * first damage event.
 *
 * These methods are called from NATIVE CODE injected by the agent.
 * Do NOT rename or change their signatures without updating the guard
 * table in dtt_vanilla_guard.c.
 */
public final class PreatorGodHelper {

    private PreatorGodHelper() {}

    /* ====================================================================
     * Entity protection check - called by hurt(), die(), kill(), remove(),
     * discard() guard prologues.
     *
     * Returns TRUE if the entity is protected and the operation should
     * be CANCELLED (damage denied, death prevented, removal blocked).
     *
     * The native prologue does:
     *   ALOAD 0 (this = the entity)
     *   INVOKESTATIC PreatorGodHelper.isGod(LivingEntity)Z
     *   IFEQ skip
     *   <cancel return>
     *   skip:
     * ==================================================================== */
    public static boolean isGod(LivingEntity entity) {
        if (entity == null) {
            return false;
        }
        /* TODO: replace with your actual god-mode check.
         * This should return true when the entity has the armor/equipment
         * that grants protection. Examples:
         *   - Check for a specific item in armor slots
         *   - Check for a specific entity flag/attribute
         *   - Check a capability/data attachment
         *
         * The check must be FAST - this runs on every damage event for
         * every entity in the world. Keep it simple: slot check or
         * capability check, not a full inventory scan. */
        return false;
    }

    /* ====================================================================
     * Entity-type protection check - called by Entity.remove(),
     * Entity.discard() guard prologues.
     *
     * Same as isGod() but accepts Entity (the supertype). Used for
     * methods declared on Entity rather than LivingEntity.
     *
     * The native prologue does:
     *   ALOAD 0 (this = the entity)
     *   INVOKESTATIC PreatorGodHelper.isGodEntity(Entity)Z
     *   IFEQ skip
     *   <cancel return>
     *   skip:
     * ==================================================================== */
    public static boolean isGodEntity(Entity entity) {
        if (entity == null) {
            return false;
        }
        /* TODO: replace with your actual god-mode check.
         * If isGod() already handles LivingEntity, this method should
         * handle the Entity supertype case. For non-LivingEntity entities
         * (item frames, boats, etc.) you may want to always return false,
         * or check for specific entity types. */
        if (entity instanceof LivingEntity) {
            return isGod((LivingEntity) entity);
        }
        return false;
    }

    /* ====================================================================
     * SynchedEntityData.set interceptor - called by SynchedEntityData.set()
     * guard prologue.
     *
     * The native prologue does:
     *   ALOAD 0 (this = SynchedEntityData instance)
     *   ALOAD 1 (EntityDataAccessor key)
     *   ALOAD 2 (Object value being set)
     *   INVOKESTATIC PreatorGodHelper.interceptEntityDataSet(...)Object
     *   ASTORE 2 (store potentially modified value back)
     *   <original set() code continues with value in slot 2>
     *
     * This method intercepts writes to the data watcher. If the write
     * targets the HEALTH accessor on a protected entity, we clamp the
     * value to max health (preventing external code from setting health
     * to 0 or below via the data watcher path).
     *
     * For ALL OTHER writes (non-health, or unprotected entity), this
     * returns the original value unchanged - zero overhead for the
     * common case.
     * ==================================================================== */
    public static Object interceptEntityDataSet(
            SynchedEntityData dataAccessor,
            EntityDataAccessor<?> key,
            Object value) {
        /* TODO: implement your health-clamp logic.
         *
         * Pseudocode:
         *   1. Get the Entity that owns this SynchedEntityData
         *      (SynchedEntityData has a private 'entity' field - you
         *       may need to use your existing reflection/AT to access it)
         *   2. If entity is null or not a LivingEntity, return value unchanged
         *   3. Check if key matches the health data accessor
         *      (LivingEntity.DATA_HEALTH_ID or the equivalent)
         *   4. If so, and if isGod((LivingEntity)entity) returns true:
         *      - Parse value as Float
         *      - If value < maxHealth, return maxHealth instead
         *      - Otherwise return value unchanged
         *   5. Return original value for all other cases
         *
         * This is the deepest layer of defense: even if code bypasses
         * hurt() and die(), it can't set health to 0 via the data
         * watcher without going through this interceptor. */
        return value;
    }
}
