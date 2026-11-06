// Touhou 12.5: Double Spoiler v1.00a

#include <stdio.h>
#include <string.h>
#include <Windows.h>
#include <TlHelp32.h>
#include "patch_util.h"
#include "limiter.h"
#include "d3d9_hook.h"
#include "config.h"
#include "overlay.h"
#include "tool_state.h"
#include "touhou125.h"

using namespace Touhou125;

CReplayManager** CReplayManager::InstancePtr = (CReplayManager**)0x004B68CC;

// ---------------------------------------------------------------------------
// Frame interpolation, stage 0: extra presentations
// ---------------------------------------------------------------------------
//
// The main loop selects one of three frame functions:
//
//   0x44E2FE -> sub_44ED10
//   0x44E31B -> sub_44F310
//   0x44E327 -> sub_44F0D0
//
// All three first submit the previous sprite queue, set up the current camera and
// viewport, then run the update walk sub_461870. When their frame divider lets
// them draw, they run the same block:
//
//   BeginScene
//   sub_458E90       clear the sprite queue
//   [0x4D1660] = 0xFF
//   sub_429FA0       prepare the render context (EDI = 0x4D0CD0)
//   sub_4619D0       draw the object walk at [0x4D0C50]+0x3C
//   sub_458ED0       submit the sprite queue (ESI = [0x4D0CB4])
//   SetTexture(0, NULL)
//   EndScene
//
// The three draw calls are 0x44EE1E, 0x44F231 and 0x44F3D9. sub_4619D0 has no
// other callers. The three presents are the five-byte device loads at 0x44F02A,
// 0x44F296 and 0x44F4B7; each is followed by the same 16-byte argument setup.
//
// The frame tails call sub_44F540 before Present. That function consumes the
// snapshot request and writes snapshot\th###.bmp. An extra presentation never
// calls a frame tail, so it cannot consume that request a second time.

static auto th125_reset_sprite_queue = (void(*)())0x00458E90;
static auto th125_draw_sprite_queue_address = (void*)0x00458ED0;
static auto th125_render_objects = (void(*)())0x004619D0;
static auto th125_prepare_render_address = (void*)0x00429FA0;
static auto th125_camera_setup_address = (void*)0x0042A580;

static DWORD* const th125_render_flag = (DWORD*)0x004D1660;
static void** const th125_camera_current = (void**)0x004D121C;
static DWORD* const th125_camera_kind = (DWORD*)0x004D1220;

static IDirect3DDevice9* th125_device() {
	IDirect3DDevice9* device = *(IDirect3DDevice9**)0x004D0CD8;
	if (device != nullptr)
		return device;
	return d3d9_hooked_device();
}

// sub_429FA0 takes its context in EDI.
static void __declspec(naked) th125_prepare_render() {
	__asm {
		push edi
		mov edi, 004D0CD0h
		call dword ptr [th125_prepare_render_address]
		pop edi
		ret
	}
}

// sub_458ED0 takes the sprite queue owner in ESI.
static void __declspec(naked) th125_draw_sprite_queue() {
	__asm {
		push esi
		mov esi, dword ptr ds:[004D0CB4h]
		call dword ptr [th125_draw_sprite_queue_address]
		pop esi
		ret
	}
}

// sub_42A580 takes the current camera in EDI.
static void __declspec(naked) th125_camera_setup() {
	__asm {
		push edi
		mov edi, dword ptr ds:[004D121Ch]
		call dword ptr [th125_camera_setup_address]
		pop edi
		ret
	}
}

// The extra draw changes the text manager while it builds and consumes the
// frame's text, and the cameras while its draw callbacks select a view. Save
// those owners, no game manager. In particular, dword_4B6898 is owned by the
// stage worker; copying its 0x78 bytes here can put back an old hold bit after
// that worker has advanced the stage.
//
// The text task pointers at +0x0C, +0x10 and +0x1C848 are deliberately outside
// the saved range. The loader creates the AsciiInf object before it creates
// those nodes, so restoring the whole object could overwrite a live node with
// zero.
static void** const th125_text_manager = (void**)0x004B6770;
static void** const th125_task_manager = (void**)0x004D0C50;
static void** const th125_sprite_queue = (void**)0x004D0CB4;
static void** const th125_player_manager = (void**)0x004B68C8;
static void** const th125_text_anm_resource = (void**)0x004D1258;
static void** const th125_loading_manager = (void**)0x004B68A8;
static void** const th125_stage_thread = (void**)0x004B6784;
static const size_t th125_text_dynamic_offset = 0x98C;
static const size_t th125_text_dynamic_size = 0x1BEB4;
static const size_t th125_player_camera = 0x2484;
static const size_t th125_player_camera_state = 0x00;
static const size_t th125_camera_size = 0x118;
static const size_t th125_camera_count = 3;
static const size_t th125_camera_bytes = th125_camera_size * th125_camera_count;
static void* const th125_camera_array = (void*)0x004D0ED4;
static const size_t th125_queue_counters_offset = 0xA0;
static const size_t th125_queue_counters_size = 4 * sizeof(DWORD);
static void* const th125_queue_cache_base = (void*)0x004BD640;
static const size_t th125_queue_cache_size = 0x18;
static float* const th125_scene_shake = (float*)0x004D11EC;
static const size_t th125_queue_screen_offset = 0xB0;
static const size_t th125_background_data = 0x10;
static const size_t th125_background_vm_pool = 0x1C8;
static const size_t th125_background_vm_size = 0x4BC;
static const size_t th125_background_vm_max = 256;
static const size_t th125_fixed_render_anm_size = 0x4BC;
static void* const th125_fixed_render_anm_slots[] = {
	(void*)0x004D0E8C,
	(void*)0x004D0E90,
	(void*)0x004D0E94,
};
static const size_t th125_fixed_render_anm_count =
	sizeof(th125_fixed_render_anm_slots) / sizeof(th125_fixed_render_anm_slots[0]);

struct Th125DrawSnapshot {
	void* text;
	void* queue;
	void* camera;
	void* background;
	void* background_vms;
	size_t background_vm_count;
	DWORD camera_kind;
	DWORD render_flag;
	unsigned char cameras[th125_camera_bytes];
	unsigned char queue_cache[th125_queue_cache_size];
	DWORD queue_counters[4];
	float screen_offset[2];
	float shake[2];
	bool text_valid;
	bool queue_valid;
	bool background_vms_valid;
};

static Th125DrawSnapshot th125_render_start;
static Th125DrawSnapshot th125_live;
static unsigned char th125_render_start_text[th125_text_dynamic_size];
static unsigned char th125_live_text[th125_text_dynamic_size];
static unsigned char
	th125_render_start_background_vms[th125_background_vm_max * th125_background_vm_size];
static unsigned char
	th125_live_background_vms[th125_background_vm_max * th125_background_vm_size];
static void* th125_render_start_anms[th125_fixed_render_anm_count];
static void* th125_live_anms[th125_fixed_render_anm_count];
static bool th125_render_start_anm_valid[th125_fixed_render_anm_count];
static bool th125_live_anm_valid[th125_fixed_render_anm_count];
static unsigned char
	th125_render_start_anm_data[th125_fixed_render_anm_count * th125_fixed_render_anm_size];
static unsigned char
	th125_live_anm_data[th125_fixed_render_anm_count * th125_fixed_render_anm_size];
static bool th125_frame_ready;
static bool th125_in_extra;
static IDirect3DSurface9* th125_background_surface;
static IDirect3DDevice9* th125_background_surface_device;
static D3DSURFACE_DESC th125_background_surface_desc;
static unsigned int th125_state_mismatches;
static unsigned int th125_render_calls;
static unsigned int th125_present_calls;
static unsigned int th125_extra_presentations;
static unsigned int th125_text_changed_max;
static unsigned int th125_draw_task_removal_guards;
static unsigned int th125_queue_cache_changed_max;
static float th125_shake_observed_max;
static unsigned int th125_background_vm_changed_max;
static size_t th125_background_vm_count_seen;
static unsigned int th125_background_surface_captures;
static unsigned int th125_background_surface_restores;
static unsigned int th125_background_surface_failures;
static unsigned int th125_fixed_render_anm_changed_max[th125_fixed_render_anm_count];

struct Th125SceneIdentity {
	void* bullet_manager;
	void* ecl_manager;
	void* item_manager;
	void* player;
	void* scene;
	void* background;
	DWORD mode;
};

static Th125SceneIdentity th125_render_identity;

static Th125SceneIdentity th125_scene_identity() {
	Th125SceneIdentity identity = {};
	identity.bullet_manager = *(void**)0x004B677C;
	identity.ecl_manager = *(void**)0x004B678C;
	identity.item_manager = *(void**)0x004B68A0;
	identity.player = *(void**)0x004B68C8;
	identity.scene = *(void**)0x004B6898;
	identity.background = *(void**)0x004B6778;
	identity.mode = *(DWORD*)0x004D1228;
	return identity;
}

static bool th125_same_scene(const Th125SceneIdentity& a, const Th125SceneIdentity& b) {
	return a.bullet_manager == b.bullet_manager &&
		a.ecl_manager == b.ecl_manager &&
		a.item_manager == b.item_manager &&
		a.player == b.player &&
		a.scene == b.scene &&
		a.background == b.background &&
		a.mode == b.mode;
}

// Bit 0 and bit 2 make the update and object draw tasks hold their picture.
// They are an object-motion condition, not a reason to stop presenting frames.
static bool th125_objects_held() {
	void* scene = *(void**)0x004B6898;
	if (scene != nullptr) {
		const DWORD flags = *(DWORD*)((char*)scene + 0x60);
		if (((flags | (flags >> 2)) & 1) != 0)
			return true;
	}

	unsigned char* player = (unsigned char*)*th125_player_manager;
	if (player == nullptr)
		return false;
	const DWORD photo_state =
		*(DWORD*)(player + th125_player_camera + th125_player_camera_state);
	return photo_state == 2 || photo_state == 3;
}

static bool th125_runtime_ready() {
	if (th125_device() == nullptr ||
		*th125_text_manager == nullptr ||
		*th125_task_manager == nullptr ||
		*th125_sprite_queue == nullptr ||
		*th125_text_anm_resource == nullptr) {
		return false;
	}

	void* loading = *th125_loading_manager;
	if (loading != nullptr) {
		if ((*(DWORD*)loading & 2) != 0 ||
			*(int*)((char*)loading + 0x4F8) == 0) {
			return false;
		}
		HANDLE thread = *(HANDLE*)((char*)loading + 0x14);
		if (thread != nullptr && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT)
			return false;
	}

	void* stage_thread = *th125_stage_thread;
	if (stage_thread != nullptr &&
		*(DWORD*)((char*)stage_thread + 0x30) != 1) {
		return false;
	}
	return true;
}

static __declspec(noinline) bool th125_is_extra() {
	return th125_in_extra;
}

struct Th125DrawProbeEntry {
	void* callback;
	void* object;
	DWORD priority;
	unsigned int calls;
};

static const size_t th125_draw_probe_capacity = 64;
static Th125DrawProbeEntry th125_draw_probe[th125_draw_probe_capacity];
static size_t th125_draw_probe_count;

static Th125DrawProbeEntry* th125_draw_probe_entry(void* callback, void* object) {
	for (size_t i = 0; i < th125_draw_probe_count; ++i) {
		if (th125_draw_probe[i].callback == callback &&
			th125_draw_probe[i].object == object) {
			return &th125_draw_probe[i];
		}
	}
	if (th125_draw_probe_count >= th125_draw_probe_capacity)
		return nullptr;
	Th125DrawProbeEntry& entry = th125_draw_probe[th125_draw_probe_count++];
	entry = {};
	entry.callback = callback;
	entry.object = object;
	entry.priority = 0;
	return &entry;
}

static void th125_draw_probe_collect() {
	th125_draw_probe_count = 0;
	void* manager = *th125_task_manager;
	if (manager == nullptr)
		return;
	void* node = *(void**)((char*)manager + 0x3C);
	for (size_t guard = 0; node != nullptr && guard < 256; ++guard) {
		void* task = *(void**)node;
		void* next = *(void**)((char*)node + 4);
		if (task != nullptr) {
			void* callback = *(void**)((char*)task + 8);
			void* object = *(void**)((char*)task + 32);
			if (callback != nullptr) {
				Th125DrawProbeEntry* entry =
					th125_draw_probe_entry(callback, object);
				if (entry != nullptr) {
					entry->priority = *(DWORD*)task;
					++entry->calls;
				}
			}
		}
		node = next;
	}
}

// The draw walk removes a task when its callback returns zero. An extra pass must
// be able to repeat callbacks without changing the chain the game will use next.
static void (__fastcall* th125_remove_draw_task)(void*, void*) =
	(void(__fastcall*)(void*, void*))0x00461C40;

static void __fastcall th125_remove_draw_task_extra_guard(void* manager, void* task) {
	if (th125_in_extra) {
		++th125_draw_task_removal_guards;
		return;
	}
	th125_remove_draw_task(manager, task);
}

static void th125_release_background_surface() {
	if (th125_background_surface != nullptr) {
		th125_background_surface->Release();
		th125_background_surface = nullptr;
	}
	th125_background_surface_device = nullptr;
	th125_background_surface_desc = {};
}

static bool th125_background_surface_ready(IDirect3DDevice9* device) {
	IDirect3DSurface9* source = *(IDirect3DSurface9**)0x004D0E7C;
	D3DSURFACE_DESC desc = {};
	if (device == nullptr || source == nullptr ||
		FAILED(source->GetDesc(&desc)) ||
		desc.MultiSampleType != D3DMULTISAMPLE_NONE ||
		desc.Width == 0 || desc.Height == 0) {
		return false;
	}
	if (th125_background_surface != nullptr &&
		th125_background_surface_device == device &&
		th125_background_surface_desc.Width == desc.Width &&
		th125_background_surface_desc.Height == desc.Height &&
		th125_background_surface_desc.Format == desc.Format) {
		return true;
	}
	th125_release_background_surface();
	if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
		D3DMULTISAMPLE_NONE, 0, FALSE, &th125_background_surface, nullptr))) {
		th125_background_surface = nullptr;
		return false;
	}
	th125_background_surface_device = device;
	th125_background_surface_desc = desc;
	return true;
}

static int __fastcall th125_background_commit_guard(void* self, void*) {
	const int result = ((int(__thiscall*)(void*))0x00428DC0)(self);
	IDirect3DDevice9* device = th125_device();
	IDirect3DSurface9* source = *(IDirect3DSurface9**)0x004D0E7C;
	if (device == nullptr || source == nullptr) {
		++th125_background_surface_failures;
		return result;
	}
	if (th125_in_extra) {
		if (th125_background_surface == nullptr ||
			th125_background_surface_device != device ||
			FAILED(device->StretchRect(th125_background_surface, nullptr, source,
				nullptr, D3DTEXF_NONE))) {
			++th125_background_surface_failures;
		} else {
			++th125_background_surface_restores;
		}
	} else if (!th125_background_surface_ready(device) ||
		FAILED(device->StretchRect(source, nullptr, th125_background_surface,
			nullptr, D3DTEXF_NONE))) {
		++th125_background_surface_failures;
	} else {
		++th125_background_surface_captures;
	}
	return result;
}

static void th125_install_background_commit_guard() {
	void* manager = *th125_task_manager;
	if (manager == nullptr)
		return;
	void* node = *(void**)((char*)manager + 0x3C);
	for (size_t guard = 0; node != nullptr && guard < 256; ++guard) {
		void* task = *(void**)node;
		void* next = *(void**)((char*)node + 4);
		if (task != nullptr) {
			void** callback = (void**)((char*)task + 8);
			if (*callback == (void*)0x00428DC0)
				*callback = (void*)th125_background_commit_guard;
		}
		node = next;
	}
}

static void th125_capture_draw_state(Th125DrawSnapshot& snapshot,
	unsigned char* text_storage, unsigned char* background_vm_storage) {
	snapshot.text = *th125_text_manager;
	snapshot.text_valid = false;
	if (snapshot.text != nullptr) {
		memcpy(text_storage, (unsigned char*)snapshot.text + th125_text_dynamic_offset,
			th125_text_dynamic_size);
		snapshot.text_valid = true;
	}

	memcpy(snapshot.cameras, th125_camera_array, th125_camera_bytes);
	snapshot.camera = *th125_camera_current;
	snapshot.camera_kind = *th125_camera_kind;
	snapshot.render_flag = *th125_render_flag;
	snapshot.shake[0] = th125_scene_shake[0];
	snapshot.shake[1] = th125_scene_shake[1];

	snapshot.queue = *th125_sprite_queue;
	snapshot.queue_valid = false;
	if (snapshot.queue != nullptr) {
		memcpy(snapshot.queue_counters,
			(unsigned char*)snapshot.queue + th125_queue_counters_offset,
			th125_queue_counters_size);
		memcpy(snapshot.queue_cache,
			(unsigned char*)th125_queue_cache_base + (size_t)snapshot.queue,
			th125_queue_cache_size);
		memcpy(snapshot.screen_offset,
			(unsigned char*)snapshot.queue + th125_queue_screen_offset,
			sizeof(snapshot.screen_offset));
		snapshot.queue_valid = true;
	}

	snapshot.background = *(void**)0x004B6778;
	snapshot.background_vms = nullptr;
	snapshot.background_vm_count = 0;
	snapshot.background_vms_valid = false;
	if (snapshot.background != nullptr) {
		const unsigned char* data =
			*(const unsigned char**)((const unsigned char*)snapshot.background +
				th125_background_data);
		if (data != nullptr) {
			const short count = *(const short*)(data + 2);
			snapshot.background_vms =
				*(void**)((unsigned char*)snapshot.background + th125_background_vm_pool);
			if (snapshot.background_vms != nullptr &&
				count > 0 && (size_t)count <= th125_background_vm_max) {
				snapshot.background_vm_count = (size_t)count;
				memcpy(background_vm_storage, snapshot.background_vms,
					snapshot.background_vm_count * th125_background_vm_size);
				snapshot.background_vms_valid = true;
			}
		}
	}
}

static bool th125_restore_draw_state(
	const Th125DrawSnapshot& snapshot, const unsigned char* text_storage,
	const unsigned char* background_vm_storage) {
	bool restored = snapshot.text_valid;
	if (snapshot.text_valid && *th125_text_manager == snapshot.text) {
		memcpy((unsigned char*)snapshot.text + th125_text_dynamic_offset,
			text_storage, th125_text_dynamic_size);
	} else if (snapshot.text_valid) {
		++th125_state_mismatches;
		restored = false;
	}

	memcpy(th125_camera_array, snapshot.cameras, th125_camera_bytes);
	*th125_camera_current = snapshot.camera;
	*th125_camera_kind = snapshot.camera_kind;
	*th125_render_flag = snapshot.render_flag;
	th125_scene_shake[0] = snapshot.shake[0];
	th125_scene_shake[1] = snapshot.shake[1];

	if (snapshot.queue_valid && *th125_sprite_queue == snapshot.queue) {
		memcpy((unsigned char*)snapshot.queue + th125_queue_counters_offset,
			snapshot.queue_counters, th125_queue_counters_size);
		memcpy((unsigned char*)th125_queue_cache_base + (size_t)snapshot.queue,
			snapshot.queue_cache, th125_queue_cache_size);
		memcpy((unsigned char*)snapshot.queue + th125_queue_screen_offset,
			snapshot.screen_offset, sizeof(snapshot.screen_offset));
	} else if (snapshot.queue_valid) {
		++th125_state_mismatches;
		restored = false;
	}

	if (snapshot.background_vms_valid) {
		void* background = *(void**)0x004B6778;
		void* vms = background != nullptr
			? *(void**)((unsigned char*)background + th125_background_vm_pool)
			: nullptr;
		if (background == snapshot.background && vms == snapshot.background_vms) {
			memcpy(vms, background_vm_storage,
				snapshot.background_vm_count * th125_background_vm_size);
		} else {
			++th125_state_mismatches;
			restored = false;
		}
	}
	return restored;
}

static void th125_measure_extra_changes() {
	void* text = *th125_text_manager;
	if (!th125_live.text_valid || text == nullptr || text != th125_live.text)
		return;
	const unsigned char* current =
		(const unsigned char*)text + th125_text_dynamic_offset;
	unsigned int changed = 0;
	for (size_t byte = 0; byte < th125_text_dynamic_size; ++byte) {
		if (current[byte] != th125_live_text[byte])
			++changed;
	}
	if (changed > th125_text_changed_max)
		th125_text_changed_max = changed;
}

static void th125_measure_queue_cache_changes() {
	void* queue = *th125_sprite_queue;
	if (!th125_live.queue_valid || queue == nullptr || queue != th125_live.queue)
		return;
	const unsigned char* current =
		(const unsigned char*)th125_queue_cache_base + (size_t)queue;
	unsigned int changed = 0;
	for (size_t byte = 0; byte < th125_queue_cache_size; ++byte) {
		if (current[byte] != th125_live.queue_cache[byte])
			++changed;
	}
	if (changed > th125_queue_cache_changed_max)
		th125_queue_cache_changed_max = changed;
}

static void th125_measure_background_vm_changes() {
	void* background = *(void**)0x004B6778;
	void* vms = background != nullptr
		? *(void**)((unsigned char*)background + th125_background_vm_pool)
		: nullptr;
	th125_background_vm_count_seen = th125_live.background_vm_count;
	if (!th125_live.background_vms_valid || background != th125_live.background ||
		vms != th125_live.background_vms) {
		return;
	}
	const unsigned char* current = (const unsigned char*)vms;
	unsigned int changed = 0;
	const size_t bytes = th125_live.background_vm_count * th125_background_vm_size;
	for (size_t byte = 0; byte < bytes; ++byte) {
		if (current[byte] != th125_live_background_vms[byte])
			++changed;
	}
	if (changed > th125_background_vm_changed_max)
		th125_background_vm_changed_max = changed;
}

static void th125_capture_fixed_render_anms(void** objects, bool* valid,
	unsigned char* storage) {
	for (size_t i = 0; i < th125_fixed_render_anm_count; ++i) {
		objects[i] = *(void**)th125_fixed_render_anm_slots[i];
		valid[i] = objects[i] != nullptr;
		if (valid[i]) {
			memcpy(storage + i * th125_fixed_render_anm_size, objects[i],
				th125_fixed_render_anm_size);
		}
	}
}

static bool th125_restore_fixed_render_anms(void* const* objects, const bool* valid,
	const unsigned char* storage) {
	bool restored = true;
	for (size_t i = 0; i < th125_fixed_render_anm_count; ++i) {
		if (!valid[i])
			continue;
		void* current = *(void**)th125_fixed_render_anm_slots[i];
		if (current != objects[i]) {
			++th125_state_mismatches;
			restored = false;
			continue;
		}
		memcpy(current, storage + i * th125_fixed_render_anm_size,
			th125_fixed_render_anm_size);
	}
	return restored;
}

static void th125_measure_fixed_render_anm_changes() {
	for (size_t i = 0; i < th125_fixed_render_anm_count; ++i) {
		if (!th125_render_start_anm_valid[i])
			continue;
		void* current = *(void**)th125_fixed_render_anm_slots[i];
		if (current != th125_render_start_anms[i])
			continue;
		const unsigned char* bytes = (const unsigned char*)current;
		const unsigned char* saved =
			th125_render_start_anm_data + i * th125_fixed_render_anm_size;
		unsigned int changed = 0;
		for (size_t byte = 0; byte < th125_fixed_render_anm_size; ++byte) {
			if (bytes[byte] != saved[byte])
				++changed;
		}
		if (changed > th125_fixed_render_anm_changed_max[i])
			th125_fixed_render_anm_changed_max[i] = changed;
	}
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 1: project the enemy bullets forward
// ---------------------------------------------------------------------------
//
// sub_407710 allocates one 0x4FC0DC byte manager at [0x4B677C]. Its pool starts
// at manager+0x64 and holds 2001 objects of 0xA34 bytes. The update task
// sub_408950 runs sub_408740, which walks the first 2000 slots and threads every
// active bullet onto one of six layer lists at manager+0x14, using bullet+0x518
// as the link. The draw task sub_408980 walks those same six lists through
// sub_408880, so this is the set that actually reaches the sprite queue.
//
// The movement in sub_407900 adds velocity +0x4D0/+0x4D4/+0x4D8 to position
// +0x4C4/+0x4C8/+0x4CC. State +0x512 chooses the step: 1 moves a full frame,
// 2, 3 and 5 move half a frame, and the other states do not move. The global
// 0x4B5308 scales all of those steps. The game renders once every
// byte_4D0EB6 + 1 updates, so an extra presentation advances by that many
// updates times its fractional position in the rendered frame.
//
// sub_408880 copies the position into the sprite fields +0x434/+0x438/+0x43C
// and queues the animation object at bullet+0x08. Restoring the position after
// the render is therefore enough to leave the game's own next update untouched.

static void** const th125_bullet_manager = (void**)0x004B677C;
static float* const th125_bullet_time_scale = (float*)0x004B5308;
static unsigned char* const th125_frames_per_render = (unsigned char*)0x004D0EB6;
static const size_t th125_bullet_list_heads = 0x14;
static const size_t th125_bullet_columns = 6;
static const size_t th125_bullet_next = 0x518;
static const size_t th125_bullet_position = 0x4C4;
static const size_t th125_bullet_velocity = 0x4D0;
static const size_t th125_bullet_state = 0x512;
static const size_t th125_bullets_max = 2000;
static const float th125_bullet_fast_speed = 16.0f;

static unsigned int th125_bullets_in_play;
static unsigned int th125_bullets_projected;
static unsigned int th125_bullets_fast_excluded;

struct Th125SavedBullet {
	void* bullet;
	float position[3];
};

static Th125SavedBullet th125_saved_bullets[th125_bullets_max];
static size_t th125_saved_bullets_count;

static float th125_bullet_step_factor(unsigned short state) {
	switch (state) {
	case 1:
		return 1.0f;
	case 2:
	case 3:
	case 5:
		return 0.5f;
	default:
		return 0.0f;
	}
}

static void th125_bullets_advance(float t) {
	th125_saved_bullets_count = 0;
	th125_bullets_in_play = 0;
	th125_bullets_projected = 0;
	th125_bullets_fast_excluded = 0;

	unsigned char* manager = (unsigned char*)*th125_bullet_manager;
	if (manager == nullptr || th125_objects_held())
		return;

	unsigned int frames_per_render = *th125_frames_per_render;
	if (frames_per_render > 2)
		frames_per_render = 2;

	const float step = t * (float)(frames_per_render + 1) * *th125_bullet_time_scale;
	for (size_t column = 0; column < th125_bullet_columns; ++column) {
		unsigned char* bullet =
			*(unsigned char**)(manager + th125_bullet_list_heads + column * sizeof(void*));
		for (size_t guard = 0; bullet != nullptr && guard < th125_bullets_max; ++guard) {
			unsigned char* next = *(unsigned char**)(bullet + th125_bullet_next);
			++th125_bullets_in_play;

			const unsigned short state = *(unsigned short*)(bullet + th125_bullet_state);
			const float factor = th125_bullet_step_factor(state);
			if (factor > 0.0f && th125_saved_bullets_count < th125_bullets_max) {
				float* position = (float*)(bullet + th125_bullet_position);
				const float* velocity = (const float*)(bullet + th125_bullet_velocity);

				// TH12.5-ONLY SPECIAL CASE.
				// Ex-4's special bullet already skips collision frames at 60 FPS. Interpolating
				// it turns those discrete jumps into an unreadable continuous sweep. This speed
				// limit is a workaround for that scene in Double Spoiler only. Do not copy it to
				// another game.
				const float speed_squared =
					velocity[0] * velocity[0] +
					velocity[1] * velocity[1] +
					velocity[2] * velocity[2];
				if (speed_squared > th125_bullet_fast_speed * th125_bullet_fast_speed) {
					++th125_bullets_fast_excluded;
					bullet = next;
					continue;
				}

				Th125SavedBullet& saved = th125_saved_bullets[th125_saved_bullets_count++];
				saved.bullet = bullet;
				for (int axis = 0; axis < 3; ++axis) {
					saved.position[axis] = position[axis];
					position[axis] += velocity[axis] * step * factor;
				}
				++th125_bullets_projected;
			}
			bullet = next;
		}
	}
}

static void th125_bullets_undo() {
	for (size_t i = 0; i < th125_saved_bullets_count; ++i) {
		const Th125SavedBullet& saved = th125_saved_bullets[i];
		float* position = (float*)((char*)saved.bullet + th125_bullet_position);
		for (int axis = 0; axis < 3; ++axis)
			position[axis] = saved.position[axis];
	}
	th125_saved_bullets_count = 0;
}

// sub_437260 updates the player's 0x1414-byte camera/viewfinder subobject at
// player+0x2484. The four child Anm objects start at +0x48, +0x504, +0x9C0 and
// +0xE7C in that subobject; sub_45B920 reads their drawn position from +0x438.
// The crosshair/photo-frame centre is camera+0x13CC, which is player+0x3850.
// The player body's draw source is the logic position at +0x5F8/+0x5FC/+0x600.
//
// These have no common velocity, so each visible point is measured once per
// rendered game frame. The extra render moves those exact draw sources and puts
// them back before the sprite queue is submitted.
static const size_t th125_player_body_position = 0x5F8;
static const size_t th125_player_crosshair_position = th125_player_camera + 0x13CC;
static const size_t th125_sprite_draw_position = 0x438;
static const size_t th125_camera_sprite_objects[] = {
	0x48,
	0x504,
	0x9C0,
	0xE7C,
};
static const size_t th125_player_motion_points =
	2 + sizeof(th125_camera_sprite_objects) / sizeof(th125_camera_sprite_objects[0]);

struct Th125PlayerMotion {
	DWORD frame;
	float last[3];
	float step[3];
};

static Th125PlayerMotion th125_player_motion[th125_player_motion_points];
static unsigned char* th125_player_motion_object;
static DWORD th125_player_motion_frame = 1;
static bool th125_player_motion_ready;
static bool th125_player_motion_active;
static unsigned int th125_player_points_projected;
static unsigned int th125_player_points_projected_last;

static float* th125_player_motion_source(unsigned char* player, size_t point) {
	if (point == 0)
		return (float*)(player + th125_player_body_position);
	if (point == 1)
		return (float*)(player + th125_player_crosshair_position);
	const size_t object = th125_camera_sprite_objects[point - 2];
	return (float*)(player + th125_player_camera + object + th125_sprite_draw_position);
}

static void th125_player_measure() {
	unsigned char* player = (unsigned char*)*th125_player_manager;
	++th125_player_motion_frame;
	th125_player_motion_active = false;

	if (player == nullptr) {
		th125_player_motion_ready = false;
		th125_player_motion_object = nullptr;
		return;
	}

	const bool same_player = th125_player_motion_ready &&
		th125_player_motion_object == player;
	const bool allow_motion = same_player && !th125_objects_held();
	for (size_t point = 0; point < th125_player_motion_points; ++point) {
		Th125PlayerMotion& motion = th125_player_motion[point];
		const float* position = th125_player_motion_source(player, point);
		if (allow_motion && motion.frame + 1 == th125_player_motion_frame) {
			for (int axis = 0; axis < 3; ++axis) {
				const float delta = position[axis] - motion.last[axis];
				motion.step[axis] =
					delta > -64.0f && delta < 64.0f ? delta : 0.0f;
			}
		} else {
			for (int axis = 0; axis < 3; ++axis)
				motion.step[axis] = 0.0f;
		}
		for (int axis = 0; axis < 3; ++axis)
			motion.last[axis] = position[axis];
		motion.frame = th125_player_motion_frame;
		if (motion.step[0] != 0.0f || motion.step[1] != 0.0f ||
			motion.step[2] != 0.0f) {
			th125_player_motion_active = true;
		}
	}

	th125_player_motion_object = player;
	th125_player_motion_ready = true;
}

struct Th125ProjectedPlayerPoint {
	float* position;
	float saved[3];
};

static Th125ProjectedPlayerPoint
	th125_projected_player[th125_player_motion_points];

static void th125_player_advance(float t) {
	th125_player_points_projected = 0;
	th125_player_points_projected_last = 0;
	unsigned char* player = (unsigned char*)*th125_player_manager;
	if (!Config::InterpolatePlayerBody || !th125_player_motion_ready ||
		player == nullptr || player != th125_player_motion_object ||
		th125_objects_held()) {
		return;
	}

	for (size_t point = 0; point < th125_player_motion_points; ++point) {
		const Th125PlayerMotion& motion = th125_player_motion[point];
		if (motion.frame != th125_player_motion_frame)
			continue;
		Th125ProjectedPlayerPoint& projected =
			th125_projected_player[th125_player_points_projected++];
		projected.position = th125_player_motion_source(player, point);
		for (int axis = 0; axis < 3; ++axis) {
			projected.saved[axis] = projected.position[axis];
			projected.position[axis] += motion.step[axis] * t;
		}
	}
	th125_player_points_projected_last = th125_player_points_projected;
}

static void th125_player_undo() {
	for (unsigned int i = 0; i < th125_player_points_projected; ++i) {
		const Th125ProjectedPlayerPoint& projected = th125_projected_player[i];
		for (int axis = 0; axis < 3; ++axis)
			projected.position[axis] = projected.saved[axis];
	}
	th125_player_points_projected = 0;
}

// ---------------------------------------------------------------------------
// Frame interpolation, stage 3: straight and infinite lasers
// ---------------------------------------------------------------------------
//
// [0x4B68A4] is the 0x494-byte LaserDataInf manager allocated by sub_420170.
// Its active list head is at +0x18; each object links to the next at +0x08.
// sub_4203E0 skips status +0x0C == 1 and otherwise calls vtable +0x0C.
//
// The three concrete classes are LaserLineInf (0xFB4 bytes), LaserInfiniteInf
// (0xFD8) and LaserCurveInf (0xFF8). The first two draw from their position
// +0x50, angle +0x68 and the scales already prepared on their body Anm. Their
// update methods also advance lifetime, collision and segment generation, so
// those methods stay in the game's logic frame. Extra frames measure and move
// only the visible position, angle and scale fields.
static void** const th125_laser_manager = (void**)0x004B68A4;
static const size_t th125_laser_list_head = 0x18;
static const size_t th125_laser_next = 0x08;
static const size_t th125_laser_status = 0x0C;
static const size_t th125_laser_position = 0x50;
static const size_t th125_laser_angle = 0x68;
static const size_t th125_laser_line_scale = 0x67C;
static const size_t th125_laser_line_flags = 0xAC0;
static const size_t th125_laser_infinite_scale = 0x6A0;
static const size_t th125_laser_infinite_flags = 0xAE4;
static const size_t th125_lasers_max = 256;
static void* const th125_laser_line_vtable = (void*)0x0049EF0C;
static void* const th125_laser_infinite_vtable = (void*)0x0049EFCC;
static void* const th125_laser_curve_vtable = (void*)0x0049EF6C;

enum Th125LaserKind {
	Th125LaserNone,
	Th125LaserLine,
	Th125LaserInfinite,
	Th125LaserCurve,
};

struct Th125LaserVisual {
	float* position;
	float* angle;
	float* scale;
	DWORD* flags;
};

static Th125LaserKind th125_laser_kind(const void* object) {
	const void* vtable = *(const void**)object;
	if (vtable == th125_laser_line_vtable)
		return Th125LaserLine;
	if (vtable == th125_laser_infinite_vtable)
		return Th125LaserInfinite;
	if (vtable == th125_laser_curve_vtable)
		return Th125LaserCurve;
	return Th125LaserNone;
}

static Th125LaserVisual th125_laser_visual(void* object, Th125LaserKind kind) {
	Th125LaserVisual visual = {};
	visual.position = (float*)((char*)object + th125_laser_position);
	visual.angle = (float*)((char*)object + th125_laser_angle);
	if (kind == Th125LaserLine) {
		visual.scale = (float*)((char*)object + th125_laser_line_scale);
		visual.flags = (DWORD*)((char*)object + th125_laser_line_flags);
	} else if (kind == Th125LaserInfinite) {
		visual.scale = (float*)((char*)object + th125_laser_infinite_scale);
		visual.flags = (DWORD*)((char*)object + th125_laser_infinite_flags);
	}
	return visual;
}

struct Th125LaserMotion {
	void* object;
	Th125LaserKind kind;
	DWORD frame;
	float step_position[3];
	float step_angle;
	float step_scale[2];
	bool active;
};

struct Th125LaserSample {
	void* object;
	Th125LaserKind kind;
	float position[3];
	float angle;
	float scale[2];
};

static Th125LaserMotion th125_laser_motion[th125_lasers_max];
static size_t th125_laser_motion_count;
static void* th125_laser_motion_manager;
static DWORD th125_laser_motion_frame = 1;
static DWORD th125_laser_previous_frame;
static Th125LaserSample th125_laser_current_samples[th125_lasers_max];
static Th125LaserSample th125_laser_previous_samples[th125_lasers_max];
static size_t th125_laser_current_sample_count;
static size_t th125_laser_previous_sample_count;
static unsigned int th125_lasers_in_play;
static unsigned int th125_lasers_line_in_play;
static unsigned int th125_lasers_infinite_in_play;
static unsigned int th125_lasers_curve_ignored;
static unsigned int th125_laser_motion_matched_last;
static unsigned int th125_laser_motion_ordinal_last;
static unsigned int th125_laser_motion_new_last;
static unsigned int th125_laser_motion_zero_last;
static float th125_laser_position_delta_max;
static float th125_laser_angle_delta_max;
static float th125_laser_scale_delta_max;

struct Th125ProjectedLaser {
	Th125LaserVisual visual;
	float saved_position[3];
	float saved_angle;
	float saved_scale[2];
	DWORD saved_flags;
};

static Th125ProjectedLaser th125_projected_lasers[th125_lasers_max];
static size_t th125_projected_lasers_count;
static unsigned int th125_lasers_line_projected;
static unsigned int th125_lasers_infinite_projected;

static float th125_bounded_delta(float current, float previous, float limit) {
	const float delta = current - previous;
	if (delta != delta || delta > limit || delta < -limit)
		return 0.0f;
	return delta;
}

static float th125_angle_delta(float current, float previous) {
	const float pi = 3.14159265358979323846f;
	const float tau = 6.28318530717958647692f;
	float delta = current - previous;
	while (delta > pi)
		delta -= tau;
	while (delta < -pi)
		delta += tau;
	if (delta > 1.57079632679489661923f ||
		delta < -1.57079632679489661923f) {
		return 0.0f;
	}
	return delta;
}

static void th125_add_laser_motion(const Th125LaserSample& current,
	const Th125LaserSample& previous, bool allow_motion) {
	if (th125_laser_motion_count >= th125_lasers_max)
		return;
	Th125LaserMotion& motion =
		th125_laser_motion[th125_laser_motion_count++];
	motion = {};
	motion.object = current.object;
	motion.kind = current.kind;
	motion.frame = th125_laser_motion_frame;
	for (int axis = 0; axis < 3; ++axis) {
		const float delta = current.position[axis] - previous.position[axis];
		const float magnitude = delta < 0.0f ? -delta : delta;
		if (magnitude > th125_laser_position_delta_max)
			th125_laser_position_delta_max = magnitude;
		motion.step_position[axis] = allow_motion
			? th125_bounded_delta(
				current.position[axis], previous.position[axis], 64.0f)
			: 0.0f;
	}
	const float raw_angle_delta = current.angle - previous.angle;
	const float raw_angle_magnitude =
		raw_angle_delta < 0.0f ? -raw_angle_delta : raw_angle_delta;
	if (raw_angle_magnitude > th125_laser_angle_delta_max)
		th125_laser_angle_delta_max = raw_angle_magnitude;
	motion.step_angle = allow_motion
		? th125_angle_delta(current.angle, previous.angle)
		: 0.0f;
	for (int axis = 0; axis < 2; ++axis) {
		const float delta = current.scale[axis] - previous.scale[axis];
		const float magnitude = delta < 0.0f ? -delta : delta;
		if (magnitude > th125_laser_scale_delta_max)
			th125_laser_scale_delta_max = magnitude;
		motion.step_scale[axis] = allow_motion
			? th125_bounded_delta(current.scale[axis], previous.scale[axis], 4.0f)
			: 0.0f;
	}
	motion.active =
		motion.step_position[0] != 0.0f ||
		motion.step_position[1] != 0.0f ||
		motion.step_position[2] != 0.0f ||
		motion.step_angle != 0.0f ||
		motion.step_scale[0] != 0.0f ||
		motion.step_scale[1] != 0.0f;
	if (!motion.active)
		++th125_laser_motion_zero_last;
}

static void th125_lasers_measure() {
	++th125_laser_motion_frame;
	th125_lasers_in_play = 0;
	th125_lasers_line_in_play = 0;
	th125_lasers_infinite_in_play = 0;
	th125_lasers_curve_ignored = 0;
	th125_laser_motion_matched_last = 0;
	th125_laser_motion_ordinal_last = 0;
	th125_laser_motion_new_last = 0;
	th125_laser_motion_zero_last = 0;
	th125_laser_motion_count = 0;
	th125_laser_current_sample_count = 0;

	void* manager = *th125_laser_manager;
	if (manager != th125_laser_motion_manager) {
		th125_laser_motion_manager = manager;
		th125_laser_previous_sample_count = 0;
		th125_laser_previous_frame = 0;
	}
	if (manager == nullptr)
		return;

	const bool allow_motion = !th125_objects_held();
	void* object = *(void**)((char*)manager + th125_laser_list_head);
	for (size_t guard = 0; object != nullptr && guard < th125_lasers_max; ++guard) {
		void* next = *(void**)((char*)object + th125_laser_next);
		if (*(DWORD*)((char*)object + th125_laser_status) != 1) {
			const Th125LaserKind kind = th125_laser_kind(object);
			if (kind == Th125LaserCurve) {
				++th125_lasers_curve_ignored;
			} else if (kind != Th125LaserNone) {
				++th125_lasers_in_play;
				if (kind == Th125LaserLine)
					++th125_lasers_line_in_play;
				else
					++th125_lasers_infinite_in_play;

				Th125LaserVisual visual = th125_laser_visual(object, kind);
				Th125LaserSample& sample =
					th125_laser_current_samples[th125_laser_current_sample_count++];
				sample = {};
				sample.object = object;
				sample.kind = kind;
				for (int axis = 0; axis < 3; ++axis)
					sample.position[axis] = visual.position[axis];
				sample.angle = *visual.angle;
				for (int axis = 0; axis < 2; ++axis)
					sample.scale[axis] = visual.scale[axis];
			}
		}
		object = next;
	}

	const bool consecutive =
		th125_laser_previous_frame != 0 &&
		th125_laser_previous_frame + 1 == th125_laser_motion_frame;
	bool previous_used[th125_lasers_max] = {};
	bool current_moved[th125_lasers_max] = {};
	if (consecutive) {
		for (size_t current = 0;
			current < th125_laser_current_sample_count; ++current) {
			for (size_t previous = 0;
				previous < th125_laser_previous_sample_count; ++previous) {
				if (previous_used[previous])
					continue;
				const Th125LaserSample& old =
					th125_laser_previous_samples[previous];
				if (old.object != th125_laser_current_samples[current].object ||
					old.kind != th125_laser_current_samples[current].kind) {
					continue;
				}
				previous_used[previous] = true;
				current_moved[current] = true;
				++th125_laser_motion_matched_last;
				th125_add_laser_motion(
					th125_laser_current_samples[current], old, allow_motion);
				break;
			}
		}

		if (th125_laser_previous_sample_count ==
			th125_laser_current_sample_count) {
			size_t previous = 0;
			for (size_t current = 0;
				current < th125_laser_current_sample_count; ++current) {
				if (current_moved[current])
					continue;
				while (previous < th125_laser_previous_sample_count &&
					previous_used[previous]) {
					++previous;
				}
				if (previous >= th125_laser_previous_sample_count)
					break;
				previous_used[previous] = true;
				current_moved[current] = true;
				++th125_laser_motion_ordinal_last;
				th125_add_laser_motion(
					th125_laser_current_samples[current],
					th125_laser_previous_samples[previous], allow_motion);
			}
		}
	}
	for (size_t i = 0; i < th125_laser_current_sample_count; ++i) {
		if (!current_moved[i])
			++th125_laser_motion_new_last;
	}
	memcpy(th125_laser_previous_samples, th125_laser_current_samples,
		th125_laser_current_sample_count * sizeof(Th125LaserSample));
	th125_laser_previous_sample_count = th125_laser_current_sample_count;
	th125_laser_previous_frame = th125_laser_motion_frame;
}

static Th125LaserMotion* th125_find_laser_motion(
	void* object, Th125LaserKind kind) {
	for (size_t i = 0; i < th125_laser_motion_count; ++i) {
		Th125LaserMotion& motion = th125_laser_motion[i];
		if (motion.object == object && motion.kind == kind &&
			motion.frame == th125_laser_motion_frame) {
			return &motion;
		}
	}
	return nullptr;
}

static void th125_lasers_advance(float t) {
	th125_projected_lasers_count = 0;
	th125_lasers_line_projected = 0;
	th125_lasers_infinite_projected = 0;
	void* manager = *th125_laser_manager;
	if (!Config::InterpolateBulletsAndLasers || manager == nullptr ||
		manager != th125_laser_motion_manager || th125_objects_held()) {
		return;
	}

	void* object = *(void**)((char*)manager + th125_laser_list_head);
	for (size_t guard = 0; object != nullptr && guard < th125_lasers_max; ++guard) {
		void* next = *(void**)((char*)object + th125_laser_next);
		if (*(DWORD*)((char*)object + th125_laser_status) != 1 &&
			th125_projected_lasers_count < th125_lasers_max) {
			const Th125LaserKind kind = th125_laser_kind(object);
			if (kind == Th125LaserLine || kind == Th125LaserInfinite) {
				Th125LaserMotion* motion = th125_find_laser_motion(object, kind);
				if (motion != nullptr && motion->active) {
					Th125ProjectedLaser& projected =
						th125_projected_lasers[th125_projected_lasers_count++];
					projected.visual = th125_laser_visual(object, kind);
					for (int axis = 0; axis < 3; ++axis) {
						projected.saved_position[axis] = projected.visual.position[axis];
						projected.visual.position[axis] +=
							motion->step_position[axis] * t;
					}
					projected.saved_angle = *projected.visual.angle;
					*projected.visual.angle += motion->step_angle * t;
					for (int axis = 0; axis < 2; ++axis) {
						projected.saved_scale[axis] = projected.visual.scale[axis];
						projected.visual.scale[axis] += motion->step_scale[axis] * t;
					}
					projected.saved_flags = *projected.visual.flags;
					*projected.visual.flags |= 0x0C;
					if (kind == Th125LaserLine)
						++th125_lasers_line_projected;
					else
						++th125_lasers_infinite_projected;
				}
			}
		}
		object = next;
	}
}

static void th125_lasers_undo() {
	for (size_t i = 0; i < th125_projected_lasers_count; ++i) {
		const Th125ProjectedLaser& projected = th125_projected_lasers[i];
		for (int axis = 0; axis < 3; ++axis)
			projected.visual.position[axis] = projected.saved_position[axis];
		*projected.visual.angle = projected.saved_angle;
		for (int axis = 0; axis < 2; ++axis)
			projected.visual.scale[axis] = projected.saved_scale[axis];
		*projected.visual.flags = projected.saved_flags;
	}
	th125_projected_lasers_count = 0;
}

static D3DVIEWPORT9 th125_scene_viewport = {};
static D3DMATRIX th125_scene_world = {};
static D3DMATRIX th125_scene_view = {};
static D3DMATRIX th125_scene_projection = {};
static bool th125_scene_transforms;
static const D3DRENDERSTATETYPE th125_scene_state_types[] = {
	D3DRS_ZENABLE,
	D3DRS_ZWRITEENABLE,
	D3DRS_ALPHATESTENABLE,
	D3DRS_SRCBLEND,
	D3DRS_DESTBLEND,
	D3DRS_CULLMODE,
	D3DRS_ZFUNC,
	D3DRS_ALPHAREF,
	D3DRS_ALPHAFUNC,
	D3DRS_DITHERENABLE,
	D3DRS_ALPHABLENDENABLE,
	D3DRS_FOGENABLE,
	D3DRS_SPECULARENABLE,
	D3DRS_FOGCOLOR,
	D3DRS_FOGTABLEMODE,
	D3DRS_FOGSTART,
	D3DRS_FOGEND,
	D3DRS_FOGDENSITY,
	D3DRS_LIGHTING,
	D3DRS_FOGVERTEXMODE,
	D3DRS_CLIPPING,
	D3DRS_TEXTUREFACTOR,
	D3DRS_SCISSORTESTENABLE,
	D3DRS_COLORWRITEENABLE,
	D3DRS_BLENDOP,
	D3DRS_SEPARATEALPHABLENDENABLE,
	D3DRS_SRCBLENDALPHA,
	D3DRS_DESTBLENDALPHA,
	D3DRS_BLENDOPALPHA,
};
static const size_t th125_scene_state_count =
	sizeof(th125_scene_state_types) / sizeof(th125_scene_state_types[0]);
static DWORD th125_scene_render_states[th125_scene_state_count] = {};
static bool th125_scene_render_states_valid;
static IDirect3DStateBlock9* th125_render_start_state;
static unsigned int th125_render_state_capture_failures;
static unsigned int th125_render_state_apply_failures;
static unsigned int th125_render_state_fallbacks;
static unsigned int th125_render_state_block_applies;
static unsigned int th125_live_state_capture_failures;
static unsigned int th125_live_state_apply_failures;

static IDirect3DStateBlock9* th125_capture_device_state(IDirect3DDevice9* device) {
	IDirect3DStateBlock9* state = nullptr;
	if (device == nullptr ||
		FAILED(device->CreateStateBlock(D3DSBT_ALL, &state)) ||
		state == nullptr) {
		if (state != nullptr)
			state->Release();
		return nullptr;
	}
	if (FAILED(state->Capture())) {
		state->Release();
		return nullptr;
	}
	return state;
}

static void th125_capture_scene_render_states(IDirect3DDevice9* device) {
	th125_scene_render_states_valid = false;
	if (device == nullptr)
		return;
	for (size_t i = 0; i < th125_scene_state_count; ++i) {
		if (FAILED(device->GetRenderState(
			th125_scene_state_types[i], &th125_scene_render_states[i]))) {
			return;
		}
	}
	th125_scene_render_states_valid = true;
}

static void th125_apply_scene_render_states(IDirect3DDevice9* device) {
	if (device == nullptr || !th125_scene_render_states_valid)
		return;
	for (size_t i = 0; i < th125_scene_state_count; ++i) {
		device->SetRenderState(
			th125_scene_state_types[i], th125_scene_render_states[i]);
	}
}

static void th125_render_hook() {
	IDirect3DDevice9* device = th125_device();
	const bool runtime_ready = th125_runtime_ready();
	th125_scene_render_states_valid = false;
	if (th125_render_start_state != nullptr) {
		th125_render_start_state->Release();
		th125_render_start_state = nullptr;
	}
	if (runtime_ready && device != nullptr) {
		device->GetViewport(&th125_scene_viewport);
		th125_scene_transforms =
			SUCCEEDED(device->GetTransform(D3DTS_WORLD, &th125_scene_world)) &&
			SUCCEEDED(device->GetTransform(D3DTS_VIEW, &th125_scene_view)) &&
			SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &th125_scene_projection));
		th125_capture_scene_render_states(device);
		th125_render_start_state = th125_capture_device_state(device);
		if (th125_render_start_state == nullptr)
			++th125_render_state_capture_failures;
	}

	if (runtime_ready) {
		th125_install_background_commit_guard();
		th125_capture_draw_state(th125_render_start, th125_render_start_text,
			th125_render_start_background_vms);
		th125_capture_fixed_render_anms(th125_render_start_anms,
			th125_render_start_anm_valid, th125_render_start_anm_data);
	}
	const float shake_x =
		th125_scene_shake[0] < 0.0f ? -th125_scene_shake[0] : th125_scene_shake[0];
	const float shake_y =
		th125_scene_shake[1] < 0.0f ? -th125_scene_shake[1] : th125_scene_shake[1];
	const float shake = shake_x > shake_y ? shake_x : shake_y;
	if (shake > th125_shake_observed_max)
		th125_shake_observed_max = shake;
	if (runtime_ready) {
		th125_player_measure();
		th125_lasers_measure();
	}
	const bool ready = runtime_ready && th125_render_start.text_valid;
	if (ready)
		th125_render_identity = th125_scene_identity();
	th125_frame_ready = ready;
	++th125_render_calls;
	th125_render_objects();
}

// thprac th125 combines GUI update and drawing at the three EndScene hooks:
// THGuiUpdate() runs GameGuiBegin, UI update, GameGuiEnd and GameGuiRender. Running
// it again advances input and ImGui. As with th09.5/th14.3, replay only its cached
// ImGui draw data. The supplied C9 build has these checked entry points.
static HMODULE th125_thprac_module;
static DWORD th125_thprac_lookup_time;
static bool th125_thprac_api_checked;
static bool th125_thprac_api_ready;
static unsigned int th125_thprac_redraws;
static unsigned int th125_thprac_module_misses;
static unsigned int th125_thprac_signature_misses;
static unsigned int th125_thprac_draw_data_misses;
static unsigned int th125_thprac_backend_waits;
static IDirect3DDevice9** th125_thprac_backend_device;

static bool th125_text_contains_ci(const wchar_t* text, const wchar_t* needle) {
	for (size_t start = 0; text[start] != L'\0'; ++start) {
		size_t i = 0;
		for (; needle[i] != L'\0'; ++i) {
			wchar_t a = text[start + i];
			wchar_t b = needle[i];
			if (a == L'\0')
				return false;
			if (a >= L'A' && a <= L'Z')
				a = (wchar_t)(a - L'A' + L'a');
			if (b >= L'A' && b <= L'Z')
				b = (wchar_t)(b - L'A' + L'a');
			if (a != b)
				break;
		}
		if (needle[i] == L'\0')
			return true;
	}
	return false;
}

static HMODULE th125_find_thprac_module() {
	if (th125_thprac_module != nullptr)
		return th125_thprac_module;

	const DWORD now = GetTickCount();
	if (th125_thprac_lookup_time != 0 &&
		(LONG)(now - th125_thprac_lookup_time) < 0) {
		return nullptr;
	}
	th125_thprac_lookup_time = now + 1000;

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snapshot == INVALID_HANDLE_VALUE) {
		++th125_thprac_module_misses;
		return nullptr;
	}

	MODULEENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	if (Module32FirstW(snapshot, &entry)) {
		do {
			if (th125_text_contains_ci(entry.szModule, L"thprac")) {
				th125_thprac_module = entry.hModule;
				break;
			}
		} while (Module32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	if (th125_thprac_module == nullptr)
		++th125_thprac_module_misses;
	return th125_thprac_module;
}

static bool th125_signature_matches(const unsigned char* address,
	const unsigned char* signature, const bool* mask, size_t size) {
	for (size_t i = 0; i < size; ++i) {
		if (mask[i] && address[i] != signature[i])
			return false;
	}
	return true;
}

static const unsigned char th125_thprac_get_draw_signature[] = {
	0xA1, 0x00, 0x00, 0x00, 0x00, 0x33, 0xC9, 0x8B, 0x80,
	0xF4, 0x1C, 0x00, 0x00, 0x8B, 0x00, 0x83, 0xC0, 0x34,
	0x38, 0x08, 0x0F, 0x44, 0xC1, 0xC3,
};
static const bool th125_thprac_get_draw_mask[] = {
	true, false, false, false, false, true, true, true, true,
	true, true, true, true, true, true, true, true, true,
	true, true, true, true, true, true,
};

static const unsigned char th125_thprac_check_signature[] = {
	0x83, 0xEC, 0x28, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x33, 0xC4,
	0x89, 0x44, 0x24, 0x24, 0x56, 0x8B, 0x74, 0x24, 0x30, 0x39,
	0x35, 0x00, 0x00, 0x00, 0x00,
};
static const bool th125_thprac_check_mask[] = {
	true, true, true, true, false, false, false, false, true, true,
	true, true, true, true, true, true, true, true, true, true,
	true, false, false, false, false,
};

static const unsigned char th125_thprac_dx9_renderer_signature[] = {
	0x83, 0xEC, 0x30, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x33, 0xC4,
	0x89, 0x44, 0x24, 0x2C, 0x55, 0x8B, 0x6C, 0x24, 0x38, 0x0F,
	0x57, 0xC0, 0x0F, 0x2F, 0x45, 0x1C,
};
static const bool th125_thprac_dx9_renderer_mask[] = {
	true, true, true, true, false, false, false, false, true, true,
	true, true, true, true, true, true, true, true, true, true,
	true, true, true, true, true, true,
};

static bool th125_prepare_thprac_api(HMODULE module) {
	if (th125_thprac_api_checked)
		return th125_thprac_api_ready;
	th125_thprac_api_checked = true;

	unsigned char* base = (unsigned char*)module;
	th125_thprac_api_ready =
		th125_signature_matches(base + 0x02A4C0,
			th125_thprac_get_draw_signature, th125_thprac_get_draw_mask,
			sizeof(th125_thprac_get_draw_signature)) &&
		th125_signature_matches(base + 0x08CDB0,
			th125_thprac_check_signature, th125_thprac_check_mask,
			sizeof(th125_thprac_check_signature)) &&
		th125_signature_matches(base + 0x08D200,
			th125_thprac_dx9_renderer_signature,
			th125_thprac_dx9_renderer_mask,
			sizeof(th125_thprac_dx9_renderer_signature));
	if (!th125_thprac_api_ready)
		++th125_thprac_signature_misses;
	else
		// 0x08CDC3 is `cmp dword ptr [g_pd3dDevice], esi`.
		// Read g_pd3dDevice's address from the following four-byte operand.
		th125_thprac_backend_device =
			(IDirect3DDevice9**)*(DWORD*)(base + 0x08CDC5);
	return th125_thprac_api_ready;
}

static bool th125_draw_thprac_overlay(IDirect3DDevice9* device) {
	HMODULE module = th125_find_thprac_module();
	if (module == nullptr || !th125_prepare_thprac_api(module))
		return false;

	unsigned char* base = (unsigned char*)module;
	auto get_draw_data = (void*(*)())(base + 0x02A4C0);
	auto check_device = (void(__cdecl*)(IDirect3DDevice9*))(base + 0x08CDB0);
	auto render_draw_data = (void(__cdecl*)(void*))(base + 0x08D200);

	IDirect3DDevice9* backend_device = th125_thprac_backend_device != nullptr
		? *th125_thprac_backend_device
		: nullptr;
	if (backend_device != device) {
		++th125_thprac_backend_waits;
		return false;
	}
	void* draw_data = get_draw_data();
	if (draw_data == nullptr) {
		++th125_thprac_draw_data_misses;
		return false;
	}
	check_device(device);
	render_draw_data(draw_data);
	++th125_thprac_redraws;
	return true;
}

// sub_404170 and sub_4045E0 are draw callbacks for the background object at
// [0x4B6778], but they also update its stage-intro fields. The second callback
// decrements the countdown through sub_463E60, and the first can create a
// 30-frame fade through sub_451720. Save only the fields those callbacks write.
static void** const th125_background_object = (void**)0x004B6778;
static void** const th125_stage_object = (void**)0x004B6784;

struct Th125ExtraRange {
	size_t offset;
	size_t size;
};

static const Th125ExtraRange th125_background_ranges[] = {
	{ 0x54C,  0x08 }, // camera pointer and index
	{ 0x27B0, 0x0C }, // frame token, colour and pending update flag
	{ 0x3608, 0x24 }, // draw counters, transition flags and countdown
	{ 0x374C, 0x08 }, // background viewport placement
};
static const Th125ExtraRange th125_stage_draw_ranges[] = {
	{ 0x6F4, 0x0C }, // cursor placement written by sub_40DE00
};

static const size_t th125_background_saved_size = 0x40;
static const size_t th125_stage_draw_saved_size = 0x0C;
static unsigned char th125_background_saved[th125_background_saved_size];
static unsigned char th125_stage_draw_saved[th125_stage_draw_saved_size];
static void* th125_background_saved_object;
static void* th125_stage_draw_saved_object;
static bool th125_background_saved_valid;
static bool th125_stage_draw_saved_valid;
static unsigned int th125_background_changed_max;
static unsigned int th125_stage_draw_changed_max;

static void th125_save_ranges(unsigned char* destination, const void* object,
	const Th125ExtraRange* ranges, size_t count) {
	size_t offset = 0;
	for (size_t i = 0; i < count; ++i) {
		memcpy(destination + offset, (const char*)object + ranges[i].offset, ranges[i].size);
		offset += ranges[i].size;
	}
}

static unsigned int th125_count_range_changes(const unsigned char* saved, const void* object,
	const Th125ExtraRange* ranges, size_t count) {
	unsigned int changed = 0;
	size_t offset = 0;
	for (size_t i = 0; i < count; ++i) {
		const unsigned char* current = (const unsigned char*)object + ranges[i].offset;
		for (size_t byte = 0; byte < ranges[i].size; ++byte) {
			if (current[byte] != saved[offset + byte])
				++changed;
		}
		offset += ranges[i].size;
	}
	return changed;
}

static void th125_restore_ranges(const unsigned char* saved, void* object,
	const Th125ExtraRange* ranges, size_t count) {
	size_t offset = 0;
	for (size_t i = 0; i < count; ++i) {
		memcpy((char*)object + ranges[i].offset, saved + offset, ranges[i].size);
		offset += ranges[i].size;
	}
}

static void th125_extra_draw_state_save() {
	th125_background_saved_valid = false;
	void* background = *th125_background_object;
	if (background != nullptr) {
		th125_background_saved_object = background;
		th125_save_ranges(th125_background_saved, background,
			th125_background_ranges,
			sizeof(th125_background_ranges) / sizeof(th125_background_ranges[0]));
		th125_background_saved_valid = true;
	}

	th125_stage_draw_saved_valid = false;
	void* stage = *th125_stage_object;
	if (stage != nullptr && *(DWORD*)((char*)stage + 0x30) == 1) {
		th125_stage_draw_saved_object = stage;
		th125_save_ranges(th125_stage_draw_saved, stage,
			th125_stage_draw_ranges,
			sizeof(th125_stage_draw_ranges) / sizeof(th125_stage_draw_ranges[0]));
		th125_stage_draw_saved_valid = true;
	}
}

static void th125_extra_draw_state_restore() {
	void* background = *th125_background_object;
	if (th125_background_saved_valid && background == th125_background_saved_object) {
		const unsigned int changed = th125_count_range_changes(th125_background_saved, background,
			th125_background_ranges,
			sizeof(th125_background_ranges) / sizeof(th125_background_ranges[0]));
		if (changed > th125_background_changed_max)
			th125_background_changed_max = changed;
		th125_restore_ranges(th125_background_saved, background,
			th125_background_ranges,
			sizeof(th125_background_ranges) / sizeof(th125_background_ranges[0]));
	}
	th125_background_saved_valid = false;

	void* stage = *th125_stage_object;
	if (th125_stage_draw_saved_valid && stage == th125_stage_draw_saved_object) {
		const unsigned int changed = th125_count_range_changes(th125_stage_draw_saved, stage,
			th125_stage_draw_ranges,
			sizeof(th125_stage_draw_ranges) / sizeof(th125_stage_draw_ranges[0]));
		if (changed > th125_stage_draw_changed_max)
			th125_stage_draw_changed_max = changed;
		th125_restore_ranges(th125_stage_draw_saved, stage,
			th125_stage_draw_ranges,
			sizeof(th125_stage_draw_ranges) / sizeof(th125_stage_draw_ranges[0]));
	}
	th125_stage_draw_saved_valid = false;
}

// sub_404170 initializes bg+0x3618 after creating the stage-intro fade. While
// replaying a draw, skip the allocation so an extra frame cannot register more
// fade tasks; the fields the callback writes are restored above afterwards.
static void* (__stdcall* th125_fade_create)(int, int, int, int, int) =
	(void*(__stdcall*)(int, int, int, int, int))0x00451720;

static void* __stdcall th125_fade_create_extra_guard(
	int mode, int duration, int a3, int a4, int a5) {
	if (th125_in_extra)
		return nullptr;
	return th125_fade_create(mode, duration, a3, a4, a5);
}

static bool th125_extra_presentation(IDirect3DDevice9* device, float t) {
	if (device == nullptr || !th125_frame_ready ||
		!th125_runtime_ready() ||
		!th125_live.text_valid ||
		!th125_same_scene(th125_render_identity, th125_scene_identity())) {
		th125_frame_ready = false;
		return false;
	}

	D3DVIEWPORT9 live_viewport = {};
	device->GetViewport(&live_viewport);

	IDirect3DStateBlock9* live_device_state =
		th125_capture_device_state(device);
	if (live_device_state == nullptr)
		++th125_live_state_capture_failures;

	// Start from the same state the game's own draw callbacks saw. The text
	// manager needs more than a record count restored: its draw advances record
	// lifetimes and compacts the live records in the same pass.
	if (!th125_restore_draw_state(th125_render_start, th125_render_start_text,
		th125_render_start_background_vms)) {
		th125_frame_ready = false;
		if (live_device_state != nullptr)
			live_device_state->Release();
		return false;
	}
	if (!th125_restore_fixed_render_anms(th125_render_start_anms,
		th125_render_start_anm_valid, th125_render_start_anm_data)) {
		th125_frame_ready = false;
		if (live_device_state != nullptr)
			live_device_state->Release();
		return false;
	}

	if (FAILED(device->BeginScene())) {
		th125_restore_draw_state(th125_live, th125_live_text,
			th125_live_background_vms);
		if (live_device_state != nullptr)
			live_device_state->Release();
		if (live_viewport.Width != 0)
			device->SetViewport(&live_viewport);
		return false;
	}
	th125_reset_sprite_queue();
	th125_camera_setup();
	th125_reset_sprite_queue();
	if (th125_scene_transforms) {
		device->SetTransform(D3DTS_WORLD, &th125_scene_world);
		device->SetTransform(D3DTS_VIEW, &th125_scene_view);
		device->SetTransform(D3DTS_PROJECTION, &th125_scene_projection);
	}
	if (th125_scene_viewport.Width != 0)
		device->SetViewport(&th125_scene_viewport);
	// The game's own pass left the depth buffer populated. Replaying 3D scene
	// geometry at equal depth is otherwise rejected and presents a black frame.
	device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
	*th125_render_flag = 0xFF;
	th125_prepare_render();
	if (th125_render_start_state != nullptr &&
		SUCCEEDED(th125_render_start_state->Apply())) {
		++th125_render_state_block_applies;
	} else {
		++th125_render_state_apply_failures;
		++th125_render_state_fallbacks;
		th125_apply_scene_render_states(device);
	}
	ToolState::ReadyToDraw();
	if (Config::InterpolateBulletsAndLasers)
		th125_bullets_advance(t);
	th125_player_advance(t);
	th125_lasers_advance(t);
	th125_extra_draw_state_save();
	th125_draw_probe_collect();
	th125_in_extra = true;
	th125_render_objects();
	th125_in_extra = false;
	th125_extra_draw_state_restore();
	th125_measure_fixed_render_anm_changes();
	th125_restore_fixed_render_anms(th125_live_anms, th125_live_anm_valid,
		th125_live_anm_data);
	th125_bullets_undo();
	th125_player_undo();
	th125_lasers_undo();
	th125_measure_extra_changes();
	th125_draw_sprite_queue();
	device->SetTexture(0, nullptr);
	if (Config::THPGuiUpdateFunc != nullptr)
		Config::THPGuiUpdateFunc();
	else
		th125_draw_thprac_overlay(device);
	const HRESULT end_result = device->EndScene();
	th125_measure_queue_cache_changes();
	th125_measure_background_vm_changes();

	// Keep the text and camera state the game had after its own draw and
	// screenshot work.
	th125_restore_draw_state(th125_live, th125_live_text,
		th125_live_background_vms);

	if (live_device_state != nullptr) {
		if (FAILED(live_device_state->Apply()))
			++th125_live_state_apply_failures;
		live_device_state->Release();
	}
	if (live_viewport.Width != 0)
		device->SetViewport(&live_viewport);

	HRESULT present_result = E_FAIL;
	if (SUCCEEDED(end_result)) {
		present_result = d3d9_hooked_device() == device
			? d3d9_present_bypassing_hooks()
			: device->Present(nullptr, nullptr, nullptr, nullptr);
	}
	if (SUCCEEDED(present_result)) {
		overlay_mark_presentation();
		++th125_extra_presentations;
	}
	return SUCCEEDED(present_result);
}

static const unsigned int th125_diag_frames_max = 300;
static const unsigned int th125_diag_presents_max = 8;

struct Th125DiagFrame {
	LARGE_INTEGER base;
	unsigned int presents;
	LARGE_INTEGER present_time[th125_diag_presents_max];
};

static Th125DiagFrame th125_diag_frames[th125_diag_frames_max];
static unsigned int th125_diag_count;
static bool th125_diag_key_down;
static LARGE_INTEGER th125_perf_freq;

static void th125_diag_dump() {
	wchar_t path[1024] = {};
	if (!GetModuleFileNameW(NULL, path, MAX_PATH))
		return;
	wchar_t* slash = wcsrchr(path, L'\\');
	if (slash)
		slash[1] = L'\0';
	wcscat_s(path, L"oilp_diag.txt");

	FILE* file = nullptr;
	if (_wfopen_s(&file, path, L"w") != 0 || file == nullptr)
		return;

	unsigned int count = th125_diag_count < th125_diag_frames_max
		? th125_diag_count : th125_diag_frames_max;
	unsigned int start = th125_diag_count < th125_diag_frames_max
		? 0 : th125_diag_count % th125_diag_frames_max;

	fprintf(file, "oilp frame interpolation diagnostics\n");
	fprintf(file, "logic frame rate: %.2f, configured presentation rate: %.2f\n",
		Limiter::game_fps, Limiter::PresentRate());
	fprintf(file, "frames recorded: %u\n", count);
	fprintf(file, "draw calls: %u, present calls: %u, extra presentations: %u\n",
		th125_render_calls, th125_present_calls, th125_extra_presentations);
	unsigned char* stage = *(unsigned char**)0x004B6784;
	if (stage != nullptr) {
		fprintf(file,
			"stage state: %d, cursor %d, file %d, ecl %d/%d, flags 0x%08x\n",
			*(int*)(stage + 0x30), *(int*)(stage + 0x3C),
			*(int*)(stage + 0x114), *(int*)(stage + 0x1EC),
			*(int*)(stage + 0x1F0), *(DWORD*)(stage + 0x794));
	} else {
		fprintf(file, "stage state: no stage object\n");
	}
	unsigned char* bullet_manager = *(unsigned char**)0x004B677C;
	fprintf(file, "managers: bullet 0x%08x (%d active), ecl 0x%08x, items 0x%08x, player 0x%08x\n",
		*(DWORD*)0x004B677C, bullet_manager != nullptr ? *(int*)(bullet_manager + 0x5C) : 0,
		*(DWORD*)0x004B678C, *(DWORD*)0x004B68A0, *(DWORD*)0x004B68C8);
	fprintf(file, "scene/input: mode %d, flags 0x%08x, input 0x%08x/0x%08x/0x%08x\n",
		*(int*)0x004D1228, *(DWORD*)0x004D1260,
		*(DWORD*)0x004D8DAC, *(DWORD*)0x004D8DA8,
		*(DWORD*)0x004D8DB0);
	fprintf(file,
		"bullets: %u in play, %u projected forward, %u speed >16 excluded "
		"(th12.5 special case)\n",
		th125_bullets_in_play, th125_bullets_projected,
		th125_bullets_fast_excluded);
	unsigned char* player = (unsigned char*)*th125_player_manager;
	const int photo_state = player != nullptr
		? *(int*)(player + th125_player_camera + th125_player_camera_state)
		: -1;
	fprintf(file,
		"player: photo state %d, motion record %s, %u of %u point(s) projected; "
		"player shots not present\n",
		photo_state, th125_player_motion_active ? "active" : "idle",
		th125_player_points_projected_last, (unsigned int)th125_player_motion_points);
	fprintf(file,
		"lasers: %u straight, %u infinite in play; last extra projected "
		"%u straight and %u infinite; %u curve(s) ignored\n",
		th125_lasers_line_in_play, th125_lasers_infinite_in_play,
		th125_lasers_line_projected, th125_lasers_infinite_projected,
		th125_lasers_curve_ignored);
	fprintf(file,
		"laser motion sample: %u pointer matched, %u ordinal matched, %u new, %u zero; "
		"max |position| %.3f, |angle| %.3f, |scale| %.3f\n",
		th125_laser_motion_matched_last, th125_laser_motion_ordinal_last,
		th125_laser_motion_new_last, th125_laser_motion_zero_last,
		th125_laser_position_delta_max, th125_laser_angle_delta_max,
		th125_laser_scale_delta_max);
	fprintf(file, "object hold: %s\n", th125_objects_held() ? "yes" : "no");
	fprintf(file, "extra draw state changes: background %u, stage %u, text %u byte(s)\n",
		th125_background_changed_max, th125_stage_draw_changed_max,
		th125_text_changed_max);
	fprintf(file, "extra queue render-cache changes: %u of %u byte(s), restored\n",
		th125_queue_cache_changed_max, (unsigned int)th125_queue_cache_size);
	fprintf(file, "extra background VM changes: %u byte(s), %u VM(s), restored\n",
		th125_background_vm_changed_max, (unsigned int)th125_background_vm_count_seen);
	fprintf(file, "background surface reuse: %u capture(s), %u restore(s), %u failure(s)\n",
		th125_background_surface_captures, th125_background_surface_restores,
		th125_background_surface_failures);
	fprintf(file, "extra fixed render-Anm changes:");
	for (size_t i = 0; i < th125_fixed_render_anm_count; ++i) {
		fprintf(file, " [%u] 0x%08x=%u",
			(unsigned int)i, (DWORD)th125_render_start_anms[i],
			th125_fixed_render_anm_changed_max[i]);
	}
	fprintf(file, " byte(s), restored\n");
	fprintf(file, "screen shake observed maximum: %.3f\n", th125_shake_observed_max);
	fprintf(file, "draw-task probe, last extra presentation:\n");
	for (size_t i = 0; i < th125_draw_probe_count; ++i) {
		const Th125DrawProbeEntry& entry = th125_draw_probe[i];
		fprintf(file, "  0x%08x -> 0x%08x priority=%u entries=%u\n",
			(DWORD)entry.callback, (DWORD)entry.object,
			entry.priority, entry.calls);
	}
	fprintf(file, "state snapshot mismatches: %u\n", th125_state_mismatches);
	fprintf(file, "blocked draw-task removals during extra passes: %u\n",
		th125_draw_task_removal_guards);
	fprintf(file, "full device state: block applies=%u, fallbacks=%u, "
		"render capture failures=%u, render apply failures=%u, "
		"live capture failures=%u, live apply failures=%u\n",
		th125_render_state_block_applies, th125_render_state_fallbacks,
		th125_render_state_capture_failures,
		th125_render_state_apply_failures, th125_live_state_capture_failures,
		th125_live_state_apply_failures);
	fprintf(file, "thprac draw-only: %u redraws, %u module misses, %u signature mismatches, "
		"%u draw-data misses, %u backend waits\n", th125_thprac_redraws,
		th125_thprac_module_misses, th125_thprac_signature_misses,
		th125_thprac_draw_data_misses, th125_thprac_backend_waits);

	double presents = 0.0;
	double first = 0.0;
	double last = 0.0;
	for (unsigned int i = 0; i < count; ++i) {
		const Th125DiagFrame& frame =
			th125_diag_frames[(start + i) % th125_diag_frames_max];
		fprintf(file, "frame %4u: %u present(s)", i, frame.presents);
		for (unsigned int p = 0; p < frame.presents && p < th125_diag_presents_max; ++p) {
			fprintf(file, " [%u] %.2fms", p,
				(double)(frame.present_time[p].QuadPart - frame.base.QuadPart) * 1000.0 /
					(double)th125_perf_freq.QuadPart);
		}
		fprintf(file, "\n");
		presents += frame.presents;
		if (i == 0)
			first = (double)frame.base.QuadPart;
		last = (double)frame.base.QuadPart;
	}
	if (last > first) {
		fprintf(file, "average: %.3f presents per frame, %.1f presentations per second\n",
			presents / (double)count,
			presents * (double)th125_perf_freq.QuadPart / (last - first));
	}

	fclose(file);
	printf("Wrote oilp_diag.txt\n");
}

static HRESULT th125_present_hook() {
	IDirect3DDevice9* device = th125_device();
	if (device == nullptr)
		return E_FAIL;

	if (th125_perf_freq.QuadPart == 0)
		QueryPerformanceFrequency(&th125_perf_freq);

	LARGE_INTEGER base;
	QueryPerformanceCounter(&base);

	if (th125_frame_ready && th125_runtime_ready() &&
		th125_same_scene(th125_render_identity, th125_scene_identity())) {
		th125_capture_draw_state(th125_live, th125_live_text,
			th125_live_background_vms);
		th125_capture_fixed_render_anms(th125_live_anms,
			th125_live_anm_valid, th125_live_anm_data);
	} else {
		th125_frame_ready = false;
	}
	++th125_present_calls;

	HRESULT result = device->Present(nullptr, nullptr, nullptr, nullptr);
	overlay_mark_presentation();
	Limiter::MarkFrameWorkEnd();

	LARGE_INTEGER present_time[th125_diag_presents_max] = {};
	unsigned int presents = 1;
	present_time[0] = base;

	double rate = Limiter::PresentRate();
	if (SUCCEEDED(result) && th125_frame_ready && rate > 0.0 &&
		Limiter::game_fps > 0.0 && !Limiter::PresentingEveryNthFrame()) {
		static bool rate_printed = false;
		if (!rate_printed) {
			rate_printed = true;
			printf("Frame interpolation: %.1f presentations per second (%.2f per game frame)\n",
				rate, rate / Limiter::game_fps);
		}

		LARGE_INTEGER spacing;
		unsigned int extras = Limiter::ExtraPresentations(rate, spacing);
		LARGE_INTEGER frame_wait = Limiter::FrameWait();
		if (extras > 0 && spacing.QuadPart > 0) {
			LARGE_INTEGER frame_start = Limiter::FrameStart();
			LARGE_INTEGER frame_work = Limiter::FrameWork();
			bool has_deadline = frame_start.QuadPart > 0 && frame_work.QuadPart > 0;
			LARGE_INTEGER deadline;
			deadline.QuadPart = frame_start.QuadPart + frame_wait.QuadPart;

			for (unsigned int k = 1; k <= extras; ++k) {
				LARGE_INTEGER target;
				target.QuadPart = base.QuadPart + spacing.QuadPart * (LONGLONG)k;
				if (has_deadline && target.QuadPart + frame_work.QuadPart > deadline.QuadPart)
					continue;

				Limiter::WaitUntil(target);
				if (!th125_extra_presentation(device, (float)k / (float)(extras + 1)))
					break;
				if (presents < th125_diag_presents_max)
					QueryPerformanceCounter(&present_time[presents]);
				++presents;
			}
		}
	}
	th125_release_background_surface();

	Th125DiagFrame& frame = th125_diag_frames[th125_diag_count % th125_diag_frames_max];
	frame.base = base;
	frame.presents = presents;
	for (unsigned int p = 0; p < th125_diag_presents_max; ++p)
		frame.present_time[p] = p < presents ? present_time[p] : base;
	++th125_diag_count;
	th125_frame_ready = false;

	if (GetAsyncKeyState('U') & 0x8000) {
		if (!th125_diag_key_down) {
			th125_diag_key_down = true;
			th125_diag_dump();
		}
	} else {
		th125_diag_key_down = false;
	}

	return result;
}

static void th125_skip_present_setup(DWORD address, size_t size) {
	BYTE patch[32];
	patch[0] = 0xE9;
	*(DWORD*)(patch + 1) = (address + size) - (address + 5);
	for (size_t i = 5; i < size; ++i)
		patch[i] = 0x90;
	patch_bytes(address, patch, size);
}

int __stdcall th125_window_update_hook(void* self) {
	Limiter::Tick();
	return CWindowManager__UpdateFast(self);
}

void th125_install_patches() {
	{
		// Skip the original frame limiter
		BYTE patch[] = { 0xEB, 0x5B };
		patch_bytes(0x0044F113, patch, sizeof(patch));
	}
	{
		// Force fast input latency mode
		BYTE patch[] = { 0xEB };
		patch_bytes(0x0044E2F7, patch, sizeof(patch)); // Skip over automatic
		patch_bytes(0x0044E30B, patch, sizeof(patch)); // Skip over normal
	}
	{
		// Hook window update
		patch_call(0x0044E327, th125_window_update_hook);
	}
	if (Config::D3D9Ex) {
		// Redirect Direct3DCreate9 call
		// IAT is being hooked, but thcrap also hooks Direct3DCreate9 in the same place
		// This should force our Direct3DCreate9 hook to be loaded no matter what
		patch_call(0x0044E179, Direct3DCreate9_hook);
	}
	if (Config::ReplaySpeedControl) {
		// Skip the original replay speed control stuff
		BYTE patch[] = { 0xEB };
		patch_bytes(0x0043C86C, patch, sizeof(patch));
	}
	if (Config::Interpolation || Config::InterpolationFPS) {
		BYTE* remove_call = (BYTE*)0x00461A7C;
		if (remove_call[0] == 0xE8 &&
			(DWORD)0x00461A7C + 5 + *(DWORD*)(remove_call + 1) == 0x00461C40) {
			patch_call(0x00461A7C, (void*)th125_remove_draw_task_extra_guard);
		} else {
			printf("th125 draw-task removal call doesn't look like the known one, skipping!\n");
		}

		BYTE* fade_call = (BYTE*)0x0040439B;
		if (fade_call[0] == 0xE8 &&
			(DWORD)0x0040439B + 5 + *(DWORD*)(fade_call + 1) == 0x00451720) {
			patch_call(0x0040439B, (void*)th125_fade_create_extra_guard);
		} else {
			printf("th125 stage fade call at 0x40439b doesn't look like the known one, skipping!\n");
		}

		static const DWORD render_calls[] = { 0x0044EE1E, 0x0044F231, 0x0044F3D9 };
		unsigned int render_patched = 0;
		for (DWORD address : render_calls) {
			BYTE* site = (BYTE*)address;
			if (site[0] == 0xE8 &&
				(DWORD)address + 5 + *(DWORD*)(site + 1) == 0x004619D0) {
				patch_call(address, (void*)th125_render_hook);
				++render_patched;
			} else {
				printf("th125 draw call at 0x%x doesn't look like the known one, skipping!\n",
					address);
			}
		}

		static const DWORD present_sites[] = { 0x0044F02A, 0x0044F296, 0x0044F4B7 };
		unsigned int present_patched = 0;
		for (DWORD address : present_sites) {
			BYTE* site = (BYTE*)address;
			if (site[0] == 0xA1 && *(DWORD*)(site + 1) == 0x004D0CD8) {
				patch_call(address, (void*)th125_present_hook);
				th125_skip_present_setup(address + 5, 16);
				++present_patched;
			} else {
				printf("th125 present call at 0x%x doesn't look like the known one, skipping!\n",
					address);
			}
		}

		printf("Frame interpolation: %u draw call(s) and %u present call(s) taken over (setting %d)\n",
			render_patched, present_patched,
			Config::InterpolationFPS ? (int)Config::InterpolationFPS : Config::Interpolation);
		overlay_set_present_rate_follow_text(true);
	}
}

FPSTarget th125_replay_callback() {
	if (*CReplayManager::InstancePtr && (*CReplayManager::InstancePtr)->mode == 1) {
		// TODO: Reverse engineer this struct instead of being lazy
		auto input = *(DWORD*)0x004D8DA0;
		if (input & InputState::Focus)
			return FPSTarget::ReplaySlow;
		else if (input & (InputState::Skip | InputState::Shoot))
			return FPSTarget::ReplaySkip;
	}
	return FPSTarget::Game;
}
