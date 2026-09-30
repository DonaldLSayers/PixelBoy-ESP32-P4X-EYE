/* Game Boy emulator - see app_gbemu.h. Vendors Peanut-GB (components/
 * peanut_gb, MIT) as a single-header include, exactly once, here - that's
 * where its implementation actually gets compiled.
 *
 * Includes real Game Boy Camera cartridge (mbc == 6) support: the real
 * cartridge's sensor register interface is fed live frames from this
 * project's own gbcam capture pipeline instead of a real M64282FP sensor, so
 * a legally-dumped GB Camera ROM takes "photos" of whatever the P4's camera
 * sees, run through actual Game Boy hardware emulation. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "peanut_gb.h"

#include "app.h"
#include "app_camera.h"
#include "app_display.h"
#include "app_gbemu.h"
#include "app_input.h"
#include "app_storage.h"
#include "gbcam.h"
#include "platform.h"

static const char *TAG = "gbemu";

/* Generous fixed cart RAM size - covers every MBC1/2/3/5 cartridge (MBC5's
 * largest is 8 banks x 8KiB = 64KiB; this doubles that for headroom) rather
 * than sizing exactly from gb->num_ram_banks, which peanut-gb only knows
 * after gb_init() has already run (and already called our read/write
 * callbacks during its own checksum/header scan). Trivial next to 22MB+ of
 * free PSRAM. */
#define CART_RAM_MAX (128 * 1024)

typedef struct {
    uint8_t *rom;
    size_t rom_size;
    uint8_t *cart_ram;
    uint8_t *rgb888; /* LCD_WIDTH x LCD_HEIGHT x 3, filled one scanline at a time */
    /* A game write has landed in cart RAM since it last went to the SD card,
     * so there's a save worth re-writing (see rom_write_sav()/SAV_FLUSH_US).
     * Set only by cart_ram_write_cb() below - nothing else writing this buffer
     * means anything the save depends on. */
    bool cart_dirty;
    uint32_t rom_crc; /* identity for save states - see gb_state_hdr_t */
} gbemu_ctx_t;

static uint8_t rom_read_cb(struct gb_s *gb, const uint_fast32_t addr)
{
    gbemu_ctx_t *ctx = gb->direct.priv;
    return (addr < ctx->rom_size) ? ctx->rom[addr] : 0xFF;
}

static uint8_t cart_ram_read_cb(struct gb_s *gb, const uint_fast32_t addr)
{
    gbemu_ctx_t *ctx = gb->direct.priv;
    return (addr < CART_RAM_MAX) ? ctx->cart_ram[addr] : 0xFF;
}

static void cart_ram_write_cb(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    gbemu_ctx_t *ctx = gb->direct.priv;
    if (addr < CART_RAM_MAX) {
        ctx->cart_ram[addr] = val;
        ctx->cart_dirty = true;
    }
}

static void error_cb(struct gb_s *gb, const enum gb_error_e err, const uint16_t val)
{
    (void)gb;
    ESP_LOGE(TAG, "peanut-gb error %d at 0x%04x", (int)err, val);
}

/* ------------------------------------------------------- GB Camera cartridge
 * (mbc == 6, header type 0xFC "POCKET CAMERA") - feed the P4's own camera
 * into the cartridge's sensor register interface via this project's own
 * gbcam pipeline (components/gbcam), instead of a real M64282FP sensor.
 *
 * Register map (relative to 0xA000, matching Pan Docs): 0x00 capture control
 * (bit0 = trigger/busy), 0x01 gain/edge mode, 0x02-0x03 exposure, 0x04 edge
 * enhancement, 0x05 reference voltage, 0x06-0x35 the 48-byte 4x4x3 dither
 * matrix - byte-for-byte the same layout as gbcam_t.matrix.
 *
 * The captured image is NOT part of this register interface, even though
 * it's easy to assume so from the address range alone: real hardware (cross-
 * checked against SameBoy's implementation, since Pan Docs alone was
 * ambiguous enough here to cost real debugging time) only maps the register
 * bank when the RAM-bank-select write has bit 4 set; the 128x112 2bpp image
 * lives at 0xA100-0xAEFF of *RAM bank 0*, i.e. the same normal cart-RAM path
 * every other MBC already uses (bit 4 clear, bank 0) - so it's written
 * straight into ctx->cart_ram at GBCAM_EMU_IMG_OFFSET below, and peanut_gb's
 * existing generic cart-RAM read/write handles serving it back without any
 * camera-specific code on the read side at all.
 *
 * v1 approximation: gain/exposure/edge registers (0x01-0x05) aren't decoded -
 * gbcam's own HARDWARE-style auto-exposure loop drives cam.gain_q8 instead of
 * precisely converting the ROM's 16-bit cycle-count exposure register. Only
 * the dither matrix (0x06-0x35, what the stock ROM actually varies per shot)
 * is taken from the game. light_table is pinned to one fixed value (not
 * GBCAM_TABLE_AUTO) so gbcam_update_matrix()'s cache key never changes and
 * it never rebuilds over the matrix bytes just copied in from the ROM. */
#define GBCAM_EMU_REG_COUNT 0x36
#define GBCAM_EMU_IMG_OFFSET 0x100

static gbcam_t s_cam;
static uint8_t s_cam_regs[GBCAM_EMU_REG_COUNT];
static gbemu_ctx_t *s_gbemu_ctx; /* set by run_rom() - lets the background task below reach ctx->cart_ram */

static void cam_downsample_cb(const gbcam_frame_t *f, void *ctx)
{
    gbcam_downsample((gbcam_t *)ctx, f);
}

static bool s_cam_gain_seeded;

/* camera_grab() + the gbcam pipeline is tens of ms of real work (sensor
 * readout, downsample, dither) - running it synchronously inside the
 * register-write trap below meant every capture trigger stalled the whole
 * emulated CPU for that long, and ROMs' live-viewfinder loops re-trigger
 * often enough (every VBlank, or faster while polling the busy bit) that
 * this made the whole game visibly stutter each time. Run it on its own
 * task instead, at its own pace, and let the register write just be a fast
 * memcpy of whatever it most recently finished - same idea as the real
 * sensor running independently of the CPU polling it. */
static TaskHandle_t s_cam_task_handle;
#define GBCAM_TASK_STACK_BYTES 32768
static StaticTask_t s_cam_task_tcb;    /* small (~200B), stays internal RAM - fine */
static StackType_t *s_cam_task_stack;  /* the actual 32KB, forced into PSRAM below */
static volatile bool s_cam_task_should_run;
static volatile bool s_cam_task_stopped;

static void gbcam_emu_task(void *arg)
{
    (void)arg;
    while (s_cam_task_should_run) {
        camera_grab(cam_downsample_cb, &s_cam);

        /* gbcam_t's auto-exposure is designed to stay warmed up across a
         * whole session (as it does in the regular camera modes) - starting
         * fresh here every time a ROM is loaded means it begins from a
         * generic default gain that's usually badly wrong for the actual
         * scene, and hunts through both extremes (a second or so of solid
         * white then solid black) before converging. Skip that by seeding
         * gain_q8 from the first frame's actual average brightness instead
         * of guessing blind. */
        if (!s_cam_gain_seeded) {
            uint32_t sum = 0;
            for (int i = 0; i < GBCAM_PIXELS; i++) sum += s_cam.luma[i];
            uint32_t avg = sum / GBCAM_PIXELS;
            if (avg < 1) avg = 1;
            uint32_t g = (128u * 256u) / avg;
            if (g < 32) g = 32;
            if (g > 8192) g = 8192;
            s_cam.gain_q8 = (uint16_t)g;
            s_cam_gain_seeded = true;
        }

        memcpy(s_cam.matrix, &s_cam_regs[0x06], 48);
        uint8_t key[3] = {
            (uint8_t)s_cam.settings.dither,
            s_cam.settings.contrast,
            (uint8_t)(s_cam.settings.light_table == GBCAM_TABLE_HIGH_LIGHT),
        };
        memcpy(s_cam.matrix_key, key, sizeof key);
        s_cam.matrix_valid = true;

        gbcam_process_luma(&s_cam);
        gbcam_shades_to_tiles(s_cam.shades, s_gbemu_ctx->cart_ram + GBCAM_EMU_IMG_OFFSET);
        /* Deliberately NOT marking ctx->cart_dirty here, even though this is a
         * cart RAM write the normal callback never sees: the flag drives the
         * periodic .sav flush (SAV_FLUSH_US), and this runs every 100ms
         * forever, which would mean re-writing the whole 128KB every interval
         * for an image area that no save actually depends on. The album the
         * ROM keeps goes through cart_ram_write_cb() like any other cart RAM
         * and does set it; the raw sensor image is saved by the teardown path,
         * which writes the buffer whether or not it's dirty. */

        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_cam_task_stopped = true;
    vTaskDelete(NULL);
}

static uint8_t gb_camera_read_cb(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    if (addr < GBCAM_EMU_REG_COUNT) {
        uint8_t v = s_cam_regs[addr];
        /* Busy bit visible for exactly one read, same as real hardware -
         * some ROMs read register 0 back once right after triggering to
         * confirm the capture actually started before looping to wait for
         * it to clear, and would treat an already-0 readback as "capture
         * never started". */
        if (addr == 0x00 && (v & 0x01))
            s_cam_regs[0x00] &= ~0x01;
        return v;
    }
    return 0xFF;
}

static void gb_camera_write_cb(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    (void)gb;
    if (addr >= GBCAM_EMU_REG_COUNT) return; /* image area is read-only */
    s_cam_regs[addr] = val;
    /* No capture work happens here any more - the background task
     * (gbcam_emu_task) keeps ctx->cart_ram's image area continuously fresh
     * on its own schedule, so triggering a capture is just protocol
     * bookkeeping (the busy bit) rather than real work. */
}

static void gbcam_emu_init(void)
{
    gbcam_settings_t settings;
    gbcam_default_settings(&settings);
    settings.style = GBCAM_STYLE_HARDWARE;
    settings.auto_exposure = true;
    settings.light_table = GBCAM_TABLE_LOW_LIGHT; /* fixed - see comment above */
    /* The final output is only 128x112 either way (that's the real GB
     * Camera's fixed resolution) - the sensor itself has no lower hardware
     * mode than the 1280x720 it's already set to (sdkconfig.defaults), so
     * the actual lever here is how many samples per output pixel the
     * downscale averages (1-8, default 0 = 8 = full quality). Fewer samples
     * means less work per background-task cycle for a difference that's
     * invisible at this output size. */
    settings.max_samples = 3;
    gbcam_init(&s_cam, &settings);
    memset(s_cam_regs, 0, sizeof s_cam_regs);
    s_cam_gain_seeded = false;
    s_cam_task_should_run = true;
    s_cam_task_stopped = false;
    /* camera_grab() + gbcam's own pipeline is the same call chain that
     * needed CONFIG_ESP_MAIN_TASK_STACK_SIZE doubled to 32768 for the main
     * task (see sdkconfig.defaults) - this task runs that same chain, so it
     * needs comparable headroom, not some arbitrary smaller default.
     * Pinned to the second core (main/GB emulation defaults to core 0) so
     * the ~30ms of real work it does every cycle actually runs in parallel
     * with the game instead of still competing for the same core's time
     * slices, which is what made things feel slower rather than smoother.
     *
     * This has now silently starved twice (v1.2.1, then again after new
     * unrelated features grew internal RAM use further) because a plain
     * xTaskCreatePinnedToCore() stack always comes out of internal RAM, and
     * internal free heap on this board keeps shrinking as features get
     * added - there's 22MB+ PSRAM sitting idle instead. Static task creation
     * with the stack buffer explicitly heap_caps_malloc'd from PSRAM makes
     * this task immune to that class of regression permanently; only the
     * tiny TCB itself (a few hundred bytes) still needs internal RAM. */
    if (!s_cam_task_stack) {
        s_cam_task_stack = heap_caps_malloc(GBCAM_TASK_STACK_BYTES, MALLOC_CAP_SPIRAM);
    }
    s_cam_task_handle = NULL;
    if (s_cam_task_stack) {
        s_cam_task_handle = xTaskCreateStaticPinnedToCore(
            gbcam_emu_task, "gbcam_emu", GBCAM_TASK_STACK_BYTES, NULL,
            tskIDLE_PRIORITY + 2, s_cam_task_stack, &s_cam_task_tcb, 1);
    }
    if (!s_cam_task_handle) {
        ESP_LOGE(TAG, "gbcam_emu task creation failed - camera image will stay blank");
    }
}

/* Stops and waits for the background capture task to actually exit before
 * the caller frees ctx.cart_ram - it writes into that buffer directly. */
static void gbcam_emu_deinit(void)
{
    if (!s_cam_task_handle) return;
    s_cam_task_should_run = false;
    while (!s_cam_task_stopped) vTaskDelay(1);
    s_cam_task_handle = NULL;
}

/* Classic DMG 4-shade green palette - shade 0 lightest, 3 darkest, matching
 * gbcam's own convention (see gbcam.h) even though this isn't gbcam output. */
static const uint8_t DMG_PALETTE[4][3] = {
    {155, 188, 15},
    {139, 172, 15},
    {48, 98, 48},
    {15, 56, 15},
};

static void lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line)
{
    gbemu_ctx_t *ctx = gb->direct.priv;
    uint8_t *out = ctx->rgb888 + (size_t)line * LCD_WIDTH * 3;
    for (int x = 0; x < LCD_WIDTH; x++) {
        const uint8_t *c = DMG_PALETTE[pixels[x] & 3];
        out[x * 3 + 0] = c[0];
        out[x * 3 + 1] = c[1];
        out[x * 3 + 2] = c[2];
    }
}

/* ------------------------------------------------------------- ROM browser */

#define MAX_ROMS 128
#define MAX_SCAN_DEPTH 4

typedef struct {
    char path[300];  /* full path, for fopen() */
    char name[48];   /* filename only, for display */
} rom_entry_t;

static rom_entry_t *s_roms;
static int s_rom_count;

static bool has_gb_ext(const char *name)
{
    size_t n = strlen(name);
    if (n >= 3 && strcasecmp(name + n - 3, ".gb") == 0) return true;
    if (n >= 4 && strcasecmp(name + n - 4, ".gbc") == 0) return true;
    return false;
}

/* Recursively collects every .gb/.gbc file under dir into s_roms, depth-
 * limited (SD card directory trees here are shallow - /ROMS/PROCESSED is
 * the only subfolder in practice) and count-limited to MAX_ROMS. */
static void scan_dir(const char *dir, int depth)
{
    if (depth > MAX_SCAN_DEPTH || s_rom_count >= MAX_ROMS) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_rom_count < MAX_ROMS) {
        if (e->d_name[0] == '.') continue;
        char path[300];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        /* Not relying on dirent's d_type (unconfirmed whether this
         * toolchain's dirent.h even declares it) - just try opening it as a
         * directory instead. */
        DIR *probe = opendir(path);
        bool is_dir = probe != NULL;
        if (probe) closedir(probe);
        if (is_dir) {
            scan_dir(path, depth + 1);
        } else if (has_gb_ext(e->d_name)) {
            rom_entry_t *r = &s_roms[s_rom_count++];
            snprintf(r->path, sizeof r->path, "%s", path);
            snprintf(r->name, sizeof r->name, "%s", e->d_name);
        }
    }
    closedir(d);
}

/* Forward declarations - pull_new_photos_from_sav()/sav_path_for() are
 * defined further down (photo pulling needs GBCAM_SAV_* from this section,
 * sav_path_for() needs rom_entry_t from this one), but pick_rom()'s "PULL
 * NEW PHOTOS" row needs both. */
static int pull_new_photos_from_sav(const char *sav_path);
static void sav_path_for(const char *rom_path, int slot, char *out, size_t out_len);
static bool file_exists(const char *path);
/* Save slots are numbered 1..GBCAM_MAX_SAVE_SLOTS, contiguous - see
 * count_save_slots() further down for the "list always ends one past the
 * last real save" logic this same constant serves there. */
#define GBCAM_MAX_SAVE_SLOTS 30

/* Runs pull_new_photos_from_sav() against every save slot of every ROM under
 * /ROMS (not just whichever one's currently selected) - one button pulls
 * everything, no need to dig into each ROM individually. Shows a short
 * result screen, waits for any input, then returns to the list. */
static void pull_all_new_photos(void)
{
    display_begin_blank(0, 0, 0);
    display_text(DISP_W / 2 - display_text_width("PULLING PHOTOS...", 1) / 2, 110, 1, "PULLING PHOTOS...", 255, 255, 255);
    display_end_frame();

    int pulled = 0;
    for (int r = 0; r < s_rom_count; r++) {
        for (int slot = 1; slot <= GBCAM_MAX_SAVE_SLOTS; slot++) {
            char sav_path[300];
            sav_path_for(s_roms[r].path, slot, sav_path, sizeof sav_path);
            if (!file_exists(sav_path)) break; /* slots are contiguous, see count_save_slots() */
            int n = pull_new_photos_from_sav(sav_path);
            if (n > 0) pulled += n;
        }
    }

    display_begin_blank(0, 0, 0);
    char msg[32];
    snprintf(msg, sizeof msg, pulled == 1 ? "PULLED 1 PHOTO" : "PULLED %d PHOTOS", pulled);
    display_text(DISP_W / 2 - display_text_width(msg, 1) / 2, 110, 1, msg, pulled ? 150 : 200, 255, pulled ? 150 : 200);
    display_end_frame();
    plat_sleep_ms(1200);
}

/* Simple scrollable text list - encoder scrolls, Shutter picks, Menu click
 * cancels. Row 0 is always the synthetic "PULL NEW PHOTOS" action (see
 * pull_all_new_photos()); real ROMs start at row 1. Returns the chosen path
 * in out_path, or false if cancelled. */
static bool pick_rom(char *out_path, size_t out_len)
{
    const int row_h = 18, scale = 1;
    const int rows_visible = (DISP_H - 40) / row_h;
    const int row_count = s_rom_count + 1; /* +1 for the PULL NEW PHOTOS row */
    int sel = 0, scroll = 0;

    for (;;) {
        display_begin_blank(0, 0, 0);
        display_text(DISP_W / 2 - display_text_width("SELECT ROM", 2) / 2, 8, 2, "SELECT ROM", 255, 255, 255);
        display_rect(8, 30, DISP_W - 16, 1, 255, 255, 255);

        if (sel < scroll) scroll = sel;
        if (sel >= scroll + rows_visible) scroll = sel - rows_visible + 1;

        for (int i = 0; i < rows_visible && scroll + i < row_count; i++) {
            int idx = scroll + i;
            int y = 38 + i * row_h;
            bool is_sel = idx == sel;
            if (is_sel) display_rect(4, y - 2, DISP_W - 8, row_h - 2, 255, 255, 255);
            uint8_t c = is_sel ? 0 : 255;
            const char *name = idx == 0 ? "PULL NEW PHOTOS" : s_roms[idx - 1].name;
            display_text(8, y, scale, name, c, c, c);
        }
        display_end_frame();

        input_event_t ev;
        while (input_get(&ev, 20)) {
            if (ev.type == INPUT_ROTATE) {
                sel += ev.value;
                if (sel < 0) sel = 0;
                if (sel > row_count - 1) sel = row_count - 1;
            } else if (ev.type == INPUT_PRESS && ev.button == BTN_SHUTTER) {
                if (sel == 0) {
                    pull_all_new_photos();
                    break; /* redraw the list instead of returning */
                }
                snprintf(out_path, out_len, "%s", s_roms[sel - 1].path);
                return true;
            } else if (ev.type == INPUT_CLICK && ev.button == BTN_MENU) {
                return false;
            } else if (ev.type == INPUT_CLICK && ev.button == BTN_CAMMODE) {
                /* Unused here otherwise - same "Bottom" button that launches
                 * the emulator from the mode cycle backs straight out of it
                 * too, see CAM_MODE_EMULATOR in app.c. */
                return false;
            }
        }
    }
}

/* --------------------------------------------------------------- save slots */

/* Builds "<rom, minus its .gb/.gbc extension>.sav" for slot 1, or
 * "....2.sav" / "....3.sav" for slots 2/3 - sits right next to the ROM on
 * the SD card, same convention as every other GB emulator's battery saves. */
static void sav_path_for(const char *rom_path, int slot, char *out, size_t out_len)
{
    char base[280];
    snprintf(base, sizeof base, "%s", rom_path);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    if (slot <= 1)
        snprintf(out, out_len, "%s.sav", base);
    else
        snprintf(out, out_len, "%s.%d.sav", base, slot);
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* Defined down with the rest of the save-state code, but needed by the save
 * slot list above (it labels a slot with a suspended session in it). */
static void state_path_for(const char *rom_path, int slot, char *out, size_t out_len);

/* Pulls every saved photo out of a GB Camera .sav into the regular /GBCAM
 * gallery (same storage_save() the normal camera app uses), independent of
 * ever loading the ROM or playing back through it.
 *
 * Layout (real hardware, cross-checked against Raphael Boichot's
 * documentation of the format - github.com/Raphael-Boichot/Inject-pictures-
 * in-your-Game-Boy-Camera-saves): the 128KB save holds up to 30 photo slots,
 * each a fixed 0x1000 (4096) bytes starting at flat offset 0x2000, laid out
 * as 3584 bytes of 2bpp tile data (the exact same GBCAM_TILES_SIZE format
 * gbcam_shades_to_tiles()/_tiles_to_shades() already use) followed by a
 * 32x32 thumbnail and a metadata block.
 *
 * Every slot's metadata carries the same "Magic" structural word regardless
 * of whether a photo was ever taken there, so it can't tell used from blank
 * (confirmed the hard way: it matched all 30 slots on a save with exactly
 * one photo). The real "is this slot actually a photo" answer is the
 * 30-byte state vector at flat offset 0x11B2: one entry per on-camera album
 * position, 0xFF meaning that position is blank/erased, otherwise the
 * physical slot number (1-30, minus one) holding that photo. */
#define GBCAM_SAV_SIZE (128 * 1024)
#define GBCAM_SAV_SLOT_COUNT 30
#define GBCAM_SAV_SLOT_SIZE 0x1000
#define GBCAM_SAV_SLOT_BASE 0x2000
#define GBCAM_SAV_STATE_VECTOR_OFFSET 0x11B2

/* Idle standby for the emulator loop. The level and the timeout both come
 * from app.c's shared policy (app_backlight_percent()/app_standby_due(), see
 * app.h) rather than being repeated here, so ROW_BACKLIGHT and ROW_STANDBY
 * mean the same thing in a game as in the viewfinder - this loop never reaches
 * app_step(), which is the only other place either setting is read. Unlike
 * app_step(), standby here does NOT touch the camera: a GB Camera ROM's
 * capture task is still filling cart RAM the whole time, and STREAMOFF under
 * it is the documented black-viewfinder failure. Only the frame emulation and
 * the display push stop. */
#define EMU_STANDBY_POLL_MS 20

/* User-triggered from the ROM list's "PULL NEW PHOTOS" row (see pick_rom()),
 * not automatic on ROM exit - a physical slot number isn't a stable photo
 * identity: gb-photo's own "Clear camera roll" frees every slot for reuse,
 * so the next photo taken can land right back in a slot this .sav had
 * already given up on a previous run. A slot-number marker (this function's
 * previous approach) would then skip it as "already exported" even though
 * it's a completely different photo - confirmed on real hardware. Comparing
 * actual tile bytes against every existing GB/EMU photo's .BIN
 * (storage_has_duplicate_gb_tiles()) is slower but correct regardless of
 * slot reuse, and cheap enough run on demand (tens of small file reads). */
static int pull_new_photos_from_sav(const char *sav_path)
{
    FILE *f = fopen(sav_path, "rb");
    if (!f) return -1;

    uint8_t *buf = malloc(GBCAM_SAV_SIZE);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t got = fread(buf, 1, GBCAM_SAV_SIZE, f);
    fclose(f);
    if (got != GBCAM_SAV_SIZE) {
        free(buf);
        return -1;
    }

    static uint8_t shades[GBCAM_PIXELS];
    const uint8_t *state_vector = buf + GBCAM_SAV_STATE_VECTOR_OFFSET;
    int pulled = 0;
    for (int i = 0; i < GBCAM_SAV_SLOT_COUNT; i++) {
        uint8_t slot_num = state_vector[i];
        if (slot_num >= GBCAM_SAV_SLOT_COUNT) continue; /* 0xFF (or garbage) = blank */
        uint8_t *slot = buf + GBCAM_SAV_SLOT_BASE + (size_t)slot_num * GBCAM_SAV_SLOT_SIZE;
        if (storage_has_duplicate_gb_tiles(slot)) continue; /* already pulled this exact photo */
        gbcam_tiles_to_shades(slot, shades);
        if (storage_save(shades, GBCAM_PALETTE_DEFAULT, -1, "EMU") >= 0) pulled++;
    }
    free(buf);
    return pulled;
}

/* Every slot up to and including the first non-existent one, so the list
 * always ends with exactly one "NEW SAVE" row past however many saves
 * already exist - no fixed cap, just keeps growing as you make more. */
static int count_save_slots(const char *rom_path)
{
    int n = 1;
    char path[300];
    while (n < GBCAM_MAX_SAVE_SLOTS) {
        sav_path_for(rom_path, n, path, sizeof path);
        if (!file_exists(path)) break;
        n++;
    }
    return n; /* 1..GBCAM_MAX_SAVE_SLOTS, the last one always new/empty */
}

/* Same scrollable-list UI as pick_rom() - lists every existing save for this
 * ROM plus one "NEW SAVE" row past them. Menu falls back to slot 1 (a fresh/
 * empty cartridge if none exists yet, same as before slots existed) and
 * still plays; CamMode aborts straight back to the camera app instead,
 * without ever running the ROM - same button that launched the emulator.
 *
 * A slot with a suspended session in it reads "RESUMABLE" rather than
 * "SAVED" - the slot number is the only thing run_rom() needs to find both
 * files (it's in the state's filename as well as the .sav's), so it comes back
 * through out_slot. */
static bool pick_save_slot(const char *rom_path, char *out_sav_path, size_t out_len, int *out_slot)
{
    const int row_h = 18;
    const int rows_visible = (DISP_H - 56) / row_h; /* leaves room for the footer hint below */
    int sel = 0, scroll = 0;
    for (;;) {
        int slot_count = count_save_slots(rom_path);
        if (sel > slot_count - 1) sel = slot_count - 1;
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + rows_visible) scroll = sel - rows_visible + 1;

        display_begin_blank(0, 0, 0);
        display_text(DISP_W / 2 - display_text_width("SELECT SAVE", 2) / 2, 8, 2, "SELECT SAVE", 255, 255, 255);
        display_rect(8, 30, DISP_W - 16, 1, 255, 255, 255);

        for (int i = 0; i < rows_visible && scroll + i < slot_count; i++) {
            int idx = scroll + i;
            char path[300], state_path[300], label[48];
            sav_path_for(rom_path, idx + 1, path, sizeof path);
            state_path_for(rom_path, idx + 1, state_path, sizeof state_path);
            /* "RESUMABLE" wins over "SAVED" because it says something the .sav
             * can't: this slot has a session suspended mid-game in it (see
             * gb_state_hdr_t), which is where picking it will land. */
            const char *what = file_exists(state_path) ? "RESUMABLE" : file_exists(path) ? "SAVED" : "NEW SAVE";
            snprintf(label, sizeof label, "SLOT %d - %s", idx + 1, what);
            int y = 38 + i * row_h;
            bool is_sel = idx == sel;
            if (is_sel) display_rect(4, y - 2, DISP_W - 8, row_h - 2, 255, 255, 255);
            uint8_t c = is_sel ? 0 : 255;
            display_text(8, y, 1, label, c, c, c);
        }
        display_end_frame();

        input_event_t ev;
        while (input_get(&ev, 20)) {
            if (ev.type == INPUT_ROTATE) {
                sel += ev.value;
                if (sel < 0) sel = 0;
                if (sel > slot_count - 1) sel = slot_count - 1;
            } else if (ev.type == INPUT_PRESS && ev.button == BTN_SHUTTER) {
                sav_path_for(rom_path, sel + 1, out_sav_path, out_len);
                *out_slot = sel + 1;
                return true;
            } else if (ev.type == INPUT_CLICK && ev.button == BTN_MENU) {
                sav_path_for(rom_path, 1, out_sav_path, out_len);
                *out_slot = 1;
                return true;
            } else if (ev.type == INPUT_CLICK && ev.button == BTN_CAMMODE) {
                return false;
            }
        }
    }
}

/* -------------------------------------------------------------- save states */

#define GB_STATE_MAGIC   0x53424750u /* "PGBS" little-endian */
#define GB_STATE_VERSION 1u

/* A suspended session: everything in struct gb_s, cart RAM, and the GB Camera
 * cartridge's own register block. Written when the SLEEP timer powers the
 * device down mid-game (see run_rom()) and read back the next time that same
 * ROM and save slot are picked, so a game comes back on the frame it was left
 * on instead of at its last in-game save.
 *
 * The payload is the whole struct gb_s, which is why the header carries its
 * size: peanut_gb documents that only `direct` may be modified by a front-end,
 * so nothing outside this file is expected to know its layout - a struct that
 * changed shape has to be caught and refused, not memcpy'd into. Field
 * reordering of the same size wouldn't be caught by that check, hence
 * GB_STATE_VERSION as well: bump it by hand whenever gb_s changes.
 *
 * rom_crc is what stops a state landing in the wrong game - two ROMs collide
 * only by being byte-identical, in which case the state is valid anyway. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t gb_size;   /* sizeof(struct gb_s) at write time */
    uint32_t cart_size; /* CART_RAM_MAX at write time */
    uint32_t rom_crc;
    uint32_t rom_size;
    uint32_t cam; /* 1 = the register block below belongs to this state */
    uint8_t cam_regs[GBCAM_EMU_REG_COUNT];
    uint8_t cam_gain_seeded;
    uint8_t reserved[3]; /* keeps struct gb_s 4-byte aligned in the file */
} gb_state_hdr_t;

/* The only parts of struct gb_s a file can't carry: the front-end callbacks
 * and direct.priv. The callbacks are fixed addresses in this firmware and
 * would in fact restore correctly, but they're re-applied rather than trusted
 * so that a state can never point the emulator at a stale address, and priv is
 * this run's own ctx pointer, which differs every boot. Captured from the live
 * gb before the blob is memcpy'd over it. */
typedef struct {
    uint8_t (*rom_read)(struct gb_s *, const uint_fast32_t);
    uint8_t (*cart_ram_read)(struct gb_s *, const uint_fast32_t);
    void (*cart_ram_write)(struct gb_s *, const uint_fast32_t, const uint8_t);
    uint8_t (*camera_read)(struct gb_s *, const uint_fast32_t);
    void (*camera_write)(struct gb_s *, const uint_fast32_t, const uint8_t);
    void (*error)(struct gb_s *, const enum gb_error_e, const uint16_t);
    void (*serial_tx)(struct gb_s *, const uint8_t);
    enum gb_serial_rx_ret_e (*serial_rx)(struct gb_s *, uint8_t *);
    uint8_t (*bootrom_read)(struct gb_s *, const uint_fast16_t);
    void (*lcd_draw_line)(struct gb_s *, const uint8_t *, const uint_fast8_t);
    void *priv;
} gb_callbacks_t;

static void gb_callbacks_save(const struct gb_s *gb, gb_callbacks_t *cb)
{
    cb->rom_read = gb->gb_rom_read;
    cb->cart_ram_read = gb->gb_cart_ram_read;
    cb->cart_ram_write = gb->gb_cart_ram_write;
    cb->camera_read = gb->gb_camera_read;
    cb->camera_write = gb->gb_camera_write;
    cb->error = gb->gb_error;
    cb->serial_tx = gb->gb_serial_tx;
    cb->serial_rx = gb->gb_serial_rx;
    cb->bootrom_read = gb->gb_bootrom_read;
    cb->lcd_draw_line = gb->display.lcd_draw_line;
    cb->priv = gb->direct.priv;
}

static void gb_callbacks_apply(struct gb_s *gb, const gb_callbacks_t *cb)
{
    gb->gb_rom_read = cb->rom_read;
    gb->gb_cart_ram_read = cb->cart_ram_read;
    gb->gb_cart_ram_write = cb->cart_ram_write;
    gb->gb_camera_read = cb->camera_read;
    gb->gb_camera_write = cb->camera_write;
    gb->gb_error = cb->error;
    gb->gb_serial_tx = cb->serial_tx;
    gb->gb_serial_rx = cb->serial_rx;
    gb->gb_bootrom_read = cb->bootrom_read;
    gb->display.lcd_draw_line = cb->lcd_draw_line;
    gb->direct.priv = cb->priv;
}

/* "<rom>.state", "<rom>.2.state" - the same slot numbering as sav_path_for()
 * above, one state per save slot so picking a different slot can't resume a
 * different slot's game. */
static void state_path_for(const char *rom_path, int slot, char *out, size_t out_len)
{
    char base[280];
    snprintf(base, sizeof base, "%s", rom_path);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    if (slot <= 1)
        snprintf(out, out_len, "%s.state", base);
    else
        snprintf(out, out_len, "%s.%d.state", base, slot);
}

static bool gb_state_write(const char *path, const struct gb_s *gb, const gbemu_ctx_t *ctx)
{
    gb_state_hdr_t hdr = {
        .magic = GB_STATE_MAGIC,
        .version = GB_STATE_VERSION,
        .gb_size = sizeof(struct gb_s),
        .cart_size = CART_RAM_MAX,
        .rom_crc = ctx->rom_crc,
        .rom_size = (uint32_t)ctx->rom_size,
        .cam = gb->mbc == 6,
    };
    if (hdr.cam) {
        memcpy(hdr.cam_regs, s_cam_regs, sizeof hdr.cam_regs);
        hdr.cam_gain_seeded = s_cam_gain_seeded;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "couldn't open %s to save state", path);
        return false;
    }
    bool ok = fwrite(&hdr, sizeof hdr, 1, f) == 1;
    if (ok) ok = fwrite(gb, sizeof *gb, 1, f) == 1;
    if (ok) ok = fwrite(ctx->cart_ram, 1, CART_RAM_MAX, f) == 1;
    fclose(f);
    if (!ok) {
        /* A half-written state is worse than none: it would be read back and
         * refused (or, for a truncated struct, restore garbage). Delete it. */
        ESP_LOGE(TAG, "couldn't write %s in full - state dropped", path);
        remove(path);
        return false;
    }
    ESP_LOGW(TAG, "wrote state %s", path);
    return true;
}

/* Restores gb/ctx/s_cam_regs from `path`.
 *
 * NONE covers every "there was nothing to resume" outcome - no file, not a
 * state, a state from another ROM, or one written by a build whose gb_s
 * differs. All of those are refused *before* anything is written over, and the
 * file is deleted so it can't keep failing on every launch. CORRUPT is the one
 * case that can't be refused up front (a read that fails after the file was
 * verified whole) - gb has been partly overwritten by then, so the caller has
 * to abandon the session rather than play on or save what's left. */
typedef enum { GB_STATE_NONE = 0, GB_STATE_OK, GB_STATE_CORRUPT } gb_state_result_t;

static gb_state_result_t gb_state_read(const char *path, struct gb_s *gb, gbemu_ctx_t *ctx)
{
    FILE *f = fopen(path, "rb");
    if (!f) return GB_STATE_NONE;

    /* File length first: it's what makes a short read impossible rather than
     * merely unlikely, and so what keeps the CORRUPT case a I/O-error-only
     * path. (Verified rather than assumed because a truncated state read into
     * gb would otherwise be played as a game.) */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    const size_t need = sizeof(gb_state_hdr_t) + sizeof(struct gb_s) + CART_RAM_MAX;

    gb_state_hdr_t hdr;
    if ((size_t)fsize != need ||
        fread(&hdr, sizeof hdr, 1, f) != 1 ||
        hdr.magic != GB_STATE_MAGIC ||
        hdr.version != GB_STATE_VERSION ||
        hdr.gb_size != sizeof(struct gb_s) ||
        hdr.cart_size != CART_RAM_MAX ||
        hdr.rom_crc != ctx->rom_crc ||
        hdr.rom_size != (uint32_t)ctx->rom_size ||
        hdr.cam != (gb->mbc == 6)) {
        fclose(f);
        ESP_LOGW(TAG, "%s doesn't match this ROM/build - discarded", path);
        remove(path);
        return GB_STATE_NONE;
    }

    /* Kept aside so the blob can land on the whole struct, callbacks and all,
     * and then have exactly these put back (see gb_callbacks_t). */
    gb_callbacks_t cb;
    gb_callbacks_save(gb, &cb);

    bool ok = fread(gb, sizeof *gb, 1, f) == 1;
    if (ok) ok = fread(ctx->cart_ram, 1, CART_RAM_MAX, f) == 1;
    fclose(f);
    gb_callbacks_apply(gb, &cb);

    if (!ok) {
        ESP_LOGE(TAG, "%s read back short despite its size - state unusable", path);
        remove(path);
        return GB_STATE_CORRUPT;
    }

    if (hdr.cam) {
        memcpy(s_cam_regs, hdr.cam_regs, sizeof hdr.cam_regs);
        s_cam_gain_seeded = hdr.cam_gain_seeded;
    }
    ESP_LOGW(TAG, "resumed from %s", path);
    return GB_STATE_OK;
}

/* ------------------------------------------------------------------ run it */

/* How often cart RAM goes to the SD card while a game is running and changing
 * it. Without this a session only reached the card on a clean exit, so a
 * battery pull, a crash or a flat battery lost everything since the ROM was
 * loaded - for a game with no in-game save of its own, the whole session. A
 * minute of play is a small loss; more often costs a visible hitch each time
 * (this runs between frames, on the emulation thread, and writing 128KB is
 * tens of ms of SD traffic). */
#define SAV_FLUSH_US (60LL * 1000000LL)

/* Cart RAM out to the SD card, called on the way out and on the timer above.
 * gb->cart_ram is the header's "this cart has RAM" flag (peanut_gb's uint8_t,
 * nothing to do with ctx->cart_ram, which always exists) - a ROM without it
 * gets no .sav at all rather than a file of untouched zeros. */
static void rom_write_sav(const struct gb_s *gb, gbemu_ctx_t *ctx, const char *sav_path)
{
    if (!gb->cart_ram) return;
    ctx->cart_dirty = false; /* every writer of cart RAM sets this; the card now matches it */

    FILE *wf = fopen(sav_path, "wb");
    if (wf) {
        size_t written = fwrite(ctx->cart_ram, 1, CART_RAM_MAX, wf);
        fclose(wf);
        if (written == CART_RAM_MAX) {
            ESP_LOGW(TAG, "wrote save %s", sav_path);
        } else {
            ESP_LOGE(TAG, "short write saving %s (%u/%u bytes) - save may be corrupt",
                     sav_path, (unsigned)written, (unsigned)CART_RAM_MAX);
        }
    } else {
        ESP_LOGE(TAG, "couldn't write save %s", sav_path);
    }
}

/* Everything a ROM session owns, released in the order it has to be: the
 * background capture task first (it writes straight into ctx->cart_ram, so
 * stopping it is what makes freeing that buffer safe), then cart RAM out to
 * the SD card, then the buffers.
 *
 * Split out because there are two ways out of run_rom()'s loop - Menu-hold and
 * the SLEEP timer - and only one of them can be allowed to skip this. A deep
 * sleep is a reset: whatever wasn't written here is simply gone, and the
 * player's save with it.
 *
 * write_sav is false on the one path where the game state isn't trusted enough
 * to write anywhere (GB_STATE_CORRUPT): the .sav on the card is the last good
 * copy and stays untouched. */
static void rom_teardown(struct gb_s *gb, gbemu_ctx_t *ctx, const char *sav_path, bool write_sav)
{
    if (gb->mbc == 6) gbcam_emu_deinit();
    s_gbemu_ctx = NULL; /* the task that used it has exited (see gbcam_emu_deinit()) */

    if (write_sav) rom_write_sav(gb, ctx, sav_path);

    free(ctx->rom);
    free(ctx->cart_ram);
    free(ctx->rgb888);
}

static void run_rom(const char *rom_path, const char *sav_path, int slot)
{
    ESP_LOGW(TAG, "loading %s", rom_path);
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "couldn't open %s", rom_path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        ESP_LOGE(TAG, "empty ROM file");
        fclose(f);
        return;
    }

    gbemu_ctx_t ctx = {0};
    ctx.rom_size = (size_t)size;
    ctx.rom = malloc(ctx.rom_size);
    ctx.cart_ram = calloc(1, CART_RAM_MAX);
    ctx.rgb888 = malloc((size_t)LCD_WIDTH * LCD_HEIGHT * 3);
    if (!ctx.rom || !ctx.cart_ram || !ctx.rgb888) {
        ESP_LOGE(TAG, "out of memory");
        fclose(f);
        free(ctx.rom);
        free(ctx.cart_ram);
        free(ctx.rgb888);
        return;
    }
    size_t read = fread(ctx.rom, 1, ctx.rom_size, f);
    fclose(f);
    if (read != ctx.rom_size) {
        ESP_LOGE(TAG, "short read (%u/%u bytes)", (unsigned)read, (unsigned)ctx.rom_size);
        free(ctx.rom);
        free(ctx.cart_ram);
        free(ctx.rgb888);
        return;
    }

    /* Identity for save states (see gb_state_hdr_t) - the ROM's own bytes, not
     * its path or name, because both of those can change under a state file
     * without the game changing at all. Runs once over a few MB at 400MHz. */
    ctx.rom_crc = esp_rom_crc32_le(0, ctx.rom, (uint32_t)ctx.rom_size);

    FILE *sf = fopen(sav_path, "rb");
    if (sf) {
        size_t got = fread(ctx.cart_ram, 1, CART_RAM_MAX, sf);
        fclose(sf);
        ESP_LOGW(TAG, "loaded save %s (%u bytes)", sav_path, (unsigned)got);
    }

    s_gbemu_ctx = &ctx;

    static struct gb_s gb;
    enum gb_init_error_e err = gb_init(&gb, rom_read_cb, cart_ram_read_cb, cart_ram_write_cb, error_cb, &ctx);
    if (err != GB_INIT_NO_ERROR) {
        ESP_LOGE(TAG, "gb_init failed: %d", (int)err);
        free(ctx.rom);
        free(ctx.cart_ram);
        free(ctx.rgb888);
        return;
    }
    gb_init_lcd(&gb, lcd_draw_line_cb);

    if (gb.mbc == 6) {
        gbcam_emu_init();
        gb.gb_camera_read = gb_camera_read_cb;
        gb.gb_camera_write = gb_camera_write_cb;
        ESP_LOGW(TAG, "GB Camera cartridge detected");
    }

    char title[17];
    ESP_LOGW(TAG, "running: %s", gb_get_rom_name(&gb, title));

    /* The encoder's momentary taps cover only one axis at a time - no
     * physical D-pad, one rotational axis, four buttons, none free for a
     * second axis. Holding CamMode (the same "Bottom" button that launches
     * the emulator from the mode cycle - see CAM_MODE_EMULATOR in app.c)
     * toggles which axis the encoder sends, same idiom as Mode-button-click
     * already switching what the encoder adjusts in the normal camera app.
     * Its click still reaches the game as JOYPAD_START either way. Holding
     * Menu is the only exit now (matching the on-device gallery's own "hold
     * Menu" convention) - CamMode-hold used to exit too, but that's freed up
     * for the axis toggle instead, since Menu-hold covered exiting already. */
    bool encoder_is_updown = false;
    /* Both halves of the on-screen message: the text is chosen where the event
     * happens (the axis toggle, or a resumed session below) and shown until the
     * deadline passes, see display_osd() further down. */
    const char *osd_msg = NULL;
    int64_t osd_until_us = 0;
    int64_t left_until_us = 0, right_until_us = 0, up_until_us = 0, down_until_us = 0;
    int64_t a_until_us = 0, b_until_us = 0, select_until_us = 0;
    /* Idle handling, same shape as app_step()'s (see app_backlight_percent()
     * in app.c, which supplies the user's own backlight level). This loop
     * never returns to app_step() while a ROM is loaded, so none of that runs
     * here: without this, setting the device down mid-game would leave the
     * backlight on and a full Game Boy frame being emulated and pushed at
     * ~60Hz for as long as the ROM stayed loaded. */
    int64_t last_input_us = plat_now_us();
    /* gb_run_frame() has no real-time pacing of its own - it just advances
     * emulated game time by exactly one Game Boy frame per call, as fast as
     * it's called. Real hardware runs at DMG_CLOCK_FREQ/SCREEN_REFRESH_
     * CYCLES = ~59.73Hz; without an explicit throttle here the loop's own
     * iteration rate is what happens to govern game speed, which is exactly
     * what went wrong when the display-push skip below was added - freeing
     * up the loop to run faster also ran the whole game faster, since
     * nothing was pacing gb_run_frame() itself. */
    const int64_t frame_period_us = (int64_t)(1000000.0 * SCREEN_REFRESH_CYCLES / DMG_CLOCK_FREQ);
    int64_t next_frame_us = plat_now_us();
    /* Set by the standby block at the bottom of the loop and read by the drain
     * just below, which is why it lives out here rather than in either one -
     * see the discard there. */
    bool in_standby = false;
    /* The same rule as app.c's s_swallow_press, and the same reason this one
     * is here rather than in the standby block: a press is not one event. The
     * press itself is swallowed while in_standby is set, but its click (or the
     * long-press at 600ms) arrives after this loop has already cleared
     * in_standby and brought the backlight up - so waking the device with
     * Shutter took the photo, and waking it with Menu exited the ROM. Stays set
     * until the button comes back up. */
    bool swallow_press = false;
    bool running = true;
    bool sleeping = false; /* exits the loop into app_enter_sleep() instead of back to the launcher */

    /* Pick up where the SLEEP timer left this ROM+slot, if anything did (see
     * gb_state_hdr_t). Deliberately after gb_init()/gbcam_emu_init(): those are
     * what install the callbacks and the capture task the restored state runs
     * on top of, and the state replaces the emulator's own view of the game,
     * not the plumbing underneath it. */
    char state_path[300];
    state_path_for(rom_path, slot, state_path, sizeof state_path);
    gb_state_result_t resumed = gb_state_read(state_path, &gb, &ctx);
    if (resumed == GB_STATE_CORRUPT) {
        /* gb is part-this-game, part-whatever-the-read-got - there's nothing
         * safe to run and nothing trustworthy to save, so the .sav is left
         * exactly as it was found and the session ends here. */
        rom_teardown(&gb, &ctx, sav_path, false);
        return;
    }
    if (resumed == GB_STATE_OK) {
        osd_msg = "RESUMED";
        osd_until_us = plat_now_us() + 1500000;
    }

    int64_t next_sav_flush_us = plat_now_us() + SAV_FLUSH_US;

    while (running) {
        input_event_t ev;
        while (input_get(&ev, 0)) {
            last_input_us = plat_now_us();
            /* A press made while the screen is out is spent waking it, not
             * handed to the game: standby is entered by doing nothing, so
             * nothing pressed into it was aimed at the game, and Shutter is
             * the one that bites - it wakes the screen and takes the photo on
             * the same frame, in a game you can't see yet.
             *
             * Swallowing the whole touch, not just the press in it, is what
             * makes that true in practice: the tail of this same press (its
             * click, or the long-press at 600ms) arrives after in_standby is
             * already clear, and Menu's long-press exits the ROM - so waking
             * the game with Menu used to quit it. The next touch acts
             * normally. */
            if (in_standby || swallow_press) {
                swallow_press = true;
                continue;
            }
            if (ev.type == INPUT_LONG_PRESS && ev.button == BTN_MENU) {
                running = false;
            } else if (ev.type == INPUT_LONG_PRESS && ev.button == BTN_CAMMODE) {
                encoder_is_updown = !encoder_is_updown;
                osd_msg = encoder_is_updown ? "ENCODER: UP/DOWN" : "ENCODER: LEFT/RIGHT";
                osd_until_us = plat_now_us() + 600000;
            } else if (ev.type == INPUT_CLICK && ev.button == BTN_SHUTTER) {
                /* Momentary tap, not press/release-tracked like Mode below -
                 * a real physical tap easily spans several emulated frames
                 * (~16.7ms each) at human reaction speed, and a ROM that
                 * transitions screens on Shutter (gb-photo's "take photo" ->
                 * "Save?" prompt) can see A still logically held on the very
                 * next frame and treat it as an instant confirm, skipping the
                 * prompt entirely - confirmed on real hardware. Same fix as
                 * B/SELECT's bounded pulse below. */
                gb.direct.joypad &= ~JOYPAD_A;
                a_until_us = plat_now_us() + 150000;
            } else if (ev.type == INPUT_PRESS && ev.button == BTN_MODE) {
                gb.direct.joypad &= ~JOYPAD_START;
            } else if (ev.type == INPUT_CLICK && ev.button == BTN_MODE) {
                gb.direct.joypad |= JOYPAD_START;
            } else if (ev.type == INPUT_CLICK && ev.button == BTN_MENU) {
                /* Momentary tap, not press/release-tracked like Mode below -
                 * Menu's long-press is already reserved for exit, so a true
                 * held-B would never actually reach the game past 600ms. */
                gb.direct.joypad &= ~JOYPAD_B;
                b_until_us = plat_now_us() + 150000;
            } else if (ev.type == INPUT_CLICK && ev.button == BTN_CAMMODE) {
                /* Same reasoning as Menu/B above - CamMode's long-press is
                 * reserved for the encoder axis toggle. */
                gb.direct.joypad &= ~JOYPAD_SELECT;
                select_until_us = plat_now_us() + 150000;
            } else if (ev.type == INPUT_ROTATE) {
                int64_t now = plat_now_us();
                if (encoder_is_updown) {
                    if (ev.value > 0) {
                        gb.direct.joypad &= ~JOYPAD_DOWN;
                        down_until_us = now + 150000;
                    } else if (ev.value < 0) {
                        gb.direct.joypad &= ~JOYPAD_UP;
                        up_until_us = now + 150000;
                    }
                } else {
                    if (ev.value > 0) {
                        gb.direct.joypad &= ~JOYPAD_RIGHT;
                        right_until_us = now + 150000;
                    } else if (ev.value < 0) {
                        gb.direct.joypad &= ~JOYPAD_LEFT;
                        left_until_us = now + 150000;
                    }
                }
            }
        }
        int64_t tap_now = plat_now_us();
        if (left_until_us && tap_now > left_until_us) { gb.direct.joypad |= JOYPAD_LEFT; left_until_us = 0; }
        if (right_until_us && tap_now > right_until_us) { gb.direct.joypad |= JOYPAD_RIGHT; right_until_us = 0; }
        if (up_until_us && tap_now > up_until_us) { gb.direct.joypad |= JOYPAD_UP; up_until_us = 0; }
        if (down_until_us && tap_now > down_until_us) { gb.direct.joypad |= JOYPAD_DOWN; down_until_us = 0; }
        /* B and SELECT are momentary taps, not press/release-tracked like
         * Shutter/Mode above - Menu's and CamMode's long-presses are already
         * spoken for (exit, axis toggle), so a plain click is all either can
         * safely send. SELECT briefly had a real bug here: it used to release
         * unconditionally on this same line every iteration regardless of any
         * timer, which cleared it before gb_run_frame() ever ran and meant the
         * game never actually saw SELECT pressed at all - confirmed on real
         * hardware. */
        if (a_until_us && tap_now > a_until_us) { gb.direct.joypad |= JOYPAD_A; a_until_us = 0; }
        if (b_until_us && tap_now > b_until_us) { gb.direct.joypad |= JOYPAD_B; b_until_us = 0; }
        if (select_until_us && tap_now > select_until_us) { gb.direct.joypad |= JOYPAD_SELECT; select_until_us = 0; }

        /* Checked after the input drain and the momentary-tap timers above, so
         * a button press wakes this in the same iteration it arrives in rather
         * than one later. */
        int64_t idle_us = plat_now_us() - last_input_us;

        /* The SLEEP timer (app.c's ROW_SLEEP), which this loop used to ignore
         * entirely: app_step() is the only place that checked it, and a loaded
         * ROM never returns there, so leaving a game running meant the device
         * stayed fully awake - camera streaming, backlight on, emulating at
         * 60Hz - for as long as it was left alone, overnight included.
         *
         * Checked before the standby branch below, and by break rather than a
         * continue through it, because a SLEEP shorter than STANDBY (they're
         * independent settings) has to be reachable without ever having gone
         * through standby first. Falling out of the loop is what gets the
         * teardown - and so the save write - to happen before the power goes. */
        if (app_sleep_due(idle_us)) {
            running = false;
            sleeping = true;
            break;
        }

        bool standby = app_standby_due(idle_us);
        in_standby = standby;
        display_set_backlight(standby ? 0 : app_backlight_percent());
        /* The waking touch is over once nothing is held and nothing is still
         * queued - both, because the click that ends a press is queued by the
         * same callback that clears the button state, and checking the button
         * alone would drop the flag in the window between the two. Same rule as
         * app.c's s_swallow_press. */
        if (swallow_press && !input_any_held() && !input_pending()) swallow_press = false;
        if (standby) {
            /* Nothing to emulate and nothing to show, so skip the frame
             * entirely - both gb_run_frame() and the display push below are
             * the whole cost of this loop. The pacing block further down keeps
             * next_frame_us where it was, so its existing "behind by more than
             * a frame" resync is what wakes this back up to real time instead
             * of a burst of catch-up frames.
             *
             * The camera is deliberately left running: a GB Camera ROM's own
             * capture task (gbcam_emu_task) keeps writing fresh frames into
             * cart RAM underneath this, and pausing the ISP out from under it
             * is the exact "viewfinder goes black" failure that keeps the
             * camera unpaused for the whole emulator session (see
             * cycle_cam_mode()'s comment in app.c). */
            plat_sleep_ms(EMU_STANDBY_POLL_MS);
            continue;
        }

        gb_run_frame(&gb);
        vTaskDelay(1); /* safety net for the watchdog task's own feed */

        /* Draw every emulated frame - a prior attempt at skipping 2 of every
         * 3 display pushes (misdiagnosing tearing as a display-push-rate
         * issue) instead made fast motion look stuttery/torn between
         * updates, since game logic kept running every frame while the
         * picture only changed every 3rd - confirmed on real hardware to be
         * the actual cause once game speed was paced correctly (see
         * frame_period_us above) and the tearing turned out to be specific
         * to the emulator, not present in the other camera modes that
         * already push at a much lower, similar rate. */
        display_begin_camera(ctx.rgb888, LCD_WIDTH, LCD_HEIGHT, true);
        int64_t now = plat_now_us();
        /* Same overlay-box primitive the normal camera app's own on-screen
         * messages use (display_osd()) - drawn over the live game frame
         * already in the framebuffer, not a separate blank screen, so
         * gameplay keeps running and stays visible underneath it. */
        if (osd_msg && now < osd_until_us) display_osd(osd_msg, NULL, NULL);
        display_end_frame();

        /* Cart RAM out to the card while a game is changing it - between
         * frames, after the draw, so the write itself can't delay the picture.
         * The pacing block below resyncs afterwards, so the one frame this
         * costs shows up as a single hitch rather than the game running fast
         * to catch up. */
        if (ctx.cart_dirty && now >= next_sav_flush_us) {
            rom_write_sav(&gb, &ctx, sav_path);
            next_sav_flush_us = now + SAV_FLUSH_US;
        }

        /* Pace to real Game Boy speed - see this loop's own comment on
         * frame_period_us above. If we're badly behind (a slow draw, or
         * anything else that ate more than a frame's budget), resync to
         * now instead of trying to catch up, so a one-off hitch doesn't
         * turn into a burst of frames running silently faster afterward. */
        next_frame_us += frame_period_us;
        int64_t behind_us = now - next_frame_us;
        if (behind_us > frame_period_us) {
            next_frame_us = now;
        } else if (behind_us < 0) {
            plat_sleep_ms((uint32_t)(-behind_us / 1000));
        }
    }

    /* The capture task writes into cart RAM's image area on its own 100ms
     * schedule, so it has to be down before anything reads that buffer into a
     * file or the copy can catch it mid-write. rom_teardown() calls this again
     * below, which is a no-op by then. */
    if (gb.mbc == 6) gbcam_emu_deinit();

    /* Both of these have to run before rom_teardown(), which is what frees the
     * buffers a state is made of. Which of the two exits this was decides
     * whether there's still a session to come back to:
     *
     * Sleeping is not an exit, it's a pause - the state is written so the next
     * launch of this ROM+slot resumes on the frame the player left (see
     * gb_state_hdr_t). Menu-hold is an exit, so the state goes: coming back to
     * that ROM should start from its .sav, not silently drop the player back
     * mid-level from a session they ended on purpose. It also has to be the
     * way back to a clean start at all, since there's no on-device way to
     * delete a file by hand. */
    if (sleeping) {
        gb_state_write(state_path, &gb, &ctx);
    } else if (file_exists(state_path)) {
        remove(state_path);
    }

    rom_teardown(&gb, &ctx, sav_path, true);

    if (sleeping) {
        /* Never returns - the next power-on is a reset, which is why the save
         * had to be written above. The game's last frame is still underneath
         * the SLEEPING message app_enter_sleep() draws. */
        app_enter_sleep();
        return; /* not reached - see app.h */
    }

    ESP_LOGW(TAG, "exited");
}

void gbemu_run(void)
{
    if (!storage_ready()) {
        ESP_LOGE(TAG, "no SD card");
        return;
    }

    s_roms = calloc(MAX_ROMS, sizeof(rom_entry_t));
    if (!s_roms) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    s_rom_count = 0;
    char roms_dir[300];
    snprintf(roms_dir, sizeof roms_dir, "%s/ROMS", storage_root());
    scan_dir(roms_dir, 0);
    ESP_LOGW(TAG, "%d ROM(s) found under /ROMS", s_rom_count);

    if (s_rom_count == 0) {
        display_begin_blank(0, 0, 0);
        display_text(20, 110, 2, "NO ROMS FOUND", 255, 80, 80);
        display_text(20, 140, 1, "DROP .GB/.GBC IN /ROMS", 200, 200, 200);
        display_end_frame();
        plat_sleep_ms(2000);
        free(s_roms);
        s_roms = NULL;
        return;
    }

    char chosen[300];
    if (pick_rom(chosen, sizeof chosen)) {
        char sav_path[300];
        int slot = 1;
        if (pick_save_slot(chosen, sav_path, sizeof sav_path, &slot))
            run_rom(chosen, sav_path, slot);
    }

    free(s_roms);
    s_roms = NULL;
}
