// Render & shader-cache anomaly diagnostics for diagnosing transient missing draws / black flickers.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    VK_DIAG_SK_NO_PROGRAM = 0,
    VK_DIAG_SK_NO_STAGES,
    VK_DIAG_SK_PROG_BUILDING,
    VK_DIAG_SK_PROG_FAILED,
    VK_DIAG_SK_PIPE_BUILDING,
    VK_DIAG_SK_PIPE_FAILED,
    VK_DIAG_SK_CS_BUILDING,
    VK_DIAG_SK_CS_FAILED,
    VK_DIAG_SK_TOPOLOGY,
    VK_DIAG_SK_DEBUG_SKIP,
    VK_DIAG_SK_HIDDEN,
    VK_DIAG_SK_COUNT
};

extern int g_vk_diag_level;
bool vk_diag_enabled_slow(void);
static inline bool vk_diag_enabled(void) {
    int l = __atomic_load_n(&g_vk_diag_level, __ATOMIC_RELAXED);
    return l >= 0 ? l > 0 : vk_diag_enabled_slow();
}

void vk_diag_init(void);
void vk_diag_shutdown(void);

void vk_diag_spv_result(int stage, bool cache_hit, double ms, bool ok);
void vk_diag_prog_start(uint64_t code0, uint64_t code1, uint64_t vh, uint64_t fh, bool prep);
void vk_diag_prog_done(uint64_t code0, uint64_t code1, uint64_t vh, uint64_t fh,
                       double q_tr_ms, double tr_ms, double q_spv_ms, double spv_ms, double total_ms,
                       uint32_t skipped_draws, uint64_t first_frame, uint64_t last_frame, int state);
void vk_diag_pipe_done(uint64_t code0, uint64_t code1, double ms, bool cache_hit, int state,
                       uint32_t skipped_draws, uint64_t first_frame, uint64_t last_frame, bool prep);
void vk_diag_rec_pipe_wait(double ms);
void vk_diag_rec_draw_dropped(void);

void vk_diag_skip(int why, uint64_t code0, uint64_t code1, uint64_t vh, uint64_t fh,
                  const void *rt0, uint32_t w, uint32_t h, uint32_t fmt, bool presented_rt,
                  bool fullscreen_like, uint64_t frame);
void vk_diag_draw_executed(bool bad_tex, bool zero_scissor, bool zero_viewport, bool no_write_color_or_depth,
                           const void *rt0, uint32_t w, uint32_t h, bool presented_rt, uint64_t fs_hash, uint64_t frame);

void vk_diag_pass_begin(const void *rt0, uint32_t w, uint32_t h, uint32_t fmt, uint32_t ncolor,
                        uint32_t load_clear_mask, uint32_t fresh_mask, uint32_t first_write_mask,
                        uint32_t level_fresh_mask, bool presented_rt, uint64_t frame);
void vk_diag_pass_guest_clear(bool is_color, uint32_t index, uint32_t w, uint32_t h, uint32_t mask,
                              const float rgba[4], bool fullscreen_like, bool presented_rt, uint64_t frame);
void vk_diag_pass_end(uint64_t frame);

void vk_diag_dead_slot_sampled(uint32_t slot, uint32_t dead_fmt, uint64_t fs_hash, uint64_t frame);
void vk_diag_bad_tex_bind(const char *why, uint32_t slot, uint64_t handle, const void *tex,
                          uint32_t w, uint32_t h, uint32_t fmt, uint64_t frame);
void vk_diag_tex_event(const char *kind, const void *tex, uint32_t w, uint32_t h, uint32_t fmt,
                       uint64_t extra1, int64_t extra2, uint64_t frame);
void vk_diag_mem_event(const char *kind, uint64_t a, uint64_t b, uint64_t frame);
void vk_diag_swapchain_event(const char *kind, int code, uint32_t w, uint32_t h, uint64_t frame);

void vk_diag_frame_end(uint64_t frame, double present_gap_ms, double acquire_ms,
                       const void *presented_tex, uint32_t w, uint32_t h, uint32_t fmt,
                       size_t prep_progs, size_t prep_pipes, size_t prep_cs, const char *level_name);

#ifdef __cplusplus
}
#endif
