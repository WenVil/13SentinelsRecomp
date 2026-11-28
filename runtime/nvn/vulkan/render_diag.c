// Persistent, low-overhead render & shader-cache diagnostic logger (writes render_diag.log in data root).
#include "render_diag.h"

#include <SDL3/SDL_timer.h>
#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../paths.h"
#include "../../platform.h"
#include "../../rt.h"
#include "../../settings.h"
#include "../rqueue.h"
#include "record.h"
#include "vk.h"

static const char *const SK_NAMES[VK_DIAG_SK_COUNT] = {
    "no_prog", "no_stages", "prog_build", "prog_fail", "pipe_build", "pipe_fail",
    "cs_build", "cs_fail", "topology", "debug_skip", "hidden"
};

int g_vk_diag_level = -1;  // 0: off (default), 1: anomalies + 2s heartbeat, 2: verbose
#define g_diag_level g_vk_diag_level
static pthread_mutex_t g_diag_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_diag_fp;
static uint64_t g_t0_ns;
static char g_diag_path[1200];

// Cross-thread atomic counters (interval / total)
static _Atomic uint64_t a_spv_hits, a_spv_miss, a_spv_fail;
static _Atomic uint64_t a_prog_started, a_prog_ready, a_prog_fail, a_prog_skipped_total;
static _Atomic uint64_t a_pipe_ready, a_pipe_fail, a_pipe_cache_hit, a_pipe_skipped_total;
static _Atomic uint64_t a_rec_pipe_waits, a_rec_pipe_wait_ns, a_rec_draws_dropped;
static _Atomic uint64_t a_tex_late_change, a_tex_dyn_promote, a_tex_realloc, a_tex_view_evict, a_tex_view_fail;
static _Atomic uint64_t a_tex_pending_drop, a_dead_slots, a_bad_tex_binds, a_mem_events, a_swap_events;
static _Atomic int a_inflight_progs;
static _Atomic uint64_t a_max_prog_ms_x100, a_max_pipe_ms_x100;

// Render-thread per-frame / per-pass tracking
static struct {
    bool open;
    const void *rt0;
    uint32_t w, h, fmt, ncolor;
    uint32_t load_clear_mask, fresh_mask, first_write_mask, level_fresh_mask;
    bool presented_rt;
    uint32_t draws, skipped, guest_clears;
} g_pass;

static struct {
    uint64_t draws, skipped, passes, guest_clears, fresh_clears, fresh_without_guest_clear;
    uint64_t empty_cleared_passes, empty_skipped_passes;
    uint64_t zero_scissor, zero_viewport, no_write_draws, bad_tex_draws;
    uint64_t dead_slots, bad_tex_binds, tex_late_change, tex_realloc, view_evict, mem_events, swap_events;
    uint64_t sk[VK_DIAG_SK_COUNT];
    uint64_t last_spv_hits, last_spv_miss, last_prog_ready, last_pipe_ready, last_pipe_hit;
    uint64_t last_rec_waits, last_rec_wait_ns, last_rec_dropped;
} g_fr, g_hb, g_prev;

static struct {
    const void *rt0;
    uint64_t first_frame, last_frame, seen_frames, fresh_no_clear_count;
    bool warned_fresh_no_clear;
} g_rt_hist[128];
static int g_n_rt_hist;

static uint64_t g_hb_last_ns;
static double g_hb_max_gap_ms;
static uint64_t g_hb_frames;
static uint64_t g_hb_draws_sum;
static double g_roll_draws = 0.0;
static int g_roll_samples = 0;

bool vk_diag_enabled_slow(void) {
    int cur = __atomic_load_n(&g_vk_diag_level, __ATOMIC_RELAXED);
    if (cur < 0) {
        const char *e = getenv("LARECOMP_RENDER_DIAG");
        cur = e && *e ? atoi(e) : 0;
        if (cur < 0) cur = 0;
        __atomic_store_n(&g_vk_diag_level, cur, __ATOMIC_RELAXED);
    }
    return cur > 0;
}

static void atomic_max_u64(_Atomic uint64_t *dst, uint64_t val) {
    uint64_t cur = atomic_load_explicit(dst, memory_order_relaxed);
    while (val > cur && !atomic_compare_exchange_weak_explicit(dst, &cur, val, memory_order_relaxed, memory_order_relaxed)) {}
}

static void diag_write_line(bool echo_stderr, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void diag_write_line(bool echo_stderr, const char *fmt, ...) {
    if (!vk_diag_enabled()) return;
    char msg[1792];
    va_list ap;
    va_start(ap, fmt);
    int mn = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (mn < 0) return;

    double sec = g_t0_ns ? (double)(plat_now_ns() - g_t0_ns) / 1e9 : 0.0;
    char line[2048];
    int ln = snprintf(line, sizeof line, "[t=%7.3f] %s\n", sec, msg);
    if (ln <= 0) return;
    size_t len = (size_t)ln < sizeof line ? (size_t)ln : sizeof line - 1;

    pthread_mutex_lock(&g_diag_mu);
    if (g_diag_fp) {
        fwrite(line, 1, len, g_diag_fp);
        fflush(g_diag_fp);
    }
    pthread_mutex_unlock(&g_diag_mu);

    if (echo_stderr) LOG("%s", line);
}

static void count_file_lines_dir(const char *path, size_t *lines_out, uint64_t *bytes_out) {
    *lines_out = 0;
    *bytes_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz > 0) *bytes_out = (uint64_t)sz;
    fseek(f, 0, SEEK_SET);
    char buf[4096];
    size_t n;
    bool bol = true;
    char first = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (bol) { first = buf[i]; bol = false; }
            if (buf[i] == '\n') {
                if (first == 'G' || first == 'C') (*lines_out)++;
                bol = true;
            }
        }
    }
    if (!bol && (first == 'G' || first == 'C')) (*lines_out)++;
    fclose(f);
}

static size_t count_dir_entries(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

void vk_diag_init(void) {
    if (!vk_diag_enabled()) return;
    g_t0_ns = plat_now_ns();
    g_hb_last_ns = g_t0_ns;
    const char *root = rt_data_root();
    char prev[1200];
    if (root && *root) {
        snprintf(g_diag_path, sizeof g_diag_path, "%s/render_diag.log", root);
        snprintf(prev, sizeof prev, "%s/render_diag.prev.log", root);
    } else {
        snprintf(g_diag_path, sizeof g_diag_path, "render_diag.log");
        snprintf(prev, sizeof prev, "render_diag.prev.log");
    }
    pthread_mutex_lock(&g_diag_mu);
    if (g_diag_fp) { fclose(g_diag_fp); g_diag_fp = NULL; }
    remove(prev);
    rename(g_diag_path, prev);
    g_diag_fp = fopen(g_diag_path, "wb");
    pthread_mutex_unlock(&g_diag_mu);

    size_t seen_lines = 0;
    uint64_t seen_bytes = 0;
    count_file_lines_dir("cache/shader_seen.txt", &seen_lines, &seen_bytes);
    size_t spv_files = count_dir_entries("cache/spirv");
    const char *prep_env = getenv("LARECOMP_SHADER_PREP");
    const char *sync_sh = getenv("LARECOMP_SYNC_SHADERS");
    const char *log_sk = getenv("LARECOMP_LOG_SKIPS");

    diag_write_line(true,
                    "[diag-init] log=%s level=%d | gpu='%s' swap=%ux%u vsync=%d res=%d(auto=%d out=%d) up=%d(auto=%d) aa=%d | "
                    "wait_pipes=%d(rec=%d) copy_vtx=%d overlap=%d rq=%d rec=%d | lean=%d no_frame_clear=%d vids=%d | "
                    "shader_prep_env=%s sync_shaders=%s log_skips=%s | cache: shader_seen=%zu lines (%llu B), spirv_files=%zu",
                    g_diag_path, g_diag_level, g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height,
                    g_settings.vsync_mode, g_settings.res_mode, g_settings.res_auto, g_settings.output_res,
                    g_settings.upscaler, g_settings.upscaler_auto, g_settings.antialias,
                    g_settings.vk_wait_pipes, rec_wait_pipes(), g_settings.vk_copy_vertices, g_settings.vk_gpu_overlap,
                    rq_on(), rec_on(), g_vk.lean_usage, g_vk.no_frame_clear, g_vk.vids,
                    prep_env ? prep_env : "(unset)", sync_sh ? sync_sh : "(unset)", log_sk ? log_sk : "(unset)",
                    seen_lines, (unsigned long long)seen_bytes, spv_files);
}

void vk_diag_shutdown(void) {
    if (!vk_diag_enabled()) return;
    diag_write_line(false,
                    "[diag-exit] totals: spv(hit=%llu miss=%llu fail=%llu) prog(started=%llu ready=%llu fail=%llu skipped=%llu max=%.1fms) "
                    "pipe(ready=%llu hit=%llu fail=%llu skipped=%llu max=%.1fms) rec(wait=%llu/%.1fms drop=%llu) "
                    "tex(late=%llu dyn=%llu realloc=%llu view_evict=%llu view_fail=%llu dead_slot=%llu bad_bind=%llu)",
                    (unsigned long long)atomic_load(&a_spv_hits), (unsigned long long)atomic_load(&a_spv_miss),
                    (unsigned long long)atomic_load(&a_spv_fail), (unsigned long long)atomic_load(&a_prog_started),
                    (unsigned long long)atomic_load(&a_prog_ready), (unsigned long long)atomic_load(&a_prog_fail),
                    (unsigned long long)atomic_load(&a_prog_skipped_total),
                    (double)atomic_load(&a_max_prog_ms_x100) / 100.0,
                    (unsigned long long)atomic_load(&a_pipe_ready), (unsigned long long)atomic_load(&a_pipe_cache_hit),
                    (unsigned long long)atomic_load(&a_pipe_fail), (unsigned long long)atomic_load(&a_pipe_skipped_total),
                    (double)atomic_load(&a_max_pipe_ms_x100) / 100.0,
                    (unsigned long long)atomic_load(&a_rec_pipe_waits),
                    (double)atomic_load(&a_rec_pipe_wait_ns) / 1e6,
                    (unsigned long long)atomic_load(&a_rec_draws_dropped),
                    (unsigned long long)atomic_load(&a_tex_late_change),
                    (unsigned long long)atomic_load(&a_tex_dyn_promote),
                    (unsigned long long)atomic_load(&a_tex_realloc),
                    (unsigned long long)atomic_load(&a_tex_view_evict),
                    (unsigned long long)atomic_load(&a_tex_view_fail),
                    (unsigned long long)atomic_load(&a_dead_slots),
                    (unsigned long long)atomic_load(&a_bad_tex_binds));
    pthread_mutex_lock(&g_diag_mu);
    if (g_diag_fp) { fclose(g_diag_fp); g_diag_fp = NULL; }
    pthread_mutex_unlock(&g_diag_mu);
}

void vk_diag_spv_result(int stage, bool cache_hit, double ms, bool ok) {
    if (!vk_diag_enabled()) return;
    if (!ok) {
        atomic_fetch_add_explicit(&a_spv_fail, 1, memory_order_relaxed);
        diag_write_line(true, "[diag-spv] FAIL stage=%d ms=%.2f", stage, ms);
        return;
    }
    if (cache_hit) atomic_fetch_add_explicit(&a_spv_hits, 1, memory_order_relaxed);
    else atomic_fetch_add_explicit(&a_spv_miss, 1, memory_order_relaxed);
    if (g_diag_level >= 2 || (!cache_hit && ms >= 15.0)) {
        diag_write_line(false, "[diag-spv] stage=%d %s ms=%.2f", stage, cache_hit ? "cache_hit" : "compiled", ms);
    }
}

void vk_diag_prog_start(uint64_t code0, uint64_t code1, uint64_t vh, uint64_t fh, bool prep) {
    if (!vk_diag_enabled() || prep) return;
    atomic_fetch_add_explicit(&a_prog_started, 1, memory_order_relaxed);
    int inflight = atomic_fetch_add_explicit(&a_inflight_progs, 1, memory_order_relaxed) + 1;
    if (g_diag_level >= 2 || inflight >= 1) {
        diag_write_line(false, "[diag-prog-start] code=%llx/%llx hash=%016llx/%016llx inflight=%d",
                        (unsigned long long)code0, (unsigned long long)code1,
                        (unsigned long long)vh, (unsigned long long)fh, inflight);
    }
}

void vk_diag_prog_done(uint64_t code0, uint64_t code1, uint64_t vh, uint64_t fh,
                       double q_tr_ms, double tr_ms, double q_spv_ms, double spv_ms, double total_ms,
                       uint32_t skipped_draws, uint64_t first_frame, uint64_t last_frame, int state) {
    if (!vk_diag_enabled()) return;
    atomic_fetch_sub_explicit(&a_inflight_progs, 1, memory_order_relaxed);
    if (state == 1) atomic_fetch_add_explicit(&a_prog_ready, 1, memory_order_relaxed);
    else atomic_fetch_add_explicit(&a_prog_fail, 1, memory_order_relaxed);
    if (skipped_draws) atomic_fetch_add_explicit(&a_prog_skipped_total, skipped_draws, memory_order_relaxed);
    atomic_max_u64(&a_max_prog_ms_x100, (uint64_t)(total_ms * 100.0));
    if (skipped_draws > 0 || state != 1 || total_ms >= 5.0 || g_diag_level >= 2) {
        diag_write_line(skipped_draws > 0 || state != 1 || total_ms >= 50.0,
                        "[diag-prog-done] state=%s code=%llx/%llx hash=%016llx/%016llx "
                        "skipped_draws=%u frames=%llu..%llu | total=%.1fms (q_tr=%.1f tr=%.1f q_spv=%.1f spv+mod=%.1f)",
                        state == 1 ? "READY" : "FAILED",
                        (unsigned long long)code0, (unsigned long long)code1,
                        (unsigned long long)vh, (unsigned long long)fh,
                        skipped_draws, (unsigned long long)first_frame, (unsigned long long)last_frame,
                        total_ms, q_tr_ms, tr_ms, q_spv_ms, spv_ms);
    }
}

void vk_diag_pipe_done(uint64_t code0, uint64_t code1, double ms, bool cache_hit, int state,
                       uint32_t skipped_draws, uint64_t first_frame, uint64_t last_frame, bool prep) {
    if (!vk_diag_enabled()) return;
    if (state == 1) atomic_fetch_add_explicit(&a_pipe_ready, 1, memory_order_relaxed);
    else atomic_fetch_add_explicit(&a_pipe_fail, 1, memory_order_relaxed);
    if (cache_hit) atomic_fetch_add_explicit(&a_pipe_cache_hit, 1, memory_order_relaxed);
    if (skipped_draws) atomic_fetch_add_explicit(&a_pipe_skipped_total, skipped_draws, memory_order_relaxed);
    atomic_max_u64(&a_max_pipe_ms_x100, (uint64_t)(ms * 100.0));
    if ((!prep && (skipped_draws > 0 || state != 1 || ms >= 8.0)) || g_diag_level >= 2) {
        diag_write_line(skipped_draws > 0 || state != 1 || ms >= 40.0,
                        "[diag-pipe-done] state=%s prep=%d cache_hit=%d code=%llx/%llx skipped=%u frames=%llu..%llu ms=%.1f",
                        state == 1 ? "READY" : "FAILED", prep, cache_hit,
                        (unsigned long long)code0, (unsigned long long)code1,
                        skipped_draws, (unsigned long long)first_frame, (unsigned long long)last_frame, ms);
    }
}

void vk_diag_rec_pipe_wait(double ms) {
    if (!vk_diag_enabled()) return;
    atomic_fetch_add_explicit(&a_rec_pipe_waits, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&a_rec_pipe_wait_ns, (uint64_t)(ms * 1e6), memory_order_relaxed);
    static _Atomic uint32_t nlog;
    if (ms >= 2.0 && atomic_fetch_add_explicit(&nlog, 1, memory_order_relaxed) < 200) {
        diag_write_line(ms >= 20.0, "[diag-rec] pipe_wait=%.2fms (recorder blocked waiting for pipeline)", ms);
    }
}

void vk_diag_rec_draw_dropped(void) {
    if (!vk_diag_enabled()) return;
    uint64_t n = atomic_fetch_add_explicit(&a_rec_draws_dropped, 1, memory_order_relaxed) + 1;
    if (n <= 20 || (n & (n - 1)) == 0) {
        diag_write_line(true, "[diag-rec] DRAW DROPPED because pipeline failed (total=%llu)", (unsigned long long)n);
    }
}

void vk_diag_skip(int why, uint64_t code0, uint64_t code1, uint64_t vh, uint64_t fh,
                  const void *rt0, uint32_t w, uint32_t h, uint32_t fmt, bool presented_rt,
                  bool fullscreen_like, uint64_t frame) {
    if (!vk_diag_enabled()) return;
    if (why < 0 || why >= VK_DIAG_SK_COUNT) why = 0;
    g_fr.skipped++;
    g_fr.sk[why]++;
    if (g_pass.open) g_pass.skipped++;

    static struct { int why; uint64_t c0, c1, last_f; bool fs_like; uint32_t count; } recent[32];
    static int nrec;
    int idx = -1;
    for (int i = 0; i < nrec; i++) {
        if (recent[i].why == why && recent[i].c0 == code0 && recent[i].c1 == code1 && recent[i].fs_like == fullscreen_like) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (nrec < 32) idx = nrec++;
        else {
            idx = 0;
            for (int i = 1; i < nrec; i++) if (recent[i].last_f < recent[idx].last_f) idx = i;
        }
        recent[idx].why = why;
        recent[idx].c0 = code0;
        recent[idx].c1 = code1;
        recent[idx].fs_like = fullscreen_like;
        recent[idx].last_f = frame;
        recent[idx].count = 1;
        diag_write_line(true,
                        "[diag-skip] frame=%llu why=%s code=%llx/%llx hash=%016llx/%016llx rt=%p (%ux%u fmt=%#x pres=%d fs_quad=%d)",
                        (unsigned long long)frame, SK_NAMES[why],
                        (unsigned long long)code0, (unsigned long long)code1,
                        (unsigned long long)vh, (unsigned long long)fh,
                        rt0, w, h, fmt, presented_rt, fullscreen_like);
    } else {
        bool new_burst = frame > recent[idx].last_f + 30;
        recent[idx].last_f = frame;
        recent[idx].count++;
        if (new_burst) {
            diag_write_line(false,
                            "[diag-skip-repeat] frame=%llu why=%s code=%llx/%llx seen=%u rt=%ux%u pres=%d fs_quad=%d",
                            (unsigned long long)frame, SK_NAMES[why],
                            (unsigned long long)code0, (unsigned long long)code1,
                            recent[idx].count, w, h, presented_rt, fullscreen_like);
        }
    }
}

void vk_diag_draw_executed(bool bad_tex, bool zero_scissor, bool zero_viewport, bool no_write_color_or_depth,
                           const void *rt0, uint32_t w, uint32_t h, bool presented_rt, uint64_t fs_hash, uint64_t frame) {
    if (!vk_diag_enabled()) return;
    g_fr.draws++;
    if (g_pass.open) g_pass.draws++;
    if (bad_tex) g_fr.bad_tex_draws++;
    if (zero_scissor) g_fr.zero_scissor++;
    if (zero_viewport) g_fr.zero_viewport++;
    if (no_write_color_or_depth) g_fr.no_write_draws++;

    if (zero_scissor || zero_viewport) {
        static uint64_t last_z_f = ~0ull;
        static int z_logs;
        if (frame != last_z_f && z_logs++ < 80) {
            last_z_f = frame;
            diag_write_line(true,
                            "[diag-draw-zero] frame=%llu %s fs=%016llx rt=%p (%ux%u pres=%d)",
                            (unsigned long long)frame, zero_scissor ? "ZERO_SCISSOR" : "ZERO_VIEWPORT",
                            (unsigned long long)fs_hash, rt0, w, h, presented_rt);
        }
    }
}

static int rt_hist_find_or_add(const void *rt0, uint64_t frame) {
    int idx = -1;
    for (int i = 0; i < g_n_rt_hist; i++) if (g_rt_hist[i].rt0 == rt0) { idx = i; break; }
    if (idx < 0) {
        if (g_n_rt_hist < 128) idx = g_n_rt_hist++;
        else {
            idx = 0;
            for (int i = 1; i < 128; i++) if (g_rt_hist[i].last_frame < g_rt_hist[idx].last_frame) idx = i;
        }
        memset(&g_rt_hist[idx], 0, sizeof g_rt_hist[idx]);
        g_rt_hist[idx].rt0 = rt0;
        g_rt_hist[idx].first_frame = frame;
    }
    if (g_rt_hist[idx].last_frame != frame || g_rt_hist[idx].seen_frames == 0) {
        g_rt_hist[idx].last_frame = frame;
        g_rt_hist[idx].seen_frames++;
    }
    return idx;
}

void vk_diag_pass_begin(const void *rt0, uint32_t w, uint32_t h, uint32_t fmt, uint32_t ncolor,
                        uint32_t load_clear_mask, uint32_t fresh_mask, uint32_t first_write_mask,
                        uint32_t level_fresh_mask, bool presented_rt, uint64_t frame) {
    if (!vk_diag_enabled()) return;
    g_fr.passes++;
    g_pass.open = true;
    g_pass.rt0 = rt0;
    g_pass.w = w;
    g_pass.h = h;
    g_pass.fmt = fmt;
    g_pass.ncolor = ncolor;
    g_pass.load_clear_mask = load_clear_mask;
    g_pass.fresh_mask = fresh_mask;
    g_pass.first_write_mask = first_write_mask;
    g_pass.level_fresh_mask = level_fresh_mask;
    g_pass.presented_rt = presented_rt;
    g_pass.draws = 0;
    g_pass.skipped = 0;
    g_pass.guest_clears = 0;
    if (fresh_mask || first_write_mask || level_fresh_mask) g_fr.fresh_clears++;
    if (rt0 && ncolor) rt_hist_find_or_add(rt0, frame);
}

void vk_diag_pass_guest_clear(bool is_color, uint32_t index, uint32_t w, uint32_t h, uint32_t mask,
                              const float rgba[4], bool fullscreen_like, bool presented_rt, uint64_t frame) {
    (void)index;
    (void)mask;
    if (!vk_diag_enabled()) return;
    g_fr.guest_clears++;
    if (g_pass.open) g_pass.guest_clears++;
    if (is_color && presented_rt && fullscreen_like && rgba &&
        rgba[0] < 0.01f && rgba[1] < 0.01f && rgba[2] < 0.01f && rgba[3] > 0.99f && g_diag_level >= 2) {
        diag_write_line(false, "[diag-clear] frame=%llu fullscreen black clear on presented RT (%ux%u)",
                        (unsigned long long)frame, w, h);
    }
}

void vk_diag_pass_end(uint64_t frame) {
    if (!vk_diag_enabled() || !g_pass.open) return;
    g_pass.open = false;

    bool forced_clear = (g_pass.fresh_mask | g_pass.first_write_mask | g_pass.level_fresh_mask) != 0;
    if (forced_clear && g_pass.guest_clears == 0 && g_pass.ncolor > 0 && g_pass.rt0) {
        g_fr.fresh_without_guest_clear++;
        int hi = rt_hist_find_or_add(g_pass.rt0, frame);
        g_rt_hist[hi].fresh_no_clear_count++;
        if ((!g_pass.presented_rt || g_pass.draws == 0) && g_rt_hist[hi].seen_frames > 1 &&
            (!g_rt_hist[hi].warned_fresh_no_clear || g_pass.draws == 0)) {
            g_rt_hist[hi].warned_fresh_no_clear = true;
            diag_write_line(false,
                            "[diag-fresh-wipe] frame=%llu rt=%p (%ux%u fmt=%#x pres=%d) forced LOAD_OP_CLEAR "
                            "(fresh=%#x first=%#x lvl=%#x) WITHOUT guest clear! pass_draws=%u pass_skipped=%u seen_frames=%llu",
                            (unsigned long long)frame, g_pass.rt0, g_pass.w, g_pass.h, g_pass.fmt, g_pass.presented_rt,
                            g_pass.fresh_mask, g_pass.first_write_mask, g_pass.level_fresh_mask,
                            g_pass.draws, g_pass.skipped, (unsigned long long)g_rt_hist[hi].seen_frames);
        }
    }

    if (g_pass.draws == 0) {
        if (g_pass.skipped > 0) g_fr.empty_skipped_passes++;
        if ((g_pass.guest_clears > 0 || forced_clear) && g_pass.ncolor > 0) {
            g_fr.empty_cleared_passes++;
            static const void *seen_empty_rt[16];
            static int n_seen_empty;
            bool first_rt = true;
            for (int i = 0; i < n_seen_empty; i++) if (seen_empty_rt[i] == g_pass.rt0) { first_rt = false; break; }
            if (first_rt && n_seen_empty < 16) seen_empty_rt[n_seen_empty++] = g_pass.rt0;
            if (g_pass.skipped > 0 || g_pass.presented_rt || first_rt || g_diag_level >= 2) {
                diag_write_line(g_pass.skipped > 0 || g_pass.presented_rt,
                                "[diag-pass-empty] frame=%llu rt=%p (%ux%u fmt=%#x pres=%d) draws=0 skipped=%u "
                                "guest_clears=%u forced_clear(fresh=%#x first=%#x lvl=%#x)",
                                (unsigned long long)frame, g_pass.rt0, g_pass.w, g_pass.h, g_pass.fmt,
                                g_pass.presented_rt, g_pass.skipped, g_pass.guest_clears,
                                g_pass.fresh_mask, g_pass.first_write_mask, g_pass.level_fresh_mask);
            }
        }
    }
}

void vk_diag_dead_slot_sampled(uint32_t slot, uint32_t dead_fmt, uint64_t fs_hash, uint64_t frame) {
    if (!vk_diag_enabled()) return;
    atomic_fetch_add_explicit(&a_dead_slots, 1, memory_order_relaxed);
    g_fr.dead_slots++;
    static uint32_t last_slot = ~0u;
    static uint64_t last_f = ~0ull;
    static int logs;
    if ((slot != last_slot || frame > last_f + 30) && logs++ < 120) {
        last_slot = slot;
        last_f = frame;
        diag_write_line(true, "[diag-dead-tex] frame=%llu slot=%u dead_fmt=%#x fs=%016llx (substituted dummy black)",
                        (unsigned long long)frame, slot, dead_fmt, (unsigned long long)fs_hash);
    }
}

void vk_diag_bad_tex_bind(const char *why, uint32_t slot, uint64_t handle, const void *tex,
                          uint32_t w, uint32_t h, uint32_t fmt, uint64_t frame) {
    if (!vk_diag_enabled()) return;
    atomic_fetch_add_explicit(&a_bad_tex_binds, 1, memory_order_relaxed);
    g_fr.bad_tex_binds++;
    static uint64_t seen_h[32];
    static int nseen;
    bool fresh = true;
    for (int i = 0; i < nseen; i++) if (seen_h[i] == handle) { fresh = false; break; }
    if (fresh && nseen < 32) {
        seen_h[nseen++] = handle;
        diag_write_line(false, "[diag-bad-tex] frame=%llu why=%s slot=%u handle=%#llx tex=%p (%ux%u fmt=%#x)",
                        (unsigned long long)frame, why, slot, (unsigned long long)handle, tex, w, h, fmt);
    }
}

void vk_diag_tex_event(const char *kind, const void *tex, uint32_t w, uint32_t h, uint32_t fmt,
                       uint64_t extra1, int64_t extra2, uint64_t frame) {
    if (!vk_diag_enabled()) return;
    if (!strcmp(kind, "late_hash_change")) {
        atomic_fetch_add_explicit(&a_tex_late_change, 1, memory_order_relaxed);
        g_fr.tex_late_change++;
        static int logs;
        if (logs++ < 120) {
            diag_write_line(false, "[diag-tex] frame=%llu %s tex=%p (%ux%u fmt=%#x) delta_frames=%llu watch_ch=%lld",
                            (unsigned long long)frame, kind, tex, w, h, fmt,
                            (unsigned long long)extra1, (long long)extra2);
        }
    } else if (!strcmp(kind, "dynamic_promote")) {
        atomic_fetch_add_explicit(&a_tex_dyn_promote, 1, memory_order_relaxed);
    } else if (!strcmp(kind, "res_realloc")) {
        atomic_fetch_add_explicit(&a_tex_realloc, 1, memory_order_relaxed);
        g_fr.tex_realloc++;
        diag_write_line(false, "[diag-tex] frame=%llu %s tex=%p (%ux%u fmt=%#x)",
                        (unsigned long long)frame, kind, tex, w, h, fmt);
    } else if (!strcmp(kind, "view_evict")) {
        atomic_fetch_add_explicit(&a_tex_view_evict, 1, memory_order_relaxed);
        g_fr.view_evict++;
        static int ev_logs;
        if (ev_logs++ < 40) diag_write_line(false, "[diag-tex] frame=%llu view_cache_evict", (unsigned long long)frame);
    } else if (!strcmp(kind, "view_fail")) {
        atomic_fetch_add_explicit(&a_tex_view_fail, 1, memory_order_relaxed);
        static int vf_logs;
        if (vf_logs++ < 40) diag_write_line(true, "[diag-tex] frame=%llu view_fail tex=%p (%ux%u fmt=%#x)", (unsigned long long)frame, tex, w, h, fmt);
    } else if (!strcmp(kind, "pending_drop")) {
        atomic_fetch_add_explicit(&a_tex_pending_drop, 1, memory_order_relaxed);
        static int pd_logs;
        if (pd_logs++ < 40) diag_write_line(false, "[diag-tex] frame=%llu pending_upload_dropped tex=%p (%ux%u)", (unsigned long long)frame, tex, w, h);
    }
}

void vk_diag_mem_event(const char *kind, uint64_t a, uint64_t b, uint64_t frame) {
    if (!vk_diag_enabled()) return;
    atomic_fetch_add_explicit(&a_mem_events, 1, memory_order_relaxed);
    g_fr.mem_events++;
    static int logs;
    if (logs++ < 100) {
        diag_write_line(false, "[diag-mem] frame=%llu %s a=%#llx b=%#llx",
                        (unsigned long long)frame, kind, (unsigned long long)a, (unsigned long long)b);
    }
}

void vk_diag_swapchain_event(const char *kind, int code, uint32_t w, uint32_t h, uint64_t frame) {
    if (!vk_diag_enabled()) return;
    atomic_fetch_add_explicit(&a_swap_events, 1, memory_order_relaxed);
    g_fr.swap_events++;
    diag_write_line(true, "[diag-swap] frame=%llu %s code=%d extent=%ux%u",
                    (unsigned long long)frame, kind, code, w, h);
}

void vk_diag_frame_end(uint64_t frame, double present_gap_ms, double acquire_ms,
                       const void *presented_tex, uint32_t w, uint32_t h, uint32_t fmt,
                       size_t prep_progs, size_t prep_pipes, size_t prep_cs, const char *level_name) {
    if (!vk_diag_enabled()) return;
    static struct { const void *tex; uint32_t w, h, fmt; } pres_seen[8];
    static int n_pres_seen;
    bool known_pres = false;
    for (int i = 0; i < n_pres_seen; i++) {
        if (pres_seen[i].tex == presented_tex && pres_seen[i].w == w && pres_seen[i].h == h && pres_seen[i].fmt == fmt) {
            known_pres = true;
            break;
        }
    }
    if (!known_pres) {
        if (n_pres_seen < 8) pres_seen[n_pres_seen++] = (typeof(pres_seen[0])){presented_tex, w, h, fmt};
        diag_write_line(false, "[diag-present-target] frame=%llu tex=%p %ux%u fmt=%#x (slot %d)",
                        (unsigned long long)frame, presented_tex, w, h, fmt, n_pres_seen);
    }

    uint64_t spv_hits = atomic_load_explicit(&a_spv_hits, memory_order_relaxed);
    uint64_t spv_miss = atomic_load_explicit(&a_spv_miss, memory_order_relaxed);
    uint64_t prog_ready = atomic_load_explicit(&a_prog_ready, memory_order_relaxed);
    uint64_t pipe_ready = atomic_load_explicit(&a_pipe_ready, memory_order_relaxed);
    uint64_t pipe_hit = atomic_load_explicit(&a_pipe_cache_hit, memory_order_relaxed);
    uint64_t rec_waits = atomic_load_explicit(&a_rec_pipe_waits, memory_order_relaxed);
    uint64_t rec_wait_ns = atomic_load_explicit(&a_rec_pipe_wait_ns, memory_order_relaxed);
    uint64_t rec_drop = atomic_load_explicit(&a_rec_draws_dropped, memory_order_relaxed);

    uint64_t d_prog_r = prog_ready - g_prev.last_prog_ready;
    uint64_t d_pipe_r = pipe_ready - g_prev.last_pipe_ready;
    uint64_t d_spv_m = spv_miss - g_prev.last_spv_miss;
    uint64_t d_rec_w = rec_waits - g_prev.last_rec_waits;
    double d_rec_ms = (double)(rec_wait_ns - g_prev.last_rec_wait_ns) / 1e6;
    uint64_t d_rec_drop = rec_drop - g_prev.last_rec_dropped;

    g_prev.last_spv_hits = spv_hits;
    g_prev.last_spv_miss = spv_miss;
    g_prev.last_prog_ready = prog_ready;
    g_prev.last_pipe_ready = pipe_ready;
    g_prev.last_pipe_hit = pipe_hit;
    g_prev.last_rec_waits = rec_waits;
    g_prev.last_rec_wait_ns = rec_wait_ns;
    g_prev.last_rec_dropped = rec_drop;

    bool draw_drop = g_roll_samples >= 20 && g_roll_draws >= 30.0 &&
                     ((double)g_fr.draws < g_roll_draws * 0.55) &&
                     (g_roll_draws - (double)g_fr.draws >= 20.0);
    if (g_fr.draws > 0) {
        if (g_roll_samples == 0) g_roll_draws = (double)g_fr.draws;
        else g_roll_draws = g_roll_draws * 0.90 + (double)g_fr.draws * 0.10;
        if (g_roll_samples < 1000) g_roll_samples++;
    }

    bool anom = g_fr.skipped > 0 || d_prog_r > 0 || d_pipe_r > 0 || d_rec_ms >= 2.0 || d_rec_drop > 0 ||
                g_fr.empty_skipped_passes > 0 || g_fr.zero_scissor > 0 || g_fr.zero_viewport > 0 ||
                g_fr.dead_slots > 0 || g_fr.bad_tex_binds > 0 || g_fr.tex_realloc > 0 || g_fr.swap_events > 0 ||
                present_gap_ms >= 28.0 || acquire_ms >= 20.0 || g_fr.draws == 0 || draw_drop;

    if (anom || g_diag_level >= 2) {
        char skbuf[192] = "";
        int pos = 0;
        for (int i = 0; i < VK_DIAG_SK_COUNT; i++) {
            if (!g_fr.sk[i]) continue;
            pos += snprintf(skbuf + pos, sizeof skbuf - (size_t)pos, "%s%s=%llu",
                            pos ? "," : "", SK_NAMES[i], (unsigned long long)g_fr.sk[i]);
        }
        static uint64_t zero_draw_streak;
        if (g_fr.draws == 0) zero_draw_streak++; else zero_draw_streak = 0;
        if (g_fr.draws != 0 || zero_draw_streak <= 5 || (zero_draw_streak % 60) == 0) {
            diag_write_line(g_fr.skipped > 0 || d_rec_drop > 0 || present_gap_ms >= 40.0 || g_fr.zero_scissor > 0,
                            "[diag-frame] f=%llu lv=%s gap=%.1fms acq=%.1fms draws=%llu(roll=%.0f%s) skip=%llu[%s] "
                            "passes=%llu(empty_clr=%llu empty_sk=%llu fresh_noclr=%llu) "
                            "compile(prog=%llu spv_miss=%llu pipe=%llu rec_wait=%llu/%.1fms drop=%llu) "
                            "anom(zsc=%llu zvp=%llu nowr=%llu badtex=%llu dead=%llu tlate=%llu)",
                            (unsigned long long)frame, level_name ? level_name : "-", present_gap_ms, acquire_ms,
                            (unsigned long long)g_fr.draws, g_roll_draws, draw_drop ? " DROP!" : "",
                            (unsigned long long)g_fr.skipped, skbuf[0] ? skbuf : "-",
                            (unsigned long long)g_fr.passes, (unsigned long long)g_fr.empty_cleared_passes,
                            (unsigned long long)g_fr.empty_skipped_passes,
                            (unsigned long long)g_fr.fresh_without_guest_clear,
                            (unsigned long long)d_prog_r, (unsigned long long)d_spv_m,
                            (unsigned long long)d_pipe_r, (unsigned long long)d_rec_w, d_rec_ms,
                            (unsigned long long)d_rec_drop,
                            (unsigned long long)g_fr.zero_scissor, (unsigned long long)g_fr.zero_viewport,
                            (unsigned long long)g_fr.no_write_draws, (unsigned long long)g_fr.bad_tex_draws,
                            (unsigned long long)g_fr.dead_slots, (unsigned long long)g_fr.tex_late_change);
        }
    }

    g_hb_frames++;
    g_hb_draws_sum += g_fr.draws;
    if (present_gap_ms > g_hb_max_gap_ms) g_hb_max_gap_ms = present_gap_ms;
    g_hb.draws += g_fr.draws;
    g_hb.skipped += g_fr.skipped;
    g_hb.passes += g_fr.passes;
    g_hb.guest_clears += g_fr.guest_clears;
    g_hb.fresh_clears += g_fr.fresh_clears;
    g_hb.fresh_without_guest_clear += g_fr.fresh_without_guest_clear;
    g_hb.empty_cleared_passes += g_fr.empty_cleared_passes;
    g_hb.empty_skipped_passes += g_fr.empty_skipped_passes;
    g_hb.zero_scissor += g_fr.zero_scissor;
    g_hb.zero_viewport += g_fr.zero_viewport;
    g_hb.no_write_draws += g_fr.no_write_draws;
    g_hb.bad_tex_draws += g_fr.bad_tex_draws;
    g_hb.dead_slots += g_fr.dead_slots;
    g_hb.bad_tex_binds += g_fr.bad_tex_binds;
    g_hb.tex_late_change += g_fr.tex_late_change;
    g_hb.tex_realloc += g_fr.tex_realloc;
    g_hb.view_evict += g_fr.view_evict;
    g_hb.mem_events += g_fr.mem_events;
    g_hb.swap_events += g_fr.swap_events;
    for (int i = 0; i < VK_DIAG_SK_COUNT; i++) g_hb.sk[i] += g_fr.sk[i];
    memset(&g_fr, 0, sizeof g_fr);

    uint64_t now_ns = plat_now_ns();
    if (now_ns - g_hb_last_ns >= 2000000000ull) {
        double secs = (double)(now_ns - g_hb_last_ns) / 1e9;
        double fps = secs > 0 ? (double)g_hb_frames / secs : 0.0;
        double avg_draws = g_hb_frames ? (double)g_hb_draws_sum / (double)g_hb_frames : 0.0;
        char skbuf[192] = "";
        int pos = 0;
        for (int i = 0; i < VK_DIAG_SK_COUNT; i++) {
            if (!g_hb.sk[i]) continue;
            pos += snprintf(skbuf + pos, sizeof skbuf - (size_t)pos, "%s%s=%llu",
                            pos ? "," : "", SK_NAMES[i], (unsigned long long)g_hb.sk[i]);
        }
        diag_write_line(false,
                        "[diag-hb] f=%llu %.1ffps max=%.1fms avg_draws=%.0f skip=%llu[%s] "
                        "passes=%llu(empty_clr=%llu fresh_noclr=%llu) "
                        "prep(p=%zu pipe=%zu cs=%zu) inflight=%d totals[spv=%llu/%llu prog=%llu/%llu pipe=%llu(hit=%llu) rec_w=%llu/%.1fms drop=%llu] "
                        "anom[zsc=%llu badtex=%llu dead=%llu tlate=%llu view_ev=%llu]",
                        (unsigned long long)frame, fps, g_hb_max_gap_ms, avg_draws,
                        (unsigned long long)g_hb.skipped, skbuf[0] ? skbuf : "-",
                        (unsigned long long)g_hb.passes, (unsigned long long)g_hb.empty_cleared_passes,
                        (unsigned long long)g_hb.fresh_without_guest_clear,
                        prep_progs, prep_pipes, prep_cs, atomic_load_explicit(&a_inflight_progs, memory_order_relaxed),
                        (unsigned long long)spv_hits, (unsigned long long)spv_miss,
                        (unsigned long long)prog_ready, (unsigned long long)atomic_load_explicit(&a_prog_fail, memory_order_relaxed),
                        (unsigned long long)pipe_ready, (unsigned long long)pipe_hit,
                        (unsigned long long)rec_waits, (double)rec_wait_ns / 1e6, (unsigned long long)rec_drop,
                        (unsigned long long)g_hb.zero_scissor, (unsigned long long)g_hb.bad_tex_draws,
                        (unsigned long long)g_hb.dead_slots, (unsigned long long)g_hb.tex_late_change,
                        (unsigned long long)g_hb.view_evict);
        g_hb_last_ns = now_ns;
        g_hb_max_gap_ms = 0;
        g_hb_frames = 0;
        g_hb_draws_sum = 0;
        memset(&g_hb, 0, sizeof g_hb);
    }
}
