#include <pebble.h>
#include <string.h>

#define MAX_INTERVAL_SECONDS (99 * 60 + 59)
#define EDIT_TIMEOUT_MS 15000
#define AUTOROLL_DELAY_MS 2000     /* hold UP/DOWN this long in edit mode to auto-roll */
#define AUTOROLL_INTERVAL_MS 110   /* roll speed once it kicks in */

/* Factory defaults for the phone-configurable settings (Pebble app config page).
 * The live values are the g_* globals, loaded from persist / set via AppMessage. */
#define DEFAULT_INTERVAL_DEFAULT (5 * 60)
#define DEFAULT_MIN_STEP 15
#define DEFAULT_WATER_COLOR_HEX 0x55AAFF

/* AppMessage keys -- must match the indices in appinfo.json "appKeys". */
#define MSG_INTERVAL_DEFAULT 0
#define MSG_MIN_STEP 1
#define MSG_WATER_COLOR 2

/* 0: every fresh launch starts at the configured default (stateless).
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
  PERSIST_FROZEN_CYCLE = 8,
  /* Settings (separate range from the timer state above). */
  PERSIST_SET_DEFAULT_INTERVAL = 20,
  PERSIST_SET_MIN_STEP = 21,
  PERSIST_SET_WATER_COLOR = 22
};

enum { MODE_EDIT, MODE_RUN };
enum { FIELD_MIN, FIELD_SEC };

typedef struct {
  bool running;
  int32_t elapsed_accum;
  int32_t run_started_epoch;
  int32_t interval_seconds;
  int32_t next_vibe_epoch;
} TimerState;

static Window *s_main_window;
static Layer *s_water_layer;
static Layer *s_deco_layer;
static TextLayer *s_clock_layer;
static TextLayer *s_status_layer;
static TextLayer *s_c_timer_layer;
static TextLayer *s_b_timer_layer;
static TextLayer *s_b_interval_layer;
static AppTimer *s_edit_timeout;
static AppTimer *s_autoroll_timer;
static int s_autoroll_dir;          /* +1 / -1 while auto-rolling, 0 otherwise */
static TimerState s_state;
static int32_t s_frozen_cycle_elapsed;

/* Live, phone-configurable settings (see DEFAULT_* above). */
static int32_t g_default_interval = DEFAULT_INTERVAL_DEFAULT;
static int32_t g_min_step = DEFAULT_MIN_STEP;   /* seconds editor step + min interval */
static uint32_t g_water_color_hex = DEFAULT_WATER_COLOR_HEX;
static int s_mode;
static int s_edit_field;
static int32_t s_edit_min;
static int32_t s_edit_sec;

static int s_center_y;
static int s_win_w;
static int s_win_h;
static GFont s_font_big;
static GFont s_font_small;

/* Centre count-up timer has two presentations swapped at the 1h mark: a big
 * MM:SS (<1h) and a smaller HH:MM:SS (>=1h) that still clears the centre glyph.
 * Both sized per screen width at window load; box height tracks the font. */
static GFont s_timer_font_long;
static GFont s_timer_font_short;
static GRect s_timer_frame_long;
static GRect s_timer_frame_short;
static int s_timer_form;  /* -1 unset, 0 short (MM:SS), 1 long (HH:MM:SS) */

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

static const uint32_t s_start_vibe_durations[] = { 50 };
static const VibePattern s_start_vibe_pattern = {
  .durations = s_start_vibe_durations,
  .num_segments = 1
};

static void update_ui(void);
static void update_app_glance_safe(void);
static void apply_mode_layout(void);
static void update_edit_display(void);
static void update_button_labels(void);
static void edit_timeout_handler(void *context);
static void autoroll_stop(void);

static int32_t clamp_interval(int32_t interval_seconds) {
  if (interval_seconds < g_min_step) {
    return g_min_step;
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
  int32_t interval = s_state.interval_seconds > 0 ? s_state.interval_seconds : g_min_step;
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
  /* Count up as a 24h clock: rolls back to 0 after 23:59:59. Show MM:SS until the
   * first hour, then HH:MM:SS -- so the sub-hour case can use a bigger font. */
  elapsed_seconds %= 86400;
  int32_t hours = elapsed_seconds / 3600;
  int32_t minutes = (elapsed_seconds / 60) % 60;
  int32_t seconds = elapsed_seconds % 60;
  if (hours > 0) {
    snprintf(buffer, buffer_size, "%02ld:%02ld:%02ld", (long)hours, (long)minutes, (long)seconds);
  } else {
    snprintf(buffer, buffer_size, "%02ld:%02ld", (long)minutes, (long)seconds);
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
  persist_write_int(PERSIST_FROZEN_CYCLE, s_frozen_cycle_elapsed);
}

static bool is_valid_step(int32_t v) {
  return v == 1 || v == 5 || v == 10 || v == 15 || v == 20 || v == 30;
}

static void settings_sanitize(void) {
  if (!is_valid_step(g_min_step)) {
    g_min_step = DEFAULT_MIN_STEP;
  }
  if (g_default_interval < g_min_step) {
    g_default_interval = g_min_step;
  }
  if (g_default_interval > MAX_INTERVAL_SECONDS) {
    g_default_interval = MAX_INTERVAL_SECONDS;
  }
}

static void settings_load(void) {
  if (persist_exists(PERSIST_SET_DEFAULT_INTERVAL)) {
    g_default_interval = persist_read_int(PERSIST_SET_DEFAULT_INTERVAL);
  }
  if (persist_exists(PERSIST_SET_MIN_STEP)) {
    g_min_step = persist_read_int(PERSIST_SET_MIN_STEP);
  }
  if (persist_exists(PERSIST_SET_WATER_COLOR)) {
    g_water_color_hex = (uint32_t)persist_read_int(PERSIST_SET_WATER_COLOR);
  }
  settings_sanitize();
}

static void settings_save(void) {
  persist_write_int(PERSIST_SET_DEFAULT_INTERVAL, g_default_interval);
  persist_write_int(PERSIST_SET_MIN_STEP, g_min_step);
  persist_write_int(PERSIST_SET_WATER_COLOR, (int32_t)g_water_color_hex);
}

static void state_load_or_default(void) {
  if (!persist_exists(PERSIST_INITIALIZED)) {
    /* First ever launch: start in a READY (reset-to-zero) state so init's
     * manual-launch path treats it as inactive and opens the default editor. */
    s_state.running = false;
    s_state.elapsed_accum = 0;
    s_state.run_started_epoch = 0;
    s_state.interval_seconds = g_default_interval;
    s_state.next_vibe_epoch = 0;
    s_frozen_cycle_elapsed = 0;
    state_save();
    return;
  }

  s_state.running = persist_read_int(PERSIST_RUNNING) != 0;
  s_state.elapsed_accum = persist_read_int(PERSIST_ELAPSED_ACCUM);
  s_state.run_started_epoch = persist_read_int(PERSIST_RUN_STARTED);
  s_state.interval_seconds = persist_read_int(PERSIST_INTERVAL);
  if (s_state.interval_seconds == 0) {
    s_state.interval_seconds = g_default_interval;
  }
  s_state.interval_seconds = clamp_interval(s_state.interval_seconds);
  s_state.next_vibe_epoch = persist_read_int(PERSIST_NEXT_VIBE_EPOCH);
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
  return GColorFromHEX(g_water_color_hex);
#else
  return GColorLightGray;  /* B/W platforms ignore the colour setting */
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

/* Button hints are glyphs drawn in deco_update_proc; just trigger its redraw when
 * the state that selects them (mode / running / active field) changes. */
static void update_button_labels(void) {
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
  /* Refresh now so the layer that just became visible (e.g. the centre timer on
   * Edit->Run) shows its text immediately instead of waiting for the next tick. */
  update_ui();
}

/* Swap the centre timer between the big MM:SS and the smaller HH:MM:SS font/box
 * at the 1h boundary. Guarded so the font/frame only change on the crossing. */
static void set_central_form(bool long_form) {
  int form = long_form ? 1 : 0;
  if (form == s_timer_form || !s_c_timer_layer) {
    return;
  }
  s_timer_form = form;
  layer_set_frame(text_layer_get_layer(s_c_timer_layer),
                  long_form ? s_timer_frame_long : s_timer_frame_short);
  text_layer_set_font(s_c_timer_layer, long_form ? s_timer_font_long : s_timer_font_short);
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
  set_central_form((elapsed % 86400) >= 3600);

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
    /* Live in every mode: water keeps draining while running (even in edit); when
     * paused, compute_live_fill_height holds the frozen cycle. */
    int16_t fh = compute_live_fill_height(s_win_h);
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
  int32_t interval = s_state.interval_seconds > 0 ? s_state.interval_seconds : g_min_step;
  int32_t base = s_state.next_vibe_epoch;

  vibes_enqueue_custom_pattern(s_vibe_pattern);

  int32_t diff = now - base;
  if (diff < 0) {
    diff = 0;
  }
  s_state.next_vibe_epoch = base + (diff / interval + 1) * interval;
  /* Only next_vibe_epoch changed here; write just that key instead of the whole
   * state to spare flash writes on short intervals. */
  persist_write_int(PERSIST_NEXT_VIBE_EPOCH, s_state.next_vibe_epoch);
  update_app_glance_safe();
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_ui();
  if (s_state.running && now_seconds() >= s_state.next_vibe_epoch) {
    fire_vibe();
  }
}

/* Display tick = OS TickTimerService (RTC-aligned, fires on the real second/
 * minute boundary). SECOND_UNIT while counting (elapsed, water drain, vibe
 * check); MINUTE_UNIT when paused/READY where only the clock changes, so an idle
 * foreground screen wakes once a minute, not 60x. Re-subscribing swaps the unit. */
static void apply_tick_unit(void) {
  tick_timer_service_subscribe(s_state.running ? SECOND_UNIT : MINUTE_UNIT, tick_handler);
}

static void start_timer(void) {
  s_state.elapsed_accum = total_elapsed();
  s_state.running = true;
  s_state.run_started_epoch = now_seconds();
  s_state.next_vibe_epoch = next_vibe_after(now_seconds());
  s_frozen_cycle_elapsed = 0;
  vibes_enqueue_custom_pattern(s_start_vibe_pattern);
  state_save();
  apply_tick_unit();
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
  apply_tick_unit();
  update_app_glance_safe();
  update_ui();
}

static void reset_timer(void) {
  s_state.running = false;
  s_state.elapsed_accum = 0;
  s_state.run_started_epoch = 0;
  s_state.next_vibe_epoch = 0;
  s_frozen_cycle_elapsed = 0;
  state_save();
  cancel_pending_wakeup();
  apply_tick_unit();
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
  apply_tick_unit();
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

/* Phone config page -> watch. Each key is optional; apply only what arrived. */
static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  bool sched_changed = false;  /* interval default or min step */
  bool color_changed = false;

  Tuple *t = dict_find(iter, MSG_MIN_STEP);
  if (t && is_valid_step(t->value->int32)) {
    g_min_step = t->value->int32;
    sched_changed = true;
  }
  t = dict_find(iter, MSG_INTERVAL_DEFAULT);
  if (t) {
    g_default_interval = t->value->int32;
    sched_changed = true;
  }
  t = dict_find(iter, MSG_WATER_COLOR);
  if (t) {
    g_water_color_hex = (uint32_t)t->value->int32;
    color_changed = true;
  }

  if (!sched_changed && !color_changed) {
    return;
  }
  settings_sanitize();
  settings_save();
  /* Adopt the new default as the live interval (re-clamped to the new minimum,
   * which re-syncs the vibe schedule) ONLY from a no-progress state -- stopped,
   * reset, or freshly launched all have elapsed == 0. A running or paused session
   * with progress keeps its on-watch interval; the new default still takes effect
   * at the next fresh start / launch. */
  if (sched_changed && total_elapsed() == 0) {
    apply_interval(g_default_interval);
  }
  if (color_changed && s_water_layer) {
    layer_mark_dirty(s_water_layer);
  }
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
  autoroll_stop();
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
  /* Snap the seconds onto the configured step grid so on-watch toggling stays in
   * clean multiples (e.g. step 20 -> 0/20/40), even if the phone set an off-grid
   * default. All allowed steps divide 60. */
  s_edit_sec = ((s_state.interval_seconds % 60) / g_min_step) * g_min_step;
  apply_mode_layout();
  reset_edit_timeout();
}

static void autoroll_stop(void) {
  s_autoroll_dir = 0;
  if (s_autoroll_timer) {
    app_timer_cancel(s_autoroll_timer);
    s_autoroll_timer = NULL;
  }
}

static void commit_edit_and_run(void) {
  autoroll_stop();
  cancel_edit_timeout();
  int32_t total = s_edit_min * 60 + s_edit_sec;
  if (total != s_state.interval_seconds) {
    apply_interval(total);
  }
  s_mode = MODE_RUN;
  apply_mode_layout();
}

/* One step of the active edit field; dir = +1 (UP) / -1 (DOWN). Seconds move by
 * the configured step, minutes by 1; both wrap. Shared by taps and auto-roll. */
static void edit_adjust(int dir) {
  if (s_edit_field == FIELD_MIN) {
    s_edit_min = (s_edit_min + (dir > 0 ? 1 : 99)) % 100;
  } else if (dir > 0) {
    s_edit_sec = (s_edit_sec + g_min_step) % 60;
  } else {
    s_edit_sec = (s_edit_sec + (60 - g_min_step)) % 60;
  }
  update_edit_display();
  reset_edit_timeout();
}

static void autoroll_tick(void *context) {
  s_autoroll_timer = NULL;
  if (s_mode != MODE_EDIT || s_autoroll_dir == 0) {
    return;
  }
  edit_adjust(s_autoroll_dir);
  s_autoroll_timer = app_timer_register(AUTOROLL_INTERVAL_MS, autoroll_tick, NULL);
}

/* Long-press (held AUTOROLL_DELAY_MS) in edit mode starts the auto-roll; release
 * stops it. A short tap never reaches here -- the single-click handler fires. */
static void autoroll_start(int dir) {
  if (s_mode != MODE_EDIT) {
    return;
  }
  s_autoroll_dir = dir;
  if (s_autoroll_timer) {
    app_timer_cancel(s_autoroll_timer);
    s_autoroll_timer = NULL;
  }
  autoroll_tick(NULL);
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_mode == MODE_EDIT) {
    edit_adjust(+1);
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
    edit_adjust(-1);
  } else {
    if (s_state.running || total_elapsed() > 0) {
      reset_timer();
    } else {
      start_timer();
    }
    update_button_labels();
  }
}

static void up_long_down_handler(ClickRecognizerRef recognizer, void *context) {
  autoroll_start(+1);
}

static void down_long_down_handler(ClickRecognizerRef recognizer, void *context) {
  autoroll_start(-1);
}

static void long_up_handler(ClickRecognizerRef recognizer, void *context) {
  autoroll_stop();
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
  /* Hold UP/DOWN to auto-roll the edit fields (guarded to edit mode in the
   * handlers). A short tap fires the single-click handler instead. */
  window_long_click_subscribe(BUTTON_ID_UP, AUTOROLL_DELAY_MS, up_long_down_handler, long_up_handler);
  window_long_click_subscribe(BUTTON_ID_DOWN, AUTOROLL_DELAY_MS, down_long_down_handler, long_up_handler);
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

  /* Centre timer shares its row with the centre action glyph, so the biggest font
   * that stays screen-centred without touching it depends on both screen width
   * AND the string length. The shorter MM:SS (<1h) fits a much bigger font than
   * the 8-char HH:MM:SS (>=1h). Pick both per width; box height tracks the font so
   * the number stays vertically centred on the glyph row. */
  const char *long_key, *short_key;
  int16_t long_h, short_h;
  if (bounds.size.w >= 200) {          /* emery */
    long_key = FONT_KEY_LECO_32_BOLD_NUMBERS;       long_h = 32;
    short_key = FONT_KEY_LECO_42_NUMBERS;           short_h = 42;
  } else if (bounds.size.w >= 180) {   /* chalk */
    long_key = FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM; long_h = 26;
    short_key = FONT_KEY_LECO_42_NUMBERS;           short_h = 42;
  } else {                             /* 144px */
    long_key = FONT_KEY_LECO_20_BOLD_NUMBERS;       long_h = 20;
    short_key = FONT_KEY_LECO_32_BOLD_NUMBERS;      short_h = 32;
  }
  int16_t long_box_h = long_h + 2, short_box_h = short_h + 2;
  s_timer_font_long  = fonts_get_system_font(long_key);
  s_timer_font_short = fonts_get_system_font(short_key);
  s_timer_frame_long  = GRect(0, s_center_y - long_box_h / 2,  bounds.size.w, long_box_h);
  s_timer_frame_short = GRect(0, s_center_y - short_box_h / 2, bounds.size.w, short_box_h);
  s_timer_form = -1;  /* force set_central_form to apply the right one on first update */
  s_c_timer_layer = make_label(window_layer, s_timer_frame_short,
                               GTextAlignmentCenter, short_key, color_ink());

  s_b_timer_layer = make_label(window_layer, GRect(0, bounds.size.h - 32, bounds.size.w, 26),
                               GTextAlignmentCenter, FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM, color_ink());
  s_b_interval_layer = make_label(window_layer, GRect(0, bounds.size.h - 32, bounds.size.w, 26),
                                  GTextAlignmentCenter, FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM, color_ink());

  apply_mode_layout();  /* also refreshes the UI (calls update_ui) */

  window_set_click_config_provider(window, click_config_provider);
  apply_tick_unit();
}

static void main_window_unload(Window *window) {
  tick_timer_service_unsubscribe();
  autoroll_stop();
  cancel_edit_timeout();
  text_layer_destroy(s_b_interval_layer);
  text_layer_destroy(s_b_timer_layer);
  text_layer_destroy(s_c_timer_layer);
  text_layer_destroy(s_status_layer);
  text_layer_destroy(s_clock_layer);
  layer_destroy(s_deco_layer);
  layer_destroy(s_water_layer);
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
  settings_load();        /* must precede state load: it seeds the default interval */
  state_load_or_default();

  app_message_register_inbox_received(inbox_received_handler);
  /* Inbox holds the 3 settings keys; outbox is minimal -- the watch only
   * receives config, it never sends an AppMessage. */
  app_message_open(256, 16);

  wakeup_service_subscribe(wakeup_handler);

  bool launched_by_wakeup = (launch_reason() == APP_LAUNCH_WAKEUP);

  if (launched_by_wakeup) {
    WakeupId id = 0;
    int32_t cookie = 0;
    wakeup_get_launch_event(&id, &cookie);
    fire_vibe();
    s_mode = MODE_RUN;
    s_edit_field = FIELD_MIN;
    push_main_window();
    apply_tick_unit();
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
      apply_tick_unit();
      update_app_glance_safe();
    } else {
      /* Default: open the editor with the timer already running from a full
       * cycle, so vibrations fire at the (default) interval until the user
       * changes it. Only the interval optionally persists (REMEMBER_INTERVAL). */
#if !REMEMBER_INTERVAL
      s_state.interval_seconds = g_default_interval;
#endif
      s_state.running = true;
      s_state.elapsed_accum = 0;
      s_state.run_started_epoch = now_seconds();
      s_state.next_vibe_epoch = next_vibe_after(now_seconds());
      s_frozen_cycle_elapsed = 0;
      vibes_enqueue_custom_pattern(s_start_vibe_pattern);
      state_save();
      s_mode = MODE_EDIT;
      s_edit_field = FIELD_MIN;
      push_main_window();
      enter_edit_mode();
      apply_tick_unit();
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
