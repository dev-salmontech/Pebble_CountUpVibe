#include <pebble.h>

#define DEFAULT_INTERVAL_SECONDS (5 * 60)
#define MIN_INTERVAL_SECONDS 10
#define MAX_INTERVAL_SECONDS (99 * 60 + 59)

#define POPUP_AUTO_DISMISS_MS 2400
#define POPUP_ANIM_STEP_MS 33
#define POPUP_ANIM_STEP (0.066f)

#define COOKIE_VIBE 1

enum {
  PERSIST_INITIALIZED = 1,
  PERSIST_RUNNING = 2,
  PERSIST_ELAPSED_ACCUM = 3,
  PERSIST_RUN_STARTED = 4,
  PERSIST_INTERVAL = 5,
  PERSIST_NEXT_VIBE_EPOCH = 6,
  PERSIST_VIBE_COUNT = 7
};

typedef struct {
  bool running;
  int32_t elapsed_accum;
  int32_t run_started_epoch;
  int32_t interval_seconds;
  int32_t next_vibe_epoch;
  int32_t vibe_count;
} TimerState;

static Window *s_main_window;
static Window *s_picker_window;
static Window *s_popup_window;
static Layer *s_canvas_layer;
static Layer *s_popup_canvas_layer;
static Layer *s_picker_canvas_layer;
static TextLayer *s_clock_layer;
static TextLayer *s_status_layer;
static TextLayer *s_hero_layer;
static TextLayer *s_next_layer;
static TextLayer *s_hint_layer;
static TextLayer *s_popup_title_layer;
static TextLayer *s_popup_sub_layer;
static TextLayer *s_picker_title_layer;
static TextLayer *s_pick_min_layer;
static TextLayer *s_pick_sec_layer;
static TextLayer *s_pick_min_label_layer;
static TextLayer *s_pick_sec_label_layer;
static AppTimer *s_ui_timer;
static AppTimer *s_popup_anim_timer;
static AppTimer *s_popup_dismiss_timer;
static TimerState s_state;
static bool s_launched_by_wakeup;
static float s_popup_t;
static int s_ring_cx, s_ring_cy, s_ring_r, s_ring_thick;

static int32_t s_pick_minutes;
static int32_t s_pick_seconds;
static int s_pick_field;

static char s_clock_text[16];
static char s_status_text[16];
static char s_hero_text[16];
static char s_next_text[24];
static char s_hint_text[64];
static char s_glance_text[96];
static char s_popup_title_text[24];
static char s_popup_sub_text[32];
static char s_pick_min_buf[8];
static char s_pick_sec_buf[8];

static const uint32_t s_vibe_durations[] = { 150, 90, 320 };
static const VibePattern s_vibe_pattern = {
  .durations = s_vibe_durations,
  .num_segments = 3
};

static void update_ui(void);
static void schedule_ui_tick(void);
static void show_popup(void);
static void fire_vibe(void);
static void update_app_glance_safe(void);

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
}

static void state_load_or_default(void) {
  if (!persist_exists(PERSIST_INITIALIZED)) {
    s_state.running = true;
    s_state.elapsed_accum = 0;
    s_state.run_started_epoch = now_seconds();
    s_state.interval_seconds = DEFAULT_INTERVAL_SECONDS;
    s_state.next_vibe_epoch = now_seconds() + DEFAULT_INTERVAL_SECONDS;
    s_state.vibe_count = 0;
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

static GColor color_bg(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x0A1A1F);
#else
  return GColorBlack;
#endif
}
static GColor color_teal(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x2BD4C4);
#else
  return GColorWhite;
#endif
}
static GColor color_amber(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0xFFC857);
#else
  return GColorWhite;
#endif
}
static GColor color_dim(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x1F3A3F);
#else
  return GColorDarkGray;
#endif
}
static GColor color_text(void) {
  return GColorWhite;
}
static GColor color_sub(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x9FB8BC);
#else
  return GColorLightGray;
#endif
}

static void compute_ring_geometry(GRect bounds) {
  int16_t short_side = bounds.size.w < bounds.size.h ? bounds.size.w : bounds.size.h;
  s_ring_r = short_side / 2 - 34;
  if (s_ring_r < 30) {
    s_ring_r = 30;
  }
  s_ring_thick = s_ring_r * 18 / 100;
  if (s_ring_thick < 6) {
    s_ring_thick = 6;
  }
  s_ring_cx = bounds.size.w / 2;
  s_ring_cy = bounds.size.h / 2;
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, color_bg());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  compute_ring_geometry(bounds);

  GRect oval = GRect(s_ring_cx - s_ring_r, s_ring_cy - s_ring_r, s_ring_r * 2, s_ring_r * 2);

  graphics_context_set_fill_color(ctx, color_dim());
  graphics_fill_radial(ctx, oval, GOvalScaleModeFillCircle, s_ring_thick, 0, TRIG_MAX_ANGLE);

  int32_t interval = s_state.interval_seconds > 0 ? s_state.interval_seconds : 1;
  int32_t left = secs_to_vibe();
  if (left > interval) {
    left = interval;
  }
  int32_t elapsed_in_cycle = interval - left;
  int32_t fraction = elapsed_in_cycle * TRIG_MAX_ANGLE / interval;
  if (fraction < 0) {
    fraction = 0;
  }
  if (fraction > TRIG_MAX_ANGLE) {
    fraction = TRIG_MAX_ANGLE;
  }

  int32_t start = -TRIG_MAX_ANGLE / 4;
  graphics_context_set_fill_color(ctx, color_teal());
  graphics_fill_radial(ctx, oval, GOvalScaleModeFillCircle, s_ring_thick, start, start + fraction);

  if (fraction > 0) {
    graphics_context_set_fill_color(ctx, color_amber());
    graphics_fill_radial(ctx, oval, GOvalScaleModeFillCircle, s_ring_thick,
                         start + fraction - TRIG_MAX_ANGLE / 90,
                         start + fraction + TRIG_MAX_ANGLE / 90);
  }
}

static void update_clock_text(void) {
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);
  strftime(s_clock_text, sizeof(s_clock_text), "%H:%M", tm);
}

static void update_ui(void) {
  update_clock_text();
  format_elapsed(total_elapsed(), s_hero_text, sizeof(s_hero_text));

  if (s_state.running) {
    snprintf(s_status_text, sizeof(s_status_text), "RUNNING");
    char left_text[12];
    format_mmss(secs_to_vibe(), left_text, sizeof(left_text));
    snprintf(s_next_text, sizeof(s_next_text), "next %s", left_text);
    snprintf(s_hint_text, sizeof(s_hint_text), "SELECT pause   UP set\nDOWN reset");
  } else if (total_elapsed() > 0) {
    snprintf(s_status_text, sizeof(s_status_text), "PAUSED");
    snprintf(s_next_text, sizeof(s_next_text), "paused");
    snprintf(s_hint_text, sizeof(s_hint_text), "SELECT resume  UP set\nDOWN reset");
  } else {
    snprintf(s_status_text, sizeof(s_status_text), "READY");
    char interval_text[12];
    format_mmss(s_state.interval_seconds, interval_text, sizeof(interval_text));
    snprintf(s_next_text, sizeof(s_next_text), "every %s", interval_text);
    snprintf(s_hint_text, sizeof(s_hint_text), "SELECT start   UP set\nDOWN reset");
  }

  if (s_clock_layer) {
    text_layer_set_text(s_clock_layer, s_clock_text);
  }
  if (s_status_layer) {
    text_layer_set_text(s_status_layer, s_status_text);
  }
  if (s_hero_layer) {
    text_layer_set_text(s_hero_layer, s_hero_text);
  }
  if (s_next_layer) {
    text_layer_set_text(s_next_layer, s_next_text);
  }
  if (s_hint_layer) {
    text_layer_set_text(s_hint_layer, s_hint_text);
  }
  if (s_canvas_layer) {
    layer_mark_dirty(s_canvas_layer);
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

  vibes_enqueue_custom_pattern(&s_vibe_pattern);
  s_state.vibe_count += 1;
  s_state.next_vibe_epoch = now + s_state.interval_seconds;
  state_save();
  update_app_glance_safe();
  show_popup();
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
  s_state.next_vibe_epoch = now_seconds() + s_state.interval_seconds;
  state_save();
  cancel_ui_tick();
  schedule_ui_tick();
  update_app_glance_safe();
  update_ui();
}

static void pause_timer(void) {
  s_state.elapsed_accum = total_elapsed();
  s_state.running = false;
  s_state.run_started_epoch = 0;
  state_save();
  cancel_pending_wakeup();
  cancel_ui_tick();
  update_app_glance_safe();
  update_ui();
}

static void reset_timer(void) {
  s_state.running = false;
  s_state.elapsed_accum = 0;
  s_state.run_started_epoch = 0;
  s_state.next_vibe_epoch = 0;
  s_state.vibe_count = 0;
  state_save();
  cancel_pending_wakeup();
  cancel_ui_tick();
  update_app_glance_safe();
  update_ui();
}

static void apply_interval(int32_t total_seconds) {
  s_state.interval_seconds = clamp_interval(total_seconds);
  if (s_state.running) {
    s_state.next_vibe_epoch = now_seconds() + s_state.interval_seconds;
  }
  state_save();
  update_app_glance_safe();
  update_ui();
}

static void popup_anim_handler(void *context) {
  s_popup_anim_timer = NULL;
  s_popup_t += POPUP_ANIM_STEP;
  if (s_popup_t > 1.0f) {
    s_popup_t = 1.0f;
  }
  if (s_popup_canvas_layer) {
    layer_mark_dirty(s_popup_canvas_layer);
  }
  if (s_popup_t < 1.0f) {
    s_popup_anim_timer = app_timer_register(POPUP_ANIM_STEP_MS, popup_anim_handler, NULL);
  }
}

static void popup_dismiss_handler(void *context) {
  s_popup_dismiss_timer = NULL;
  if (s_popup_window) {
    window_stack_remove_from_stack(s_popup_window);
  }
}

static void popup_canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, color_bg());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  int cx = bounds.size.w / 2;
  int cy = bounds.size.h / 2 - 6;
  int r_max = (bounds.size.w < bounds.size.h ? bounds.size.w : bounds.size.h) / 2 - 12;

  graphics_context_set_antialiased(ctx, true);

  for (int k = 0; k < 3; k++) {
    float t = s_popup_t - k * 0.20f;
    if (t <= 0.0f) {
      continue;
    }
    if (t > 1.0f) {
      t = 1.0f;
    }
    int radius = (int)(r_max * 0.35f + (r_max - r_max * 0.35f) * t);
    int alpha = (int)(255.0f * (1.0f - t));
    if (alpha < 0) {
      alpha = 0;
    }
    if (alpha > 255) {
      alpha = 255;
    }
    int thickness = (int)(9.0f * (1.0f - t)) + 2;
#ifdef PBL_COLOR
    graphics_context_set_stroke_color(ctx, GColorFromRGBA(43, 212, 196, alpha));
#else
    graphics_context_set_stroke_color(ctx, alpha > 120 ? GColorWhite : GColorDarkGray);
#endif
    graphics_context_set_stroke_width(ctx, thickness);
    graphics_draw_circle(ctx, GPoint(cx, cy), radius);
  }

  graphics_context_set_fill_color(ctx, color_amber());
  graphics_fill_circle(ctx, GPoint(cx, cy), 11);
}

static void popup_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_popup_dismiss_timer) {
    app_timer_cancel(s_popup_dismiss_timer);
    s_popup_dismiss_timer = NULL;
  }
  if (s_popup_window) {
    window_stack_remove_from_stack(s_popup_window);
  }
}

static void popup_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, popup_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, popup_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, popup_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, popup_click_handler);
}

static void popup_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_popup_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_popup_canvas_layer, popup_canvas_update_proc);
  layer_add_child(window_layer, s_popup_canvas_layer);

  int cy = bounds.size.h / 2 - 6;

  snprintf(s_popup_title_text, sizeof(s_popup_title_text), "VIBE");
  s_popup_title_layer = text_layer_create(GRect(0, cy + 22, bounds.size.w, 44));
  text_layer_set_background_color(s_popup_title_layer, GColorClear);
  text_layer_set_text_color(s_popup_title_layer, color_amber());
  text_layer_set_text_alignment(s_popup_title_layer, GTextAlignmentCenter);
  text_layer_set_font(s_popup_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text(s_popup_title_layer, s_popup_title_text);
  layer_add_child(window_layer, text_layer_get_layer(s_popup_title_layer));

  char left_text[12];
  format_mmss(secs_to_vibe(), left_text, sizeof(left_text));
  snprintf(s_popup_sub_text, sizeof(s_popup_sub_text), "#%ld  next in %s",
           (long)(s_state.vibe_count), left_text);
  s_popup_sub_layer = text_layer_create(GRect(0, cy + 66, bounds.size.w, 24));
  text_layer_set_background_color(s_popup_sub_layer, GColorClear);
  text_layer_set_text_color(s_popup_sub_layer, color_sub());
  text_layer_set_text_alignment(s_popup_sub_layer, GTextAlignmentCenter);
  text_layer_set_font(s_popup_sub_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text(s_popup_sub_layer, s_popup_sub_text);
  layer_add_child(window_layer, text_layer_get_layer(s_popup_sub_layer));

  s_popup_t = 0.0f;
  s_popup_anim_timer = app_timer_register(POPUP_ANIM_STEP_MS, popup_anim_handler, NULL);
  s_popup_dismiss_timer = app_timer_register(POPUP_AUTO_DISMISS_MS, popup_dismiss_handler, NULL);
}

static void popup_window_unload(Window *window) {
  if (s_popup_anim_timer) {
    app_timer_cancel(s_popup_anim_timer);
    s_popup_anim_timer = NULL;
  }
  if (s_popup_dismiss_timer) {
    app_timer_cancel(s_popup_dismiss_timer);
    s_popup_dismiss_timer = NULL;
  }
  text_layer_destroy(s_popup_title_layer);
  text_layer_destroy(s_popup_sub_layer);
  layer_destroy(s_popup_canvas_layer);
  s_popup_title_layer = NULL;
  s_popup_sub_layer = NULL;
  s_popup_canvas_layer = NULL;
  s_popup_window = NULL;
}

static void show_popup(void) {
  if (s_popup_window) {
    return;
  }
  s_popup_window = window_create();
  window_set_background_color(s_popup_window, color_bg());
  window_set_fullscreen(s_popup_window, true);
  window_set_click_config_provider(s_popup_window, popup_click_config_provider);
  window_set_window_handlers(s_popup_window, (WindowHandlers) {
    .load = popup_window_load,
    .unload = popup_window_unload
  });
  window_stack_push(s_popup_window, true);
}

typedef struct {
  int16_t left_x, right_x, top, box_w, box_h, colon_x, colon_y;
} PickerGeom;

static void picker_geom(GRect bounds, PickerGeom *g) {
  g->box_w = 54;
  g->box_h = 62;
  int16_t gap = 14;
  int16_t row_w = g->box_w * 2 + gap;
  int16_t row_left = (bounds.size.w - row_w) / 2;
  g->left_x = row_left;
  g->right_x = row_left + g->box_w + gap;
  g->top = bounds.size.h / 2 - g->box_h / 2 - 6;
  g->colon_x = bounds.size.w / 2;
  g->colon_y = g->top + g->box_h / 2;
}

static void picker_refresh(void) {
  snprintf(s_pick_min_buf, sizeof(s_pick_min_buf), "%02ld", (long)s_pick_minutes);
  snprintf(s_pick_sec_buf, sizeof(s_pick_sec_buf), "%02ld", (long)s_pick_seconds);
  if (s_pick_min_layer) {
    text_layer_set_text(s_pick_min_layer, s_pick_min_buf);
    text_layer_set_text_color(s_pick_min_layer, s_pick_field == 0 ? GColorBlack : color_text());
  }
  if (s_pick_sec_layer) {
    text_layer_set_text(s_pick_sec_layer, s_pick_sec_buf);
    text_layer_set_text_color(s_pick_sec_layer, s_pick_field == 1 ? GColorBlack : color_text());
  }
  if (s_picker_canvas_layer) {
    layer_mark_dirty(s_picker_canvas_layer);
  }
}

static void picker_canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, color_bg());
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  PickerGeom g;
  picker_geom(bounds, &g);

  for (int field = 0; field < 2; field++) {
    int16_t x = field == 0 ? g.left_x : g.right_x;
    GRect box = GRect(x, g.top, g.box_w, g.box_h);
    if (field == s_pick_field) {
      graphics_context_set_fill_color(ctx, color_teal());
      graphics_fill_rect(ctx, box, 6, GCornersAll);
    } else {
      graphics_context_set_stroke_color(ctx, color_dim());
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_round_rect(ctx, box, 6);
    }
  }

  graphics_context_set_fill_color(ctx, color_sub());
  graphics_fill_circle(ctx, GPoint(g.colon_x, g.colon_y - 8), 3);
  graphics_fill_circle(ctx, GPoint(g.colon_x, g.colon_y + 8), 3);
}

static void picker_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_pick_minutes = s_state.interval_seconds / 60;
  s_pick_seconds = s_state.interval_seconds % 60;
  s_pick_field = 0;

  s_picker_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_picker_canvas_layer, picker_canvas_update_proc);
  layer_add_child(window_layer, s_picker_canvas_layer);

  PickerGeom g;
  picker_geom(bounds, &g);

  s_picker_title_layer = text_layer_create(GRect(0, 16, bounds.size.w, 26));
  text_layer_set_background_color(s_picker_title_layer, GColorClear);
  text_layer_set_text_color(s_picker_title_layer, color_sub());
  text_layer_set_text_alignment(s_picker_title_layer, GTextAlignmentCenter);
  text_layer_set_font(s_picker_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text(s_picker_title_layer, "VIBE INTERVAL");
  layer_add_child(window_layer, text_layer_get_layer(s_picker_title_layer));

  s_pick_min_layer = text_layer_create(GRect(g.left_x, g.top + 8, g.box_w, 46));
  text_layer_set_background_color(s_pick_min_layer, GColorClear);
  text_layer_set_text_alignment(s_pick_min_layer, GTextAlignmentCenter);
  text_layer_set_font(s_pick_min_layer, fonts_get_system_font(FONT_KEY_LECO_38_BOLD_NUMBERS));
  layer_add_child(window_layer, text_layer_get_layer(s_pick_min_layer));

  s_pick_sec_layer = text_layer_create(GRect(g.right_x, g.top + 8, g.box_w, 46));
  text_layer_set_background_color(s_pick_sec_layer, GColorClear);
  text_layer_set_text_alignment(s_pick_sec_layer, GTextAlignmentCenter);
  text_layer_set_font(s_pick_sec_layer, fonts_get_system_font(FONT_KEY_LECO_38_BOLD_NUMBERS));
  layer_add_child(window_layer, text_layer_get_layer(s_pick_sec_layer));

  s_pick_min_label_layer = text_layer_create(GRect(g.left_x, g.top + g.box_h + 4, g.box_w, 20));
  text_layer_set_background_color(s_pick_min_label_layer, GColorClear);
  text_layer_set_text_color(s_pick_min_label_layer, color_sub());
  text_layer_set_text_alignment(s_pick_min_label_layer, GTextAlignmentCenter);
  text_layer_set_font(s_pick_min_label_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  text_layer_set_text(s_pick_min_label_layer, "MIN");
  layer_add_child(window_layer, text_layer_get_layer(s_pick_min_label_layer));

  s_pick_sec_label_layer = text_layer_create(GRect(g.right_x, g.top + g.box_h + 4, g.box_w, 20));
  text_layer_set_background_color(s_pick_sec_label_layer, GColorClear);
  text_layer_set_text_color(s_pick_sec_label_layer, color_sub());
  text_layer_set_text_alignment(s_pick_sec_label_layer, GTextAlignmentCenter);
  text_layer_set_font(s_pick_sec_label_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  text_layer_set_text(s_pick_sec_label_layer, "SEC");
  layer_add_child(window_layer, text_layer_get_layer(s_pick_sec_label_layer));

  picker_refresh();
}

static void picker_up_click(ClickRecognizerRef recognizer, void *context) {
  if (s_pick_field == 0) {
    s_pick_minutes += 1;
    if (s_pick_minutes > 99) {
      s_pick_minutes = 0;
    }
  } else {
    s_pick_seconds += 15;
    if (s_pick_seconds >= 60) {
      s_pick_seconds = 0;
    }
  }
  picker_refresh();
}

static void picker_down_click(ClickRecognizerRef recognizer, void *context) {
  if (s_pick_field == 0) {
    s_pick_minutes -= 1;
    if (s_pick_minutes < 0) {
      s_pick_minutes = 99;
    }
  } else {
    s_pick_seconds -= 15;
    if (s_pick_seconds < 0) {
      s_pick_seconds = 45;
    }
  }
  picker_refresh();
}

static void picker_select_click(ClickRecognizerRef recognizer, void *context) {
  if (s_pick_field == 0) {
    s_pick_field = 1;
    picker_refresh();
  } else {
    int32_t total = s_pick_minutes * 60 + s_pick_seconds;
    apply_interval(total);
    window_stack_pop(true);
  }
}

static void picker_back_click(ClickRecognizerRef recognizer, void *context) {
  int32_t total = s_pick_minutes * 60 + s_pick_seconds;
  apply_interval(total);
  window_stack_pop(true);
}

static void picker_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, picker_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, picker_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, picker_select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, picker_back_click);
}

static void picker_window_unload(Window *window) {
  text_layer_destroy(s_picker_title_layer);
  text_layer_destroy(s_pick_min_layer);
  text_layer_destroy(s_pick_sec_layer);
  text_layer_destroy(s_pick_min_label_layer);
  text_layer_destroy(s_pick_sec_label_layer);
  layer_destroy(s_picker_canvas_layer);
  s_picker_title_layer = NULL;
  s_pick_min_layer = NULL;
  s_pick_sec_layer = NULL;
  s_pick_min_label_layer = NULL;
  s_pick_sec_label_layer = NULL;
  s_picker_canvas_layer = NULL;
}

static void show_picker(void) {
  if (!s_picker_window) {
    s_picker_window = window_create();
    window_set_background_color(s_picker_window, color_bg());
    window_set_click_config_provider(s_picker_window, picker_click_config_provider);
    window_set_window_handlers(s_picker_window, (WindowHandlers) {
      .load = picker_window_load,
      .unload = picker_window_unload
    });
  }
  window_stack_push(s_picker_window, true);
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_state.running) {
    pause_timer();
  } else {
    start_timer();
  }
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  reset_timer();
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  show_picker();
}

static void main_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  compute_ring_geometry(bounds);

  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, canvas_update_proc);
  layer_add_child(window_layer, s_canvas_layer);

  s_clock_layer = text_layer_create(GRect(0, 3, bounds.size.w, 20));
  text_layer_set_background_color(s_clock_layer, GColorClear);
  text_layer_set_text_color(s_clock_layer, color_text());
  text_layer_set_text_alignment(s_clock_layer, GTextAlignmentCenter);
  text_layer_set_font(s_clock_layer, fonts_get_system_font(FONT_KEY_LECO_20_BOLD_NUMBERS));
  layer_add_child(window_layer, text_layer_get_layer(s_clock_layer));

  s_status_layer = text_layer_create(GRect(0, 24, bounds.size.w, 20));
  text_layer_set_background_color(s_status_layer, GColorClear);
  text_layer_set_text_color(s_status_layer, color_amber());
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

  s_hero_layer = text_layer_create(GRect(0, s_ring_cy - 18, bounds.size.w, 36));
  text_layer_set_background_color(s_hero_layer, GColorClear);
  text_layer_set_text_color(s_hero_layer, color_text());
  text_layer_set_text_alignment(s_hero_layer, GTextAlignmentCenter);
  text_layer_set_font(s_hero_layer, fonts_get_system_font(FONT_KEY_LECO_32_BOLD_NUMBERS));
  layer_add_child(window_layer, text_layer_get_layer(s_hero_layer));

  s_next_layer = text_layer_create(GRect(0, s_ring_cy + s_ring_r, bounds.size.w, 18));
  text_layer_set_background_color(s_next_layer, GColorClear);
  text_layer_set_text_color(s_next_layer, color_teal());
  text_layer_set_text_alignment(s_next_layer, GTextAlignmentCenter);
  text_layer_set_font(s_next_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_next_layer));

  s_hint_layer = text_layer_create(GRect(6, bounds.size.h - 24, bounds.size.w - 12, 22));
  text_layer_set_background_color(s_hint_layer, GColorClear);
  text_layer_set_text_color(s_hint_layer, color_sub());
  text_layer_set_text_alignment(s_hint_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_hint_layer, GTextOverflowModeWordWrap);
  text_layer_set_font(s_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  layer_add_child(window_layer, text_layer_get_layer(s_hint_layer));

  window_set_click_config_provider(window, main_click_config_provider);
  update_ui();
  schedule_ui_tick();
}

static void main_window_unload(Window *window) {
  cancel_ui_tick();
  text_layer_destroy(s_hint_layer);
  text_layer_destroy(s_next_layer);
  text_layer_destroy(s_hero_layer);
  text_layer_destroy(s_status_layer);
  text_layer_destroy(s_clock_layer);
  layer_destroy(s_canvas_layer);
  s_hint_layer = NULL;
  s_next_layer = NULL;
  s_hero_layer = NULL;
  s_status_layer = NULL;
  s_clock_layer = NULL;
  s_canvas_layer = NULL;
}

static void wakeup_handler(WakeupId id, int32_t cookie) {
  fire_vibe();
}

static void init(void) {
  state_load_or_default();

  s_launched_by_wakeup = (launch_reason() == APP_LAUNCH_WAKEUP);
  if (s_launched_by_wakeup) {
    WakeupId id = 0;
    int32_t cookie = 0;
    wakeup_get_launch_event(&id, &cookie);
  } else {
    cancel_pending_wakeup();
  }

  wakeup_service_subscribe(wakeup_handler);

  s_main_window = window_create();
  window_set_background_color(s_main_window, color_bg());
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });
  window_stack_push(s_main_window, true);

  if (s_launched_by_wakeup && s_state.running && now_seconds() >= s_state.next_vibe_epoch) {
    fire_vibe();
  }

  schedule_ui_tick();
  update_app_glance_safe();
}

static void deinit(void) {
  if (s_state.running) {
    schedule_wakeup_for_next();
  }
  if (s_popup_window) {
    window_stack_remove_from_stack(s_popup_window);
  }
  if (s_picker_window) {
    window_destroy(s_picker_window);
  }
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
