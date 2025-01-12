/*
	Copyright (c) 2022 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <math.h>
#include <stdlib.h>

#include "../cglm/cglm.h"

#include "../item/inventory.h"
#include "../platform/thread.h"
#include "client_interface.h"
#include "server_interface.h"
#include "server_local.h"

#define CHUNK_DIST2(x1, x2, z1, z2)                                            \
	(((x1) - (x2)) * ((x1) - (x2)) + ((z1) - (z2)) * ((z1) - (z2)))

static void server_local_process(struct server_rpc* call, void* user) {
	assert(call && user);

	struct server_local* s = user;

	switch(call->type) {
		case SRPC_PLAYER_POS:
			if(s->player.finished_loading) {
				s->player.x = call->payload.player_pos.x;
				s->player.y = call->payload.player_pos.y;
				s->player.z = call->payload.player_pos.z;
				s->player.rx = call->payload.player_pos.rx;
				s->player.ry = call->payload.player_pos.ry;
				s->player.has_pos = true;
			}
			break;
		case SRPC_HOTBAR_SLOT:
			if(s->player.has_pos
			   && call->payload.hotbar_slot.slot < INVENTORY_SIZE_HOTBAR)
				inventory_set_hotbar(&s->player.inventory,
									 call->payload.hotbar_slot.slot);
			break;
		case SRPC_BLOCK_DIG:
			if(s->player.has_pos && call->payload.block_dig.y >= 0
			   && call->payload.block_dig.y < WORLD_HEIGHT
			   && call->payload.block_dig.finished) {
				server_world_set_block(&s->world, call->payload.block_dig.x,
									   call->payload.block_dig.y,
									   call->payload.block_dig.z,
									   (struct block_data) {
										   .type = BLOCK_AIR,
										   .metadata = 0,
									   });
			}
			break;
		case SRPC_BLOCK_PLACE:
			if(s->player.has_pos && call->payload.block_place.y >= 0
			   && call->payload.block_place.y < WORLD_HEIGHT) {
				int x, y, z;
				blocks_side_offset(call->payload.block_place.side, &x, &y, &z);

				size_t slot = inventory_get_hotbar(&s->player.inventory);

				struct item_data it_data;
				inventory_get_slot(&s->player.inventory, slot, &it_data);
				struct item* it = item_get(&it_data);

				if(it && it->onItemPlace) {
					struct block_data blk_where, blk_on;
					if(server_world_get_block(
						   &s->world, call->payload.block_place.x + x,
						   call->payload.block_place.y + y,
						   call->payload.block_place.z + z, &blk_where)
					   && server_world_get_block(
						   &s->world, call->payload.block_place.x,
						   call->payload.block_place.y,
						   call->payload.block_place.z, &blk_on)) {
						struct block_info where = (struct block_info) {
							.block = &blk_where,
							.neighbours = NULL,
							.x = call->payload.block_place.x + x,
							.y = call->payload.block_place.y + y,
							.z = call->payload.block_place.z + z,
						};

						struct block_info on = (struct block_info) {
							.block = &blk_on,
							.neighbours = NULL,
							.x = call->payload.block_place.x,
							.y = call->payload.block_place.y,
							.z = call->payload.block_place.z,
						};

						if((!blocks[blk_where.type]
							|| blocks[blk_where.type]->place_ignore)
						   && it->onItemPlace(s, &it_data, &where, &on,
											  call->payload.block_place.side)) {
							inventory_consume(&s->player.inventory, slot);

							clin_rpc_send(&(struct client_rpc) {
								.type = CRPC_INVENTORY_SLOT,
								.payload.inventory_slot.slot = slot,
								.payload.inventory_slot.item
								= s->player.inventory.items[slot],
							});
						}
					}
				}
			}
			break;
		case SRPC_UNLOAD_WORLD:
			// save chunks here, then destroy all
			clin_rpc_send(&(struct client_rpc) {
				.type = CRPC_WORLD_RESET,
				.payload.world_reset.dimension = WORLD_DIM_OVERWORLD,
			});

			level_archive_write_player(
				&s->level, (vec3) {s->player.x, s->player.y, s->player.z},
				(vec2) {s->player.rx, s->player.ry}, NULL, s->player.dimension);

			server_world_destroy(&s->world);
			level_archive_destroy(&s->level);

			s->player.has_pos = false;
			s->player.finished_loading = false;
			string_reset(s->level_name);
			break;
		case SRPC_LOAD_WORLD:
			assert(!s->player.has_pos);

			string_set(s->level_name, call->payload.load_world.name);
			string_clear(call->payload.load_world.name);

			if(level_archive_create(&s->level, s->level_name)) {
				vec3 pos;
				vec2 rot;
				enum world_dim dim;
				if(level_archive_read_player(&s->level, pos, rot, NULL, &dim)) {
					server_world_create(&s->world, s->level_name, dim);
					s->player.x = pos[0];
					s->player.y = pos[1];
					s->player.z = pos[2];
					s->player.rx = rot[0];
					s->player.ry = rot[1];
					s->player.dimension = dim;
					s->player.has_pos = true;
				}

				if(level_archive_read(&s->level, LEVEL_TIME, &s->world_time, 0))
					s->world_time_start = time_get();

				clin_rpc_send(&(struct client_rpc) {
					.type = CRPC_WORLD_RESET,
					.payload.world_reset.dimension = dim,
				});
			}
			break;
	}
}

static void server_local_update(struct server_local* s) {
	assert(s);

	svin_process_messages(server_local_process, s, false);

	if(!s->player.has_pos)
		return;

	w_coord_t px = WCOORD_CHUNK_OFFSET(floor(s->player.x));
	w_coord_t pz = WCOORD_CHUNK_OFFSET(floor(s->player.z));

	w_coord_t cx, cz;
	if(server_world_furthest_chunk(&s->world, MAX_VIEW_DISTANCE, px, pz, &cx,
								   &cz)) {
		// unload just one chunk
		server_world_save_chunk(&s->world, true, cx, cz);
		clin_rpc_send(&(struct client_rpc) {
			.type = CRPC_UNLOAD_CHUNK,
			.payload.unload_chunk.x = cx,
			.payload.unload_chunk.z = cz,
		});
	}

	// iterate over all chunks that should be loaded
	bool c_nearest = false;
	w_coord_t c_nearest_x, c_nearest_z;
	w_coord_t c_nearest_dist2;
	for(w_coord_t z = pz - MAX_VIEW_DISTANCE; z <= pz + MAX_VIEW_DISTANCE;
		z++) {
		for(w_coord_t x = px - MAX_VIEW_DISTANCE; x <= px + MAX_VIEW_DISTANCE;
			x++) {
			w_coord_t d = CHUNK_DIST2(px, x, pz, z);
			if(!server_world_is_chunk_loaded(&s->world, x, z)
			   && (d < c_nearest_dist2 || !c_nearest)
			   && server_world_disk_has_chunk(&s->world, x, z)) {
				c_nearest_dist2 = d;
				c_nearest_x = x;
				c_nearest_z = z;
				c_nearest = true;
			}
		}
	}

	// load just one chunk
	struct server_chunk* sc;
	if(c_nearest
	   && server_world_load_chunk(&s->world, c_nearest_x, c_nearest_z, &sc)) {
		size_t sz = CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
		void* ids = malloc(sz);
		void* metadata = malloc(sz / 2);
		void* lighting_sky = malloc(sz / 2);
		void* lighting_torch = malloc(sz / 2);

		memcpy(ids, sc->ids, sz);
		memcpy(metadata, sc->metadata, sz / 2);
		memcpy(lighting_sky, sc->lighting_sky, sz / 2);
		memcpy(lighting_torch, sc->lighting_torch, sz / 2);

		clin_rpc_send(&(struct client_rpc) {
			.type = CRPC_CHUNK,
			.payload.chunk.x = c_nearest_x * CHUNK_SIZE,
			.payload.chunk.y = 0,
			.payload.chunk.z = c_nearest_z * CHUNK_SIZE,
			.payload.chunk.sx = CHUNK_SIZE,
			.payload.chunk.sy = WORLD_HEIGHT,
			.payload.chunk.sz = CHUNK_SIZE,
			.payload.chunk.ids = ids,
			.payload.chunk.metadata = metadata,
			.payload.chunk.lighting_sky = lighting_sky,
			.payload.chunk.lighting_torch = lighting_torch,
		});
	} else if(!s->player.finished_loading) {
		struct client_rpc pos;
		pos.type = CRPC_PLAYER_POS;
		if(level_archive_read_player(&s->level, pos.payload.player_pos.position,
									 pos.payload.player_pos.rotation, NULL,
									 NULL))
			clin_rpc_send(&pos);

		s->world_time_start = time_get();

		clin_rpc_send(&(struct client_rpc) {
			.type = CRPC_TIME_SET,
			.payload.time_set = s->world_time,
		});

		if(level_archive_read_inventory(&s->level, &s->player.inventory)) {
			for(size_t k = 0; k < INVENTORY_SIZE; k++) {
				if(s->player.inventory.items[k].id > 0) {
					clin_rpc_send(&(struct client_rpc) {
						.type = CRPC_INVENTORY_SLOT,
						.payload.inventory_slot.slot = k,
						.payload.inventory_slot.item
						= s->player.inventory.items[k],
					});
				}
			}
		}

		s->player.finished_loading = true;
	}
}

static void* server_local_thread(void* user) {
	while(1) {
		server_local_update(user);
		thread_msleep(50);
	}

	return NULL;
}

void server_local_create(struct server_local* s) {
	assert(s);
	s->world_time = 0;
	s->world_time_start = time_get();
	s->player.has_pos = false;
	s->player.finished_loading = false;
	string_init(s->level_name);
	inventory_clear(&s->player.inventory);

	struct thread t;
	thread_create(&t, server_local_thread, s, 8);
}
