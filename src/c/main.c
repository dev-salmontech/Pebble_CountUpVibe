#include <pebble.h>
#include <string.h>

#define INTERVAL_GRID_SECONDS 15
#define DEFAULT_INTERVAL_SECONDS (5 * 60)
#define MIN_INTERVAL_SECONDS INTERVAL_GRID_SECONDS
#define MAX_INTERVAL_SECONDS (99 * 60 + 59)
#define EDIT_TIMEOUT_MS 15000
#define SEC_STEP INTERVAL_GRID_SECONDS

/* 0: every fresh launch starts at the 5 min default (stateless).
 * 1: remember the last interval the user set across restarts.
 * Flip to 1 to switch behaviours. */
#define REMEMBER_INTERVAL 0

#define COOKIE_VIBE 1

enum {
  PERSIST_INITIALIZED = 1,
  PERSIST_RUNNING = 2,
  PERSIST_ELAPSED_ACCUM = 3,
  PERSIST_RUN_STARTED = 4,
  PERSIST_INTERVAL = 5,
  PERSIST_NEXT_VIBE_EPOCH = 6,
  PERSIST_VIBE_COUNT = 7,
  PERSIST_FROZEN_CYCLE = 8
};

enum { MODE_EDIT, MODE_RUN };
enum { FIELD_MIN, FIELD_SEC };

typedef struct {
  bool running;
  int32_t elapsed_accum;
  int32_t run_started_epoch;
  int32_t interval_seconds;
  int32_t next_vibe_epoch;
  int32_t vibe_count;
} TimerState;

static Window *s_main_window;
static Layer *s_water_layer;
static Layer *s_deco_layer;
static TextLayer *s_clock_layer;
static TextLayer *s_status_layer;
static TextLayer *s_c_timer_layer;
static TextLayer *s_b_timer_layer;
static TextLayer *s_b_interval_layer;
static TextLayer *s_btn_up_layer;
static TextLayer *s_btn_center_layer;
static TextLayer *s_btn_down_layer;
static AppTimer *s_ui_timer;
static AppTimer *s_edit_timeout;
static TimerState s_state;
static bool s_launched_by_wakeup;
static int32_t s_frozen_cycle_elapsed;
static int s_mode;
static int s_edit_field;
static int32_t s_edit_min;
static int32_t s_edit_sec;

static int s_center_y;
static int s_win_w;
static int s_win_h;
static GFont s_font_big;
static GFont s_font_small;

/* Glyph triangles, cached once and translated per draw to avoid per-frame
 * heap churn in the render path. Points are relative to the glyph centre. */
static GPoint s_pts_play[3]  = {{-4, -6}, {-4, 6}, {6, 0}};
static GPoint s_pts_reset[3] = {{-1, 0}, {6, -6}, {6, 6}};
static GPoint s_pts_up[3]    = {{0, -5}, {-6, 5}, {6, 5}};
static GPoint s_pts_down[3]  = {{0, 5}, {-6, -5}, {6, -5}};
static GPathInfo s_info_play  = {3, s_pts_play};
static GPathInfo s_info_reset = {3, s_pts_reset};
static GPathInfo s_info_up    = {3, s_pts_up};
static GPathInfo s_info_down  = {3, s_pts_down};
static GPath *s_path_play;
static GPath *s_path_reset;
static GPath *s_path_up;
static GPath *s_path_down;

static char s_clock_text[16];
static char s_status_text[16];
static char s_timer_text[16];
static char s_interval_text[12];
static char s_min_buf[8];
static char s_sec_buf[8];
static char s_btn_up_text[16];
static char s_btn_center_text[16];
static char s_btn_down_text[16];
static char s_glance_text[96];
static char s_last_clock[16];
static char s_last_status[16];
static char s_last_interval[12];
static int s_last_fill_h = -1;

static const uint32_t s_vibe_durations[] = { 150, 90, 320 };
static const VibePattern s_vibe_pattern = {
  .durations = s_vibe_durations,
  .num_segments = 3
};

static void update_ui(void);
static void schedule_ui_tick(void);
static void update_app_glance_safe(void);
static void apply_mode_layout(void);
static void update_edit_display(void);
static void update_button_labels(void);
static void edit_timeout_handler(void *context);

static int32_t clamp_interval(int32_t interval_seconds) {
  if (interval_seconds < MIN_INTERVAL_SECONDS) {
    return MIN_INTERVAL_SECONDS;
  }
  if (interval_seconds > MAX_INTERVAL_SECONDS) {
    return MAX_INTERVAL_SECONDS;
  }
  return interval_seconds;
}

static int32_t now_seconds(void) {
  return (int32_t)time(NULL);
}

static int32_t next_vibe_after(int32_t t) {
  /* Relative to the moment the run/cycle starts (not clock-grid aligned) so a
   * fresh start always begins a full interval -> water starts at the top. */
  int32_t interval = s_state.interval_seconds > 0 ? s_state.interval_seconds : INTERVAL_GRID_SECONDS;
  return t + interval;
}

static int32_t total_elapsed(void) {
  if (!s_state.running) {
    return s_state.elapsed_accum;
  }
  int32_t delta = now_seconds() - s_state.run_started_epoch;
  if (delta < 0) {
    delta = 0;
  }
  return s_state.elapsed_accum + delta;
}

static int32_t secs_to_vibe(void) {
  if (!s_state.running) {
    return s_state.interval_seconds;
  }
  int32_t left = s_state.next_vibe_epoch - now_seconds();
  if (left < 0) {
    left = 0;
  }
  return left;
}

static void format_elapsed(int32_t elapsed_seconds, char *buffer, size_t buffer_size) {
  if (elapsed_seconds < 0) {
    elapsed_seconds = 0;
  }
  int32_t hours = elapsed_seconds / 3600;
  int32_t minutes = (elapsed_seconds / 60) % 60;
  int32_t seconds = elapsed_seconds % 60;
  if (hours > 0) {
    snprintf(buffer, buffer_size, "%ld:%02ld:%02ld", (long)hours, (long)minutes, (long)seconds);
  } else {
    snprintf(buffer, buffer_size, "%ld:%02ld", (long)minutes, (long)seconds);
  }
}

static void format_mmss(int32_t total_seconds, char *buffer, size_t buffer_size) {
  if (total_seconds < 0) {
    total_seconds = 0;
  }
  int32_t minutes = total_seconds / 60;
  int32_t seconds = total_seconds % 60;
  snprintf(buffer, buffer_size, "%ld:%02ld", (long)minutes, (long)seconds);
}

#if !PBL_PLATFORM_APLITE && defined(APP_GLANCE_SLICE_NO_EXPIRATION)
static void glance_reload_handler(AppGlanceReloadSession *session, size_t limit, void *context) {
  AppGlanceSlice slice = {
    .expiration_time = APP_GLANCE_SLICE_NO_EXPIRATION,
    .layout = {
      .icon = APP_GLANCE_SLICE_DEFAULT_ICON,
      .subtitle_template_string = s_glance_text
    }
  };
  app_glance_add_slice(session, slice);
}
#endif

static void update_app_glance_safe(void) {
#if !PBL_PLATFORM_APLITE && defined(APP_GLANCE_SLICE_NO_EXPIRATION)
  char elapsed_text[16];
  format_elapsed(total_elapsed(), elapsed_text, sizeof(elapsed_text));
  char interval_text[12];
  format_mmss(s_state.interval_seconds, interval_text, sizeof(interval_text));
  if (s_state.running) {
    snprintf(s_glance_text, sizeof(s_glance_text), "Run %s, vibe/%s", elapsed_text, interval_text);
  } else {
    snprintf(s_glance_text, sizeof(s_glance_text), "Paused %s, vibe/%s", elapsed_text, interval_text);
  }
  app_glance_reload(glance_reload_handler, NULL);
#endif
}

static void state_save(void) {
  persist_write_int(PERSIST_INITIALIZED, 1);
  persist_write_int(PERSIST_RUNNING, s_state.running ? 1 : 0);
  persist_write_int(PERSIST_ELAPSED_ACCUM, s_state.elapsed_accum);
  persist_write_int(PERSIST_RUN_STARTED, s_state.run_started_epoch);
  persist_write_int(PERSIST_INTERVAL, s_state.interval_seconds);
  persist_write_int(PERSIST_NEXT_VIBE_EPOCH, s_state.next_vibe_epoch);
  persist_write_int(PERSIST_VIBE_COUNT, s_state.vibe_count);
  persist_write_int(PERSIST_FROZEN_CYCLE, s_frozen_cycle_elapsed);
}

static void state_load_or_default(void) {
  if (!persist_exists(PERSIST_INITIALIZED)) {
    /* First ever launch: start in a READY (reset-to-zero) state so init's
     * manual-launch path treats it as inactive and opens the default editor. */
    s_state.running = false;
    s_state.elapsed_accum = 0;
    s_state.run_started_epoch = 0;
    s_state.interval_seconds = DEFAULT_INTERVAL_SECONDS;
    s_state.next_vibe_epoch = 0;
    s_state.vibe_count = 0;
    s_frozen_cycle_elapsed = 0;
    state_save();
    return;
  }

  s_state.running = persist_read_int(PERSIST_RUNNING) != 0;
  s_state.elapsed_accum = persist_read_int(PERSIST_ELAPSED_ACCUM);
  s_state.run_started_epoch = persist_read_int(PERSIST_RUN_STARTED);
  s_state.interval_seconds = persist_read_int(PERSIST_INTERVAL);
  if (s_state.interval_seconds == 0) {
    s_state.interval_seconds = DEFAULT_INTERVAL_SECONDS;
  }
  s_state.interval_seconds = clamp_interval(s_state.interval_seconds);
  s_state.next_vibe_epoch = persist_read_int(PERSIST_NEXT_VIBE_EPOCH);
  s_state.vibe_count = persist_read_int(PERSIST_VIBE_COUNT);
  s_frozen_cycle_elapsed = persist_read_int(PERSIST_FROZEN_CYCLE);
}

static void cancel_pending_wakeup(void) {
  wakeup_cancel_all();
}

static void schedule_wakeup_for_next(void) {
  cancel_pending_wakeup();
  if (!s_state.running) {
    return;
  }
  time_t target = (time_t)s_state.next_vibe_epoch;
  time_t now = time(NULL);
  if (target <= now) {
    target = now + 1;
  }
  WakeupId id = wakeup_schedule(target, COOKIE_VIBE, true);
  if (id < 0) {
    target = now + 60;
    wakeup_schedule(target, COOKIE_VIBE, true);
  }
}

static GColor color_water(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x55AAFF);
#else
  return GColorLightGray;
#endif
}
static GColor color_ink(void) {
  return GColorBlack;
}
static void compute_layout(GRect bounds) {
  s_center_y = bounds.size.h / 2 + 4;
  s_win_w = bounds.size.w;
  s_win_h = bounds.size.h;
}

/* Action-bar style glyphs: white shapes centred on `c`, drawn over an accent
 * chip so they stay legible against the water-fill or white background. */

enum {
  GLYPH_PLAY,
  GLYPH_PAUSE,
  GLYPH_RESET,
  GLYPH_SETTINGS,
  GLYPH_UP,
  GLYPH_DOWN,
  GLYPH_NEXT,
  GLYPH_CHECK
};

static void draw_tri(GContext *ctx, GPath *p, GPoint c) {
  if (!p) {
    return;
  }
  gpath_move_to(p, c);
  gpath_draw_filled(ctx, p);
}

static void draw_glyph(GContext *ctx, int glyph, GPoint c) {
  graphics_context_set_fill_color(ctx, color_ink());
  graphics_context_set_stroke_color(ctx, color_ink());
  graphics_context_set_stroke_width(ctx, 2);

  switch (glyph) {
    case GLYPH_PLAY:
      draw_tri(ctx, s_path_play, c);
      break;
    case GLYPH_PAUSE:
      graphics_fill_rect(ctx, GRect(c.x - 5, c.y - 6, 3, 12), 1, GCornersAll);
      graphics_fill_rect(ctx, GRect(c.x + 2, c.y - 6, 3, 12), 1, GCornersAll);
      break;
    case GLYPH_RESET:
      /* restart / skip-to-start: bar + left triangle */
      graphics_fill_rect(ctx, GRect(c.x - 6, c.y - 6, 3, 12), 1, GCornersAll);
      draw_tri(ctx, s_path_reset, c);
      break;
    case GLYPH_SETTINGS:
      /* sliders: two tracks with offset knobs */
      graphics_fill_rect(ctx, GRect(c.x - 6, c.y - 4, 12, 2), 1, GCornersAll);
      graphics_fill_circle(ctx, GPoint(c.x + 2, c.y - 3), 3);
      graphics_fill_rect(ctx, GRect(c.x - 6, c.y + 3, 12, 2), 1, GCornersAll);
      graphics_fill_circle(ctx, GPoint(c.x - 2, c.y + 4), 3);
      break;
    case GLYPH_UP:
      draw_tri(ctx, s_path_up, c);
      break;
    case GLYPH_DOWN:
      draw_tri(ctx, s_path_down, c);
      break;
    case GLYPH_NEXT:
      graphics_draw_line(ctx, GPoint(c.x - 2, c.y - 6), GPoint(c.x + 4, c.y));
      graphics_draw_line(ctx, GPoint(c.x + 4, c.y), GPoint(c.x - 2, c.y + 6));
      break;
    case GLYPH_CHECK:
      graphics_draw_line(ctx, GPoint(c.x - 6, c.y + 1), GPoint(c.x - 1, c.y + 6));
      graphics_draw_line(ctx, GPoint(c.x - 1, c.y + 6), GPoint(c.x + 7, c.y - 6));
      break;
  }
}

static void draw_button_symbols(GContext *ctx, GRect bounds) {
  int16_t x = bounds.size.w - 16;
  int16_t y_top = 21;
  int16_t y_bot = bounds.size.h - 22;

  if (s_mode == MODE_EDIT) {
    draw_glyph(ctx, GLYPH_UP, GPoint(x, y_top));
    draw_glyph(ctx, (s_edit_field == FIELD_MIN) ? GLYPH_NEXT : GLYPH_CHECK, GPoint(x, s_center_y));
    draw_glyph(ctx, GLYPH_DOWN, GPoint(x, y_bot));
  } else {
    draw_glyph(ctx, s_state.running ? GLYPH_PAUSE : GLYPH_PLAY, GPoint(x, y_top));
    draw_glyph(ctx, GLYPH_SETTINGS, GPoint(x, s_center_y));
    draw_glyph(ctx, (s_state.running || total_elapsed() > 0) ? GLYPH_RESET : GLYPH_PLAY,
               GPoint(x, y_bot));
  }
}

static int16_t compute_live_fill_height(int16_t h) {
  /* Draining fill: full at the start of each interval cycle, emptying to zero
   * as the countdown reaches the next vibe (height = remaining / interval). */
  int32_t interval = s_state.interval_seconds > 0 ? s_state.interval_seconds : 1;
  int32_t remaining;
  if (s_state.running) {
    remaining = secs_to_vibe();
  } else {
    remaining = interval - s_frozen_cycle_elapsed;
  }
  if (remaining < 0) {
    remaining = 0;
  }
  if (remaining > interval) {
    remaining = interval;
  }
  int32_t fh = (remaining * h) / interval;
  if (fh > h) {
    fh = h;
  }
  return (int16_t)fh;
}

static int16_t compute_fill_height(int16_t h) {
  /* Live in every mode: the water keeps draining while the timer runs (even in
   * edit mode); when paused, compute_live_fill_height holds the frozen cycle. */
  return compute_live_fill_height(h);
}

/* The water layer's frame IS the water rectangle (bottom-anchored, height =
 * fill). It just paints itself solid; the area it vacates falls back to the
 * white window background, so only the changed strip ever repaints. */
static void water_update_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, color_water());
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

/* Decorations (button glyphs + edit panel) sit above the water on a
 * transparent full-screen layer, redrawn only when mode/state/edit change. */
static void deco_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  draw_button_symbols(ctx, bounds);

  if (s_mode == MODE_EDIT && s_font_big && s_font_small) {
    /* Fields stacked vertically and centred:  MINS / <min> / <sec> / SECS.
     * A white card keeps everything readable over the frozen water-fill; the
     * active field is inverted (black box, white value). Values use full-width
     * centred rects so two digits never wrap. */
    int16_t cx = bounds.size.w / 2;
    int16_t cy = s_center_y;
    int16_t w = bounds.size.w;

    int16_t min_y = cy - 38;   /* top of minutes value rect */
    int16_t sec_y = cy - 2;    /* top of seconds value rect */
    int16_t vh = 40;           /* value/box height */

    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, GRect(cx - 54, cy - 58, 108, 116), 10, GCornersAll);

    int16_t active_y = (s_edit_field == FIELD_MIN) ? min_y : sec_y;
    graphics_context_set_fill_color(ctx, color_ink());
    graphics_fill_rect(ctx, GRect(cx - 38, active_y, 76, vh), 6, GCornersAll);

    graphics_context_set_text_color(ctx, color_ink());
    graphics_draw_text(ctx, "MINS", s_font_small, GRect(0, cy - 56, w, 16),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, "SECS", s_font_small, GRect(0, cy + 40, w, 16),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    graphics_context_set_text_color(ctx, (s_edit_field == FIELD_MIN) ? GColorWhite : color_ink());
    graphics_draw_text(ctx, s_min_buf, s_font_big, GRect(0, min_y - 4, w, vh),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    graphics_context_set_text_color(ctx, (s_edit_field == FIELD_SEC) ? GColorWhite : color_ink());
    graphics_draw_text(ctx, s_sec_buf, s_font_big, GRect(0, sec_y - 4, w, vh),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

static void update_clock_text(void) {
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);
  strftime(s_clock_text, sizeof(s_clock_text), "%H:%M", tm);
}

static void update_edit_display(void) {
  snprintf(s_min_buf, sizeof(s_min_buf), "%02ld", (long)s_edit_min);
  snprintf(s_sec_buf, sizeof(s_sec_buf), "%02ld", (long)s_edit_sec);
  if (s_deco_layer) {
    layer_mark_dirty(s_deco_layer);
  }
}

static void update_button_labels(void) {
  s_btn_up_text[0] = '\0';
  s_btn_center_text[0] = '\0';
  s_btn_down_text[0] = '\0';

  if (s_btn_up_layer) {
    text_layer_set_text(s_btn_up_layer, s_btn_up_text);
  }
  if (s_btn_center_layer) {
    text_layer_set_text(s_btn_center_layer, s_btn_center_text);
  }
  if (s_btn_down_layer) {
    text_layer_set_text(s_btn_down_layer, s_btn_down_text);
  }
  if (s_deco_layer) {
    layer_mark_dirty(s_deco_layer);
  }
}

static void apply_mode_layout(void) {
  bool edit = (s_mode == MODE_EDIT);
  if (s_c_timer_layer) {
    layer_set_hidden(text_layer_get_layer(s_c_timer_layer), edit);
  }
  if (s_b_timer_layer) {
    layer_set_hidden(text_layer_get_layer(s_b_timer_layer), !edit);
  }
  if (s_b_interval_layer) {
    layer_set_hidden(text_layer_get_layer(s_b_interval_layer), edit);
  }
  update_edit_display();
  update_button_labels();
}

static void update_ui(void) {
  update_clock_text();

  int32_t elapsed = total_elapsed();
  if (s_state.running) {
    snprintf(s_status_text, sizeof(s_status_text), "RUNNING");
  } else if (elapsed > 0) {
    snprintf(s_status_text, sizeof(s_status_text), "PAUSED");
  } else {
    snprintf(s_status_text, sizeof(s_status_text), "READY");
  }

  format_elapsed(elapsed, s_timer_text, sizeof(s_timer_text));
  format_mmss(s_state.interval_seconds, s_interval_text, sizeof(s_interval_text));

  if (s_clock_layer && strcmp(s_clock_text, s_last_clock) != 0) {
    text_layer_set_text(s_clock_layer, s_clock_text);
    strcpy(s_last_clock, s_clock_text);
  }
  if (s_status_layer && strcmp(s_status_text, s_last_status) != 0) {
    text_layer_set_text(s_status_layer, s_status_text);
    strcpy(s_last_status, s_status_text);
  }
  if (s_mode == MODE_RUN && s_c_timer_layer) {
    text_layer_set_text(s_c_timer_layer, s_timer_text);
  }
  if (s_mode == MODE_EDIT && s_b_timer_layer) {
    text_layer_set_text(s_b_timer_layer, s_timer_text);
  }
  if (s_b_interval_layer && strcmp(s_interval_text, s_last_interval) != 0) {
    text_layer_set_text(s_b_interval_layer, s_interval_text);
    strcpy(s_last_interval, s_interval_text);
  }

  if (s_water_layer) {
    int16_t fh = compute_fill_height(s_win_h);
    if (fh != s_last_fill_h) {
      s_last_fill_h = fh;
      /* Resizing the frame dirties only the old+new strip; the rest stays. */
      layer_set_frame(s_water_layer, GRect(0, s_win_h - fh, s_win_w, fh));
    }
  }
}

static void fire_vibe(void) {
  if (!s_state.running) {
    return;
  }
  int32_t now = now_seconds();
  if (now < s_state.next_vibe_epoch) {
    return;
  }
  int32_t interval = s_state.interval_seconds > 0 ? s_state.interval_seconds : INTERVAL_GRID_SECONDS;
  int32_t base = s_state.next_vibe_epoch;

  vibes_enqueue_custom_pattern(s_vibe_pattern);
  s_state.vibe_count += 1;

  int32_t diff = now - base;
  if (diff < 0) {
    diff = 0;
  }
  s_state.next_vibe_epoch = base + (diff / interval + 1) * interval;
  state_save();
  update_app_glance_safe();
}

static void ui_tick_handler(void *context) {
  s_ui_timer = NULL;
  update_ui();
  if (s_state.running && now_seconds() >= s_state.next_vibe_epoch) {
    fire_vibe();
  }
  schedule_ui_tick();
}

static void schedule_ui_tick(void) {
  if (!s_ui_timer) {
    s_ui_timer = app_timer_register(1000, ui_tick_handler, NULL);
  }
}

static void cancel_ui_tick(void) {
  if (s_ui_timer) {
    app_timer_cancel(s_ui_timer);
    s_ui_timer = NULL;
  }
}

static void start_timer(void) {
  s_state.elapsed_accum = total_elapsed();
  s_state.running = true;
  s_state.run_started_epoch = now_seconds();
  s_state.next_vibe_epoch = next_vibe_after(now_seconds());
  s_frozen_cycle_elapsed = 0;
  state_save();
  cancel_ui_tick();
  schedule_ui_tick();
  update_app_glance_safe();
  update_ui();
}

static void pause_timer(void) {
  int32_t interval = s_state.interval_seconds > 0 ? s_state.interval_seconds : 1;
  int32_t left = secs_to_vibe();
  if (left > interval) {
    left = interval;
  }
  s_frozen_cycle_elapsed = interval - left;
  s_state.elapsed_accum = total_elapsed();
  s_state.running = false;
  s_state.run_started_epoch = 0;
  state_save();
  cancel_pending_wakeup();
  update_app_glance_safe();
  update_ui();
}

static void reset_timer(void) {
  s_state.running = false;
  s_state.elapsed_accum = 0;
  s_state.run_started_epoch = 0;
  s_state.next_vibe_epoch = 0;
  s_state.vibe_count = 0;
  s_frozen_cycle_elapsed = 0;
  state_save();
  cancel_pending_wakeup();
  update_app_glance_safe();
  update_ui();
}

static void resume_timer(void) {
  /* Continue the frozen cycle from where it paused (water resumes mid-level),
   * unlike start_timer which begins a fresh, full cycle. */
  int32_t interval = s_state.interval_seconds > 0 ? s_state.interval_seconds : 1;
  int32_t left = interval - s_frozen_cycle_elapsed;
  if (left < 0) {
    left = 0;
  }
  if (left > interval || left == 0) {
    left = interval;
  }
  s_state.elapsed_accum = total_elapsed();
  s_state.running = true;
  s_state.run_started_epoch = now_seconds();
  s_state.next_vibe_epoch = now_seconds() + left;
  state_save();
  cancel_ui_tick();
  schedule_ui_tick();
  update_app_glance_safe();
  update_ui();
}

static void apply_interval(int32_t total_seconds) {
  s_state.interval_seconds = clamp_interval(total_seconds);
  if (s_state.running) {
    s_state.next_vibe_epoch = next_vibe_after(now_seconds());
  }
  state_save();
  update_app_glance_safe();
  s_last_fill_h = -1;
  update_ui();
}

static void reset_edit_timeout(void) {
  if (s_edit_timeout) {
    app_timer_cancel(s_edit_timeout);
  }
  s_edit_timeout = app_timer_register(EDIT_TIMEOUT_MS, edit_timeout_handler, NULL);
}

static void cancel_edit_timeout(void) {
  if (s_edit_timeout) {
    app_timer_cancel(s_edit_timeout);
    s_edit_timeout = NULL;
  }
}

static void edit_timeout_handler(void *context) {
  s_edit_timeout = NULL;
  int32_t total = s_edit_min * 60 + s_edit_sec;
  if (total != s_state.interval_seconds) {
    apply_interval(total);
  }
  s_mode = MODE_RUN;
  apply_mode_layout();
}

static void enter_edit_mode(void) {
  s_mode = MODE_EDIT;
  s_edit_field = FIELD_MIN;
  s_edit_min = s_state.interval_seconds / 60;
  s_edit_sec = s_state.interval_seconds % 60;
  apply_mode_layout();
  reset_edit_timeout();
}

static void commit_edit_and_run(void) {
  cancel_edit_timeout();
  int32_t total = s_edit_min * 60 + s_edit_sec;
  if (total != s_state.interval_seconds) {
    apply_interval(total);
  }
  s_mode = MODE_RUN;
  apply_mode_layout();
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_mode == MODE_EDIT) {
    if (s_edit_field == FIELD_MIN) {
      s_edit_min = (s_edit_min + 1) % 100;
    } else {
      s_edit_sec = (s_edit_sec + SEC_STEP) % 60;
    }
    update_edit_display();
    reset_edit_timeout();
  } else {
    if (s_state.running) {
      pause_timer();
    } else if (total_elapsed() > 0) {
      resume_timer();
    } else {
      start_timer();
    }
    update_button_labels();
  }
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_mode == MODE_EDIT) {
    if (s_edit_field == FIELD_MIN) {
      s_edit_min = (s_edit_min + 99) % 100;
    } else {
      s_edit_sec = (s_edit_sec + (60 - SEC_STEP)) % 60;
    }
    update_edit_display();
    reset_edit_timeout();
  } else {
    if (s_state.running || total_elapsed() > 0) {
      reset_timer();
    } else {
      start_timer();
    }
    update_button_labels();
  }
}

static void center_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_mode == MODE_EDIT) {
    if (s_edit_field == FIELD_MIN) {
      s_edit_field = FIELD_SEC;
      update_edit_display();
      update_button_labels();
      reset_edit_timeout();
    } else {
      commit_edit_and_run();
    }
  } else {
    enter_edit_mode();
  }
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, center_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
}

static TextLayer *make_label(Layer *parent, GRect frame, GTextAlignment align, const char *font_key, GColor color) {
  TextLayer *t = text_layer_create(frame);
  text_layer_set_background_color(t, GColorClear);
  text_layer_set_text_color(t, color);
  text_layer_set_text_alignment(t, align);
  text_layer_set_font(t, fonts_get_system_font(font_key));
  layer_add_child(parent, text_layer_get_layer(t));
  return t;
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  compute_layout(bounds);

  s_font_big = fonts_get_system_font(FONT_KEY_LECO_32_BOLD_NUMBERS);
  s_font_small = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

  s_path_play = gpath_create(&s_info_play);
  s_path_reset = gpath_create(&s_info_reset);
  s_path_up = gpath_create(&s_info_up);
  s_path_down = gpath_create(&s_info_down);

  /* Water sits at the bottom; its frame grows/shrinks so only the changed
   * strip repaints. Deco (glyphs + edit panel) is a transparent layer above. */
  s_water_layer = layer_create(GRect(0, bounds.size.h, bounds.size.w, 0));
  layer_set_update_proc(s_water_layer, water_update_proc);
  layer_add_child(window_layer, s_water_layer);

  s_deco_layer = layer_create(bounds);
  layer_set_update_proc(s_deco_layer, deco_update_proc);
  layer_add_child(window_layer, s_deco_layer);

  s_clock_layer = make_label(window_layer, GRect(0, 3, bounds.size.w, 20),
                             GTextAlignmentCenter, FONT_KEY_LECO_20_BOLD_NUMBERS, color_ink());
  s_status_layer = make_label(window_layer, GRect(0, 24, bounds.size.w, 20),
                              GTextAlignmentCenter, FONT_KEY_GOTHIC_18_BOLD, color_ink());

  s_c_timer_layer = make_label(window_layer, GRect(0, s_center_y - 22, bounds.size.w, 44),
                               GTextAlignmentCenter, FONT_KEY_LECO_42_NUMBERS, color_ink());

  s_b_timer_layer = make_label(window_layer, GRect(0, bounds.size.h - 32, bounds.size.w, 26),
                               GTextAlignmentCenter, FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM, color_ink());
  s_b_interval_layer = make_label(window_layer, GRect(0, bounds.size.h - 32, bounds.size.w, 26),
                                  GTextAlignmentCenter, FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM, color_ink());

  int16_t btn_x = bounds.size.w - 44;
  s_btn_up_layer = make_label(window_layer, GRect(btn_x, 6, 40, 18),
                              GTextAlignmentRight, FONT_KEY_GOTHIC_14_BOLD, color_ink());
  s_btn_center_layer = make_label(window_layer, GRect(btn_x, s_center_y - 9, 40, 18),
                                  GTextAlignmentRight, FONT_KEY_GOTHIC_14_BOLD, color_ink());
  s_btn_down_layer = make_label(window_layer, GRect(btn_x, bounds.size.h - 26, 40, 18),
                                GTextAlignmentRight, FONT_KEY_GOTHIC_14_BOLD, color_ink());

  apply_mode_layout();
  update_ui();

  window_set_click_config_provider(window, click_config_provider);
  schedule_ui_tick();
}

static void main_window_unload(Window *window) {
  cancel_ui_tick();
  cancel_edit_timeout();
  text_layer_destroy(s_btn_down_layer);
  text_layer_destroy(s_btn_center_layer);
  text_layer_destroy(s_btn_up_layer);
  text_layer_destroy(s_b_interval_layer);
  text_layer_destroy(s_b_timer_layer);
  text_layer_destroy(s_c_timer_layer);
  text_layer_destroy(s_status_layer);
  text_layer_destroy(s_clock_layer);
  layer_destroy(s_deco_layer);
  layer_destroy(s_water_layer);
  s_btn_down_layer = NULL;
  s_btn_center_layer = NULL;
  s_btn_up_layer = NULL;
  s_b_interval_layer = NULL;
  s_b_timer_layer = NULL;
  s_c_timer_layer = NULL;
  s_status_layer = NULL;
  s_clock_layer = NULL;
  s_water_layer = NULL;
  s_deco_layer = NULL;
  s_font_big = NULL;
  s_font_small = NULL;
  gpath_destroy(s_path_play);
  gpath_destroy(s_path_reset);
  gpath_destroy(s_path_up);
  gpath_destroy(s_path_down);
  s_path_play = NULL;
  s_path_reset = NULL;
  s_path_up = NULL;
  s_path_down = NULL;
}

static void wakeup_handler(WakeupId id, int32_t cookie) {
  fire_vibe();
}

static void push_main_window(void) {
  s_main_window = window_create();
  window_set_background_color(s_main_window, GColorWhite);
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });
  window_stack_push(s_main_window, true);
}

static void init(void) {
  state_load_or_default();

  wakeup_service_subscribe(wakeup_handler);

  s_launched_by_wakeup = (launch_reason() == APP_LAUNCH_WAKEUP);

  if (s_launched_by_wakeup) {
    WakeupId id = 0;
    int32_t cookie = 0;
    wakeup_get_launch_event(&id, &cookie);
    fire_vibe();
    s_mode = MODE_RUN;
    s_edit_field = FIELD_MIN;
    push_main_window();
    schedule_ui_tick();
    update_app_glance_safe();
  } else {
    /* Manual launch (foreground). If a timer session is still active -- either
     * running, or paused with elapsed time (a paused timer counts as active) --
     * restore it and continue where it left off. run_started_epoch and
     * next_vibe_epoch are absolute, so a running timer keeps advancing across
     * the time the app spent in the background. Only when the timer was reset
     * to zero (READY) do we fall back to the default running editor. */
    cancel_pending_wakeup();
    bool was_active = s_state.running || s_state.elapsed_accum > 0;
    if (was_active) {
      s_mode = MODE_RUN;
      s_edit_field = FIELD_MIN;
      push_main_window();
      schedule_ui_tick();
      update_app_glance_safe();
    } else {
      /* Default: open the editor with the timer already running from a full
       * cycle, so vibrations fire at the (default) interval until the user
       * changes it. Only the interval optionally persists (REMEMBER_INTERVAL). */
#if !REMEMBER_INTERVAL
      s_state.interval_seconds = DEFAULT_INTERVAL_SECONDS;
#endif
      s_state.running = true;
      s_state.elapsed_accum = 0;
      s_state.run_started_epoch = now_seconds();
      s_state.next_vibe_epoch = next_vibe_after(now_seconds());
      s_state.vibe_count = 0;
      s_frozen_cycle_elapsed = 0;
      state_save();
      s_mode = MODE_EDIT;
      s_edit_field = FIELD_MIN;
      push_main_window();
      enter_edit_mode();
      schedule_ui_tick();
      update_app_glance_safe();
    }
  }
}

static void deinit(void) {
  if (s_state.running) {
    schedule_wakeup_for_next();
  }
  while (window_stack_get_top_window() && window_stack_get_top_window() != s_main_window) {
    window_stack_pop(false);
  }
  if (s_main_window) {
    window_destroy(s_main_window);
  }
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
