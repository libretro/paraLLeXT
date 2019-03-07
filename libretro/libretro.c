#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "libretro_private.h"
#include "libretro_externs.h"

#include <libco.h>

#ifdef HAVE_LIBNX
#include <switch.h>
#endif

#include "api/m64p_frontend.h"
#include "plugin/plugin.h"
#include "api/m64p_types.h"
#include "device/r4300/r4300_core.h"
#include "device/memory/memory.h"
#include "main/main.h"
#include "api/callbacks.h"
#include "main/cheat.h"
#include "main/version.h"
#include "main/savestates.h"
#include "main/mupen64plus.ini.h"
#include "api/m64p_config.h"
#include "osal_files.h"
#include "main/rom.h"
#include "device/rcp/pi/pi_controller.h"
#include "device/pif/pif.h"
#include "libretro_memory.h"

#include "audio_plugin.h"

#if defined(HAVE_PARALLEL)
#include "../mupen64plus-video-paraLLEl/parallel.h"

static struct retro_hw_render_callback hw_render;
static struct retro_hw_render_context_negotiation_interface_vulkan hw_context_negotiation;
static const struct retro_hw_render_interface_vulkan *vulkan;
#endif

#ifndef PRESCALE_WIDTH
#define PRESCALE_WIDTH  640
#endif

#ifndef PRESCALE_HEIGHT
#define PRESCALE_HEIGHT 625
#endif

#define PATH_SIZE 2048

#define ISHEXDEC ((codeLine[cursor]>='0') && (codeLine[cursor]<='9')) || ((codeLine[cursor]>='a') && (codeLine[cursor]<='f')) || ((codeLine[cursor]>='A') && (codeLine[cursor]<='F'))

struct retro_perf_callback perf_cb;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;

retro_log_printf_t log_cb = NULL;
retro_video_refresh_t video_cb = NULL;
retro_input_poll_t poll_cb = NULL;
retro_input_state_t input_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_environment_t environ_cb = NULL;

struct retro_rumble_interface rumble;

save_memory_data saved_memory;

static cothread_t game_thread;
cothread_t retro_thread;

int astick_deadzone;
int astick_sensitivity;

static uint8_t* game_data = NULL;
static uint32_t game_size = 0;

static bool     emu_initialized = false;
static unsigned initial_boot = true;
static unsigned audio_buffer_size = 2048;
static bool vulkan_inited = false;
static bool     first_context_reset = false;
static bool     initializing = true;

static unsigned retro_filtering     = 0;
static unsigned retro_dithering     = 0;
uint32_t retro_screen_width = 320;
uint32_t retro_screen_height = 240;
float retro_screen_aspect = 4.0 / 3.0;

uint32_t CountPerOp = 0;
uint32_t CountPerScanlineOverride = 0;
uint32_t *blitter_buf = NULL;
uint32_t *blitter_buf_lock = NULL;
uint32_t screen_width = 640;
uint32_t screen_height = 480;
uint32_t screen_pitch = 0;
enum gfx_plugin_type gfx_plugin;
enum rsp_plugin_type rsp_plugin;

int rspMode = 0;

extern struct device g_dev;
extern unsigned int emumode;
extern struct cheat_ctx g_cheat_ctx;

// after the controller's CONTROL* member has been assigned we can update
// them straight from here...
extern struct
{
	CONTROL *control;
	BUTTONS buttons;
} controller[4];
// ...but it won't be at least the first time we're called, in that case set
// these instead for input_plugin to read.
int pad_pak_types[4];
int pad_present[4] = { 1, 1, 1, 1 };

static void n64DebugCallback(void* aContext, int aLevel, const char* aMessage)
{
	char buffer[1024];
	snprintf(buffer, 1024, "mupen64plus: %s\n", aMessage);
	if (log_cb)
		log_cb(RETRO_LOG_INFO, buffer);
}

extern m64p_rom_header ROM_HEADER;

static void setup_variables(void)
{
	struct retro_variable variables[] = {
		{ "mupen64plus-cpucore",
#ifdef DYNAREC
			"CPU Core; dynamic_recompiler|cached_interpreter|pure_interpreter" },
#else
			"CPU Core; cached_interpreter|pure_interpreter" },
#endif
#ifdef HAVE_PARALLEL
	{ "parallel-n64-parallel-rdp-synchronous",
	   "ParaLLEl Synchronous RDP; enabled|disabled" },
#endif
	{ "parallel-n64-gfxplugin",
	   "GFX Plugin; angrylion"
#if defined(HAVE_PARALLEL)
			"|parallel"
#endif
	},
	{ "parallel-n64-rspplugin",
	   "RSP Plugin; cxd4"
#ifdef HAVE_PARALLEL_RSP
		 "|parallel"
#endif
	},
	{ "parallel-n64-dithering",
		 "Dithering; enabled|disabled" },
	{ "parallel-n64-angrylion-vioverlay",
	"(Angrylion) VI Overlay; Filtered|Unfiltered|Depth|Coverage"
	},
	{ "parallel-n64-angrylion-multithread",
	  "(Angrylion) Multi-threading; enabled|disabled" },
	{ "parallel-n64-angrylion-overscan",
	  "(Angrylion) Hide overscan; disabled|enabled" },

	{ "parallel-n64-screensize",
	  "Resolution (restart); 640x480|960x720|1280x960|1440x1080|1600x1200|1920x1440|2240x1680|2880x2160|5760x4320|320x240" },
	{ "mupen64plus-Framerate",
		"Framerate; Original|Fullspeed" },
	{ "mupen64plus-virefresh",
		"VI Refresh (Overclock); Auto|1500|2200" },
	{ "mupen64plus-astick-deadzone",
	   "Analog Deadzone (percent); 15|20|25|30|0|5|10" },
	{ "mupen64plus-astick-sensitivity",
	   "Analog Sensitivity (percent); 100|105|110|115|120|125|130|135|140|145|150|50|55|60|65|70|75|80|85|90|95" },
	{ "mupen64plus-pak1",
	   "Player 1 Pak; memory|rumble|none" },
	{ "mupen64plus-pak2",
	   "Player 2 Pak; none|memory|rumble" },
	{ "mupen64plus-pak3",
	   "Player 3 Pak; none|memory|rumble" },
	{ "mupen64plus-pak4",
	   "Player 4 Pak; none|memory|rumble" },
	{ "mupen64plus-CountPerOp",
		"Count Per Op; 0|1|2|3" },
	{ NULL, NULL },
	};

	static const struct retro_controller_description port[] = {
		{ "Controller", RETRO_DEVICE_JOYPAD },
		{ "RetroPad", RETRO_DEVICE_JOYPAD },
	};

	static const struct retro_controller_info ports[] = {
		{ port, 2 },
		{ port, 2 },
		{ port, 2 },
		{ port, 2 },
		{ 0, 0 }
	};

	environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
	environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

static bool emu_step_load_data()
{
	m64p_error ret = CoreStartup(FRONTEND_API_VERSION, ".", ".", "Core", n64DebugCallback, 0, 0);
	if (ret && log_cb)
		log_cb(RETRO_LOG_ERROR, "mupen64plus: Failed to initialize core %i\n", ret);

	log_cb(RETRO_LOG_INFO, "EmuThread: M64CMD_ROM_OPEN\n");

	if (CoreDoCommand(M64CMD_ROM_OPEN, game_size, (void*)game_data))
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "mupen64plus: Failed to load ROM\n");
		goto load_fail;
	}

	free(game_data);
	game_data = NULL;

	log_cb(RETRO_LOG_INFO, "EmuThread: M64CMD_ROM_GET_HEADER\n");

	if (CoreDoCommand(M64CMD_ROM_GET_HEADER, sizeof(ROM_HEADER), &ROM_HEADER))
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "mupen64plus; Failed to query ROM header information\n");
		goto load_fail;
	}

	return true;

load_fail:
	free(game_data);
	game_data = NULL;
	//stop = 1;

	return false;
}

static void emu_step_initialize(void)
{
	if (emu_initialized)
		return;

	struct retro_variable gfx_var = { "parallel-n64-gfxplugin", 0 };
	struct retro_variable rsp_var = { "parallel-n64-rspplugin", 0 };
	environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &gfx_var);
	environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &rsp_var);

	if (gfx_var.value && !strcmp(gfx_var.value, "angrylion"))
		gfx_plugin = GFX_ANGRYLION;
#ifdef HAVE_PARALLEL
	if (gfx_var.value && !strcmp(gfx_var.value, "parallel") && vulkan_inited)
		gfx_plugin = GFX_PARALLEL;
#endif

	if (vulkan_inited)
	{
#if defined(HAVE_PARALLEL_RSP)
		rsp_plugin = RSP_PARALLEL;
#else
		rsp_plugin = RSP_CXD4;
#endif
	}

	if (gfx_plugin == GFX_ANGRYLION)
		rsp_plugin = RSP_CXD4;
	emu_initialized = true;
	plugin_connect_all(gfx_plugin, rsp_plugin);
}

static void EmuThreadFunction(void)
{
	log_cb(RETRO_LOG_INFO, "EmuThread: M64CMD_EXECUTE. \n");

	initializing = false;
	CoreDoCommand(M64CMD_EXECUTE, 0, NULL);
}

const char* retro_get_system_directory(void)
{
	const char* dir;
	environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir);

	return dir ? dir : ".";
}


void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }


void retro_set_environment(retro_environment_t cb)
{
	environ_cb = cb;

	setup_variables();
}

void retro_get_system_info(struct retro_system_info *info)
{
	info->library_name = "paraLLeXT";
#ifndef GIT_VERSION
#define GIT_VERSION " git"
#endif
	info->library_version = "0.1" GIT_VERSION;
	info->valid_extensions = "n64|v64|z64|bin|u1|ndd";
	info->need_fullpath = false;
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	info->geometry.base_width = screen_width;
	info->geometry.base_height = screen_height;
	info->geometry.max_width = screen_width;
	info->geometry.max_height = screen_height;
	info->geometry.aspect_ratio = 4.0 / 3.0;
	info->timing.fps = vi_expected_refresh_rate_from_tv_standard(ROM_PARAMS.systemtype);
	info->timing.sample_rate = 44100.0;
}

unsigned retro_get_region(void)
{
	return ((ROM_PARAMS.systemtype == SYSTEM_PAL) ? RETRO_REGION_PAL : RETRO_REGION_NTSC);
}

void copy_file(char * ininame, char * fileName)
{
	const char* filename = ConfigGetSharedDataFilepath(fileName);
	FILE *fp = fopen(filename, "w");
	if (fp != NULL) {
		fputs(ininame, fp);
		fclose(fp);
	}
}

void retro_init(void)
{
	char* sys_pathname;
	wchar_t w_pathname[PATH_SIZE];
	environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_pathname);
	char pathname[PATH_SIZE];
	strncpy(pathname, sys_pathname, PATH_SIZE);
	if (pathname[(strlen(pathname) - 1)] != '/' && pathname[(strlen(pathname) - 1)] != '\\')
		strcat(pathname, "/");
	strcat(pathname, "Mupen64plus/");
	mbstowcs(w_pathname, pathname, PATH_SIZE);
	if (!osal_path_existsW(w_pathname) || !osal_is_directory(w_pathname))
		osal_mkdirp(w_pathname);
	copy_file(inifile, "mupen64plus.ini");

	struct retro_log_callback log;
	unsigned colorMode = RETRO_PIXEL_FORMAT_XRGB8888;

	if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
		log_cb = log.log;
	else
		log_cb = NULL;

	if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb))
		perf_get_cpu_features_cb = perf_cb.get_cpu_features;
	else
		perf_get_cpu_features_cb = NULL;

	environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &colorMode);
	environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble);
	initializing = true;

	blitter_buf = (uint32_t*)calloc(
		PRESCALE_WIDTH * PRESCALE_HEIGHT, sizeof(uint32_t)
	);
	blitter_buf_lock = blitter_buf;

	retro_thread = co_active();
	game_thread = co_create(65536 * sizeof(void*) * 16, EmuThreadFunction);
}

void retro_deinit(void)
{
	CoreDoCommand(M64CMD_STOP, 0, NULL);
	co_switch(game_thread);

	deinit_audio_libretro();

	if (perf_cb.perf_log)
		perf_cb.perf_log();
	if(blitter_buf)
		free(blitter_buf);
}

void update_controllers()
{
	struct retro_variable pk1var = { "mupen64plus-pak1" };
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &pk1var) && pk1var.value)
	{
		int p1_pak = PLUGIN_NONE;
		if (!strcmp(pk1var.value, "rumble"))
			p1_pak = PLUGIN_RAW;
		else if (!strcmp(pk1var.value, "memory"))
			p1_pak = PLUGIN_MEMPAK;

		// If controller struct is not initialised yet, set pad_pak_types instead
		// which will be looked at when initialising the controllers.
		if (controller[0].control)
			controller[0].control->Plugin = p1_pak;
		else
			pad_pak_types[0] = p1_pak;
	}

	struct retro_variable pk2var = { "mupen64plus-pak2" };
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &pk2var) && pk2var.value)
	{
		int p2_pak = PLUGIN_NONE;
		if (!strcmp(pk2var.value, "rumble"))
			p2_pak = PLUGIN_RAW;
		else if (!strcmp(pk2var.value, "memory"))
			p2_pak = PLUGIN_MEMPAK;

		if (controller[1].control)
			controller[1].control->Plugin = p2_pak;
		else
			pad_pak_types[1] = p2_pak;
	}

	struct retro_variable pk3var = { "mupen64plus-pak3" };
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &pk3var) && pk3var.value)
	{
		int p3_pak = PLUGIN_NONE;
		if (!strcmp(pk3var.value, "rumble"))
			p3_pak = PLUGIN_RAW;
		else if (!strcmp(pk3var.value, "memory"))
			p3_pak = PLUGIN_MEMPAK;

		if (controller[2].control)
			controller[2].control->Plugin = p3_pak;
		else
			pad_pak_types[2] = p3_pak;
	}

	struct retro_variable pk4var = { "mupen64plus-pak4" };
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &pk4var) && pk4var.value)
	{
		int p4_pak = PLUGIN_NONE;
		if (!strcmp(pk4var.value, "rumble"))
			p4_pak = PLUGIN_RAW;
		else if (!strcmp(pk4var.value, "memory"))
			p4_pak = PLUGIN_MEMPAK;

		if (controller[3].control)
			controller[3].control->Plugin = p4_pak;
		else
			pad_pak_types[3] = p4_pak;
	}
}

extern void angrylion_set_vi(unsigned value);
extern void angrylion_set_filtering(unsigned value);
extern void angrylion_set_dithering(unsigned value);
extern void  angrylion_set_threads(unsigned value);
extern void parallel_set_dithering(unsigned value);
extern void  angrylion_set_threads(unsigned value);
extern void  angrylion_set_overscan(unsigned value);

static void gfx_set_filtering(void)
{
     if (log_cb)
        log_cb(RETRO_LOG_DEBUG, "set filtering mode...\n");
     switch (gfx_plugin)
     {
        case GFX_ANGRYLION:
           angrylion_set_filtering(retro_filtering);
           break;
     }
}

unsigned setting_get_dithering(void)
{
   return retro_dithering;
}

static void gfx_set_dithering(void)
{
   if (log_cb)
      log_cb(RETRO_LOG_DEBUG, "set dithering mode...\n");

   switch (gfx_plugin)
   {
      case GFX_ANGRYLION:
         angrylion_set_dithering(retro_dithering);
         break;
      case GFX_PARALLEL:
#ifdef HAVE_PARALLEL
         parallel_set_dithering(retro_dithering);
#endif
         break;
     }
}

void update_variables()
{
	struct retro_variable var;

	var.key = "mupen64plus-virefresh";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		if (!strcmp(var.value, "Auto"))
			CountPerScanlineOverride = 0;
		else
			CountPerScanlineOverride = atoi(var.value);
	}

	var.key = "mupen64plus-cpucore";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		if (!strcmp(var.value, "pure_interpreter"))
			emumode = EMUMODE_PURE_INTERPRETER;
		else if (!strcmp(var.value, "cached_interpreter"))
			emumode = EMUMODE_INTERPRETER;
		else if (!strcmp(var.value, "dynamic_recompiler"))
			emumode = EMUMODE_DYNAREC;
	}

	var.key = "mupen64plus-astick-deadzone";
	var.value = NULL;

	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		astick_deadzone = (int)(atoi(var.value) * 0.01f * 0x8000);

	var.key = "mupen64plus-astick-sensitivity";
	var.value = NULL;

	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		astick_sensitivity = atoi(var.value);

	var.key = "mupen64plus-CountPerOp";
	var.value = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		CountPerOp = atoi(var.value);
	}

	#if defined(HAVE_PARALLEL)
   var.key = "parallel-n64-parallel-rdp-synchronous";
   var.value = NULL;

   bool rdp_sync;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         rdp_sync = true;
      else
         rdp_sync = false;
   }
   else
      rdp_sync = true;
   parallel_set_synchronous_rdp(rdp_sync);
#endif

 var.key = "parallel-n64-angrylion-vioverlay";
   var.value = NULL;

   environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);

   if (var.value)
   {
      if(!strcmp(var.value, "Filtered"))
         angrylion_set_vi(0);
      else if(!strcmp(var.value, "Unfiltered"))
         angrylion_set_vi(1);
      else if(!strcmp(var.value, "Depth"))
         angrylion_set_vi(2);
      else if(!strcmp(var.value, "Coverage"))
         angrylion_set_vi(3);
   }
   else
      angrylion_set_vi(0);

   var.key = "parallel-n64-angrylion-multithread";
   var.value = NULL;

   environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);

   if (var.value)
   {
      if(!strcmp(var.value, "enabled"))
         angrylion_set_threads(0);
      else if(!strcmp(var.value, "disabled"))
         angrylion_set_threads(1);
   }
   else
      angrylion_set_threads(0);

   var.key = "parallel-n64-angrylion-overscan";
   var.value = NULL;

   environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);

   if (var.value)
   {
      if(!strcmp(var.value, "enabled"))
         angrylion_set_overscan(1);
      else if(!strcmp(var.value, "disabled"))
         angrylion_set_overscan(0);
   }
   else
      angrylion_set_overscan(0);

	   var.key = "parallel-n64-dithering";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      static signed old_dithering = -1;

      if (!strcmp(var.value, "enabled"))
         retro_dithering = 1;
      else if (!strcmp(var.value, "disabled"))
         retro_dithering = 0;

      gfx_set_dithering();

      old_dithering      = retro_dithering;
   }
   else
   {
      retro_dithering = 1;
      gfx_set_dithering();
   }




	update_controllers();
}

static void format_saved_memory(void)
{
	format_sram(saved_memory.sram);
	format_eeprom(saved_memory.eeprom, EEPROM_MAX_SIZE);
	format_flashram(saved_memory.flashram);
	format_mempak(saved_memory.mempack + 0 * MEMPAK_SIZE);
	format_mempak(saved_memory.mempack + 1 * MEMPAK_SIZE);
	format_mempak(saved_memory.mempack + 2 * MEMPAK_SIZE);
	format_mempak(saved_memory.mempack + 3 * MEMPAK_SIZE);
}


static bool retro_init_vulkan(void)
{
#if defined(HAVE_PARALLEL)
	hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
	hw_render.version_major = VK_MAKE_VERSION(1, 0, 12);
	hw_render.context_reset = context_reset;
	hw_render.context_destroy = context_destroy;

	if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "mupen64plus: libretro frontend doesn't have Vulkan support.\n");
		return false;
	}

	hw_context_negotiation.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
	hw_context_negotiation.interface_version = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;
	hw_context_negotiation.get_application_info = parallel_get_application_info;
	hw_context_negotiation.create_device = parallel_create_device;
	hw_context_negotiation.destroy_device = NULL;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, &hw_context_negotiation))
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "mupen64plus: libretro frontend doesn't have context negotiation support.\n");
	}

	return true;
#else
	return false;
#endif
}

bool retro_load_game(const struct retro_game_info *game)
{
	format_saved_memory();

	update_variables();
	initial_boot = false;

	if (gfx_plugin != GFX_ANGRYLION)
	{
		if (retro_init_vulkan())
			vulkan_inited = true;
	}

	if (vulkan_inited)
	{
		gfx_plugin = GFX_PARALLEL;
	}


	init_audio_libretro(audio_buffer_size);

	game_data = malloc(game->size);
	memcpy(game_data, game->data, game->size);
	game_size = game->size;

	if (!emu_step_load_data())
		return false;

	emu_step_initialize();

	return true;
}

#ifdef HAVE_LIBNX
extern Jit dynarec_jit;
extern void *jit_rw_buffer;
extern void *jit_old_addr;
#endif
void retro_unload_game(void)
{
#if defined(HAVE_LIBNX) && defined(DYNAREC)
	jitTransitionToWritable(&dynarec_jit);
	if (jit_old_addr != 0)
		dynarec_jit.rx_addr = jit_old_addr;
	jit_old_addr = 0;
	jitClose(&dynarec_jit);

	if (jit_rw_buffer != 0)
		free(jit_rw_buffer);

	jit_rw_buffer = 0;
#endif
	CoreDoCommand(M64CMD_ROM_CLOSE, 0, NULL);
	emu_initialized = false;
}

void retro_run(void)
{
	libretro_swap_buffer = false;
	static bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
		update_controllers();


	co_switch(game_thread);
	//if (libretro_swap_buffer)
	{
		switch (gfx_plugin)
		{
		case GFX_ANGRYLION:
			video_cb(blitter_buf_lock, screen_width, screen_height, screen_pitch);
			break;

		case GFX_PARALLEL:
#if defined(HAVE_PARALLEL)
			video_cb(parallel_frame_is_valid() ? RETRO_HW_FRAME_BUFFER_VALID : NULL,
				parallel_frame_width(), parallel_frame_height(), 0);
#endif
			break;

		default:
			video_cb((screen_pitch == 0) ? NULL : blitter_buf_lock, screen_width, screen_height, screen_pitch);
			break;
		}

	}
}

void retro_reset(void)
{
	CoreDoCommand(M64CMD_RESET, 0, (void*)0);
}

void *retro_get_memory_data(unsigned type)
{
	switch (type)
	{
	case RETRO_MEMORY_SYSTEM_RAM: return g_dev.rdram.dram;
	case RETRO_MEMORY_SAVE_RAM:   return &saved_memory;
	}

	return NULL;
}

size_t retro_get_memory_size(unsigned type)
{
	switch (type)
	{
	case RETRO_MEMORY_SYSTEM_RAM: return RDRAM_MAX_SIZE;
	case RETRO_MEMORY_SAVE_RAM:   return sizeof(saved_memory);
	}

	return 0;
}

size_t retro_serialize_size(void)
{
	return 16788288 + 1024 + 4 + 4096;
}

bool retro_serialize(void *data, size_t size)
{
	if (initializing)
		return false;

	int success = savestates_save_m64p(&g_dev, data);
	if (success)
		return true;

	return false;
}

bool retro_unserialize(const void * data, size_t size)
{
	if (initializing)
		return false;

	int success = savestates_load_m64p(&g_dev, data);
	if (success)
		return true;

	return false;
}

//Needed to be able to detach controllers for Lylat Wars multiplayer
//Only sets if controller struct is initialised as addon paks do.
void retro_set_controller_port_device(unsigned in_port, unsigned device) {
	if (in_port < 4) {
		switch (device)
		{
		case RETRO_DEVICE_NONE:
			if (controller[in_port].control) {
				controller[in_port].control->Present = 0;
				break;
			}
			else {
				pad_present[in_port] = 0;
				break;
			}

		case RETRO_DEVICE_JOYPAD:
		default:
			if (controller[in_port].control) {
				controller[in_port].control->Present = 1;
				break;
			}
			else {
				pad_present[in_port] = 1;
				break;
			}
		}
	}
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }
bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) { return false; }

void retro_cheat_reset(void)
{
	cheat_delete_all(&g_cheat_ctx);
}

void retro_cheat_set(unsigned index, bool enabled, const char* codeLine)
{
	char name[256];
	m64p_cheat_code mupenCode[256];
	int matchLength = 0, partCount = 0;
	uint32_t codeParts[256];
	int cursor;

	//Generate a name
	sprintf(name, "cheat_%u", index);

	//Break the code into Parts
	for (cursor = 0;; cursor++)
	{
		if (ISHEXDEC) {
			matchLength++;
		}
		else {
			if (matchLength) {
				char codePartS[matchLength];
				strncpy(codePartS, codeLine + cursor - matchLength, matchLength);
				codePartS[matchLength] = 0;
				codeParts[partCount++] = strtoul(codePartS, NULL, 16);
				matchLength = 0;
			}
		}
		if (!codeLine[cursor]) {
			break;
		}
	}

	//Assign the parts to mupenCode
	for (cursor = 0; 2 * cursor + 1 < partCount; cursor++) {
		mupenCode[cursor].address = codeParts[2 * cursor];
		mupenCode[cursor].value = codeParts[2 * cursor + 1];
	}

	//Assign to mupenCode
	cheat_add_new(&g_cheat_ctx, name, mupenCode, partCount / 2);
	cheat_set_enabled(&g_cheat_ctx, name, enabled);
}

void retro_return(void)
{
	libretro_swap_buffer = true;
	co_switch(retro_thread);

}

uint32_t get_retro_screen_width()
{
	return screen_width;
}

uint32_t get_retro_screen_height()
{
	return screen_height;
}

static int GamesharkActive = 0;

int event_gameshark_active(void)
{
	return GamesharkActive;
}

void event_set_gameshark(int active)
{
	// if boolean value doesn't change then just return
	if (!active == !GamesharkActive)
		return;

	// set the button state
	GamesharkActive = (active ? 1 : 0);

	// notify front-end application that gameshark button state has changed
	StateChanged(M64CORE_INPUT_GAMESHARK, GamesharkActive);
}
