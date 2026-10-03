package dev.skycraft.mixin;

import dev.skycraft.world.SkyCollision;
import net.minecraft.world.level.pathfinder.PathType;
import net.minecraft.world.level.pathfinder.PathfindingContext;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Mobs find their way over Halo's level, which isn't blocks: a cell its geometry reaches into the
 * upper half of is in the way like a block (so the cell above it is walkable). Ground lower in a
 * cell is handled by WalkNodeEvaluatorMixin.
 */
@Mixin(PathfindingContext.class)
public abstract class PathfindingContextMixin {
	@Inject(method = "getPathTypeFromState", at = @At("RETURN"), cancellable = true)
	private void skycraft$haloBlocks(int x, int y, int z, CallbackInfoReturnable<PathType> cir) {
		if (cir.getReturnValue() == PathType.OPEN && SkyCollision.blocksPath(x, y, z)) {
			cir.setReturnValue(PathType.BLOCKED);
		}
	}
}
