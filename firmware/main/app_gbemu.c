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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "peanut_gb.h"

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
    if (addr < CART_RAM_MAX) ctx->cart_ram[addr] = val;
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
 * without ever running the ROM - same button that launched the emulator. */
static bool pick_save_slot(const char *rom_path, char *out_sav_path, size_t out_len)
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
            char path[300], label[48];
            sav_path_for(rom_path, idx + 1, path, sizeof path);
            bool saved = file_exists(path);
            snprintf(label, sizeof label, "SLOT %d - %s", idx + 1, saved ? "SAVED" : "NEW SAVE");
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
                return true;
            } else if (ev.type == INPUT_CLICK && ev.button == BTN_MENU) {
                sav_path_for(rom_path, 1, out_sav_path, out_len);
                return true;
            } else if (ev.type == INPUT_CLICK && ev.button == BTN_CAMMODE) {
                return false;
            }
        }
    }
}

/* ------------------------------------------------------------------ run it */

static void run_rom(const char *rom_path, const char *sav_path)
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
    int64_t osd_until_us = 0; /* non-zero while the axis-toggle message should show, see display_osd() below */
    int64_t left_until_us = 0, right_until_us = 0, up_until_us = 0, down_until_us = 0;
    int64_t a_until_us = 0, b_until_us = 0, select_until_us = 0;
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
    bool running = true;
    while (running) {
        input_event_t ev;
        while (input_get(&ev, 0)) {
            if (ev.type == INPUT_LONG_PRESS && ev.button == BTN_MENU) {
                running = false;
            } else if (ev.type == INPUT_LONG_PRESS && ev.button == BTN_CAMMODE) {
                encoder_is_updown = !encoder_is_updown;
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
        if (now < osd_until_us)
            display_osd(encoder_is_updown ? "ENCODER: UP/DOWN" : "ENCODER: LEFT/RIGHT", NULL, NULL);
        display_end_frame();

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

    if (gb.mbc == 6) gbcam_emu_deinit();

    if (gb.cart_ram) {
        FILE *wf = fopen(sav_path, "wb");
        if (wf) {
            fwrite(ctx.cart_ram, 1, CART_RAM_MAX, wf);
            fclose(wf);
            ESP_LOGW(TAG, "wrote save %s", sav_path);
        } else {
            ESP_LOGE(TAG, "couldn't write save %s", sav_path);
        }
    }

    free(ctx.rom);
    free(ctx.cart_ram);
    free(ctx.rgb888);
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
        if (pick_save_slot(chosen, sav_path, sizeof sav_path))
            run_rom(chosen, sav_path);
    }

    free(s_roms);
    s_roms = NULL;
}
