#include "blocks.h"

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_STONE;
}

static bool getBoundingBox(struct block_info* this, bool entity,
						   struct AABB* x) {
	aabb_setsize(x, 1.0F, 1.0F, 1.0F);
	return true;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_full();
}

static enum block_render_type getRenderType(struct block_info* this) {
	return RENDERTYPE_FULL;
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	switch(this->block->metadata) {
		default:
		case 0:
			return (side == SIDE_TOP || side == SIDE_BOTTOM) ?
				TEXTURE_INDEX(6, 0) :
				TEXTURE_INDEX(5, 0);
		case 1:
			return (side == SIDE_TOP) ?
				TEXTURE_INDEX(0, 11) :
				((side == SIDE_BOTTOM) ? TEXTURE_INDEX(0, 13) :
										 TEXTURE_INDEX(0, 12));
		case 2: return TEXTURE_INDEX(4, 0);
		case 3: return TEXTURE_INDEX(0, 1);
	}
}

static uint32_t getBaseColor(struct block_info* this, enum side side) {
	return 0xFFFFFF;
}

struct block block_double_slab = {
	.name = "Double Slab",
	.getRenderType = getRenderType,
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.transparent = false,
	.getBaseColor = getBaseColor,
	.renderBlock = render_block_full,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
};