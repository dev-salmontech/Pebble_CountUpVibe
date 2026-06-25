#include <pebble.h>

#define DEFAULT_INTERVAL_SECONDS (5 * 60)
#define MIN_INTERVAL_SECONDS 10
#define MAX_INTERVAL_SECONDS (99 * 60 + 59)

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
static Layer *s_fill_layer;
static Layer *s_picker_canvas_layer;
static TextLayer *s_clock_layer;
static TextLayer *s_status_layer;
static TextLayer *s_hero_layer;
static TextLayer *s_next_layer;
static TextLayer *s_btn_up_layer;
static TextLayer *s_btn_sel_layer;
static TextLayer *s_btn_down_layer;
static TextLayer *s_picker_title_layer;
static TextLayer *s_pk_btn_up_layer;
static TextLayer *s_pk_btn_sel_layer;
static TextLayer *s_pk_btn_down_layer;
static TextLayer *s_pick_min_layer;
static TextLayer *s_pick_sec_layer;
static TextLayer *s_pick_min_label_layer;
static TextLayer *s_pick_sec_label_layer;
static AppTimer *s_ui_timer;
static TimerState s_state;
static bool s_launched_by_wakeup;
static int s_center_y;

static int32_t s_pick_minutes;
static int32_t s_pick_seconds;
static int s_pick_field;

static char s_clock_text[16];
static char s_status_text[16];
static char s_hero_text[16];
static char s_next_text[24];
static char s_btn_up_text[16];
static char s_btn_sel_text[16];
static char s_btn_down_text[16];
static char s_glance_text[96];
static char s_pick_min_buf[8];
static char s_pick_sec_buf[8];

static const uint32_t s_vibe_durations[] = { 150, 90, 320 };
static const VibePattern s_vibe_pattern = {
  .durations = s_vibe_durations,
  .num_segments = 3
};

static void update_ui(void);
static void schedule_ui_tick(void);
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
static GColor color_accent(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x1262B5);
#else
  return GColorBlack;
#endif
}
static GColor color_next(void) {
#ifdef PBL_COLOR
  return GColorFromHEX(0x0B3D91);
#else
  return GColorBlack;
#endif
}
static GColor color_sub(void) {
  return GColorDarkGray;
}
static GColor color_outline(void) {
  return GColorLightGray;
}

static void compute_layout(GRect bounds) {
  s_center_y = bounds.size.h / 2 + 4;
}

static void fill_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  int32_t interval = s_state.interval_seconds > 0 ? s_state.interval_seconds : 1;
  int32_t left = secs_to_vibe();
  if (left > interval) {
    left = interval;
  }
  int32_t elapsed_in_cycle = interval - left;
  if (elapsed_in_cycle < 0) {
    elapsed_in_cycle = 0;
  }
  if (elapsed_in_cycle > interval) {
    elapsed_in_cycle = interval;
  }
  int16_t fill_h = (int16_t)((elapsed_in_cycle * bounds.size.h) / interval);

  graphics_context_set_fill_color(ctx, color_water());
  GRect fill = GRect(0, bounds.size.h - fill_h, bounds.size.w, fill_h);
  graphics_fill_rect(ctx, fill, 0, GCornerNone);
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
    snprintf(s_btn_up_text, sizeof(s_btn_up_text), "SET");
    snprintf(s_btn_sel_text, sizeof(s_btn_sel_text), "PAUSE");
    snprintf(s_btn_down_text, sizeof(s_btn_down_text), "RESET");
  } else if (total_elapsed() > 0) {
    snprintf(s_status_text, sizeof(s_status_text), "PAUSED");
    snprintf(s_next_text, sizeof(s_next_text), "paused");
    snprintf(s_btn_up_text, sizeof(s_btn_up_text), "SET");
    snprintf(s_btn_sel_text, sizeof(s_btn_sel_text), "RESUME");
    snprintf(s_btn_down_text, sizeof(s_btn_down_text), "RESET");
  } else {
    snprintf(s_status_text, sizeof(s_status_text), "READY");
    char interval_text[12];
    format_mmss(s_state.interval_seconds, interval_text, sizeof(interval_text));
    snprintf(s_next_text, sizeof(s_next_text), "every %s", interval_text);
    snprintf(s_btn_up_text, sizeof(s_btn_up_text), "SET");
    snprintf(s_btn_sel_text, sizeof(s_btn_sel_text), "START");
    snprintf(s_btn_down_text, sizeof(s_btn_down_text), "RESET");
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
  if (s_btn_up_layer) {
    text_layer_set_text(s_btn_up_layer, s_btn_up_text);
  }
  if (s_btn_sel_layer) {
    text_layer_set_text(s_btn_sel_layer, s_btn_sel_text);
  }
  if (s_btn_down_layer) {
    text_layer_set_text(s_btn_down_layer, s_btn_down_text);
  }
  if (s_fill_layer) {
    layer_mark_dirty(s_fill_layer);
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

  vibes_enqueue_custom_pattern(s_vibe_pattern);
  s_state.vibe_count += 1;
  s_state.next_vibe_epoch = now + s_state.interval_seconds;
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

typedef struct {
  int16_t left_x, right_x, top, box_w, box_h, colon_x, colon_y;
} PickerGeom;

static void picker_geom(GRect bounds, PickerGeom *g) {
  g->box_w = 50;
  g->box_h = 62;
  int16_t gap = 12;
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
    text_layer_set_text_color(s_pick_min_layer, color_ink());
  }
  if (s_pick_sec_layer) {
    text_layer_set_text(s_pick_sec_layer, s_pick_sec_buf);
    text_layer_set_text_color(s_pick_sec_layer, color_ink());
  }
  if (s_picker_canvas_layer) {
    layer_mark_dirty(s_picker_canvas_layer);
  }
}

static void picker_canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  PickerGeom g;
  picker_geom(bounds, &g);

  for (int field = 0; field < 2; field++) {
    int16_t x = field == 0 ? g.left_x : g.right_x;
    GRect box = GRect(x, g.top, g.box_w, g.box_h);
    if (field == s_pick_field) {
      graphics_context_set_fill_color(ctx, color_water());
      graphics_fill_rect(ctx, box, 6, GCornersAll);
    } else {
      graphics_context_set_stroke_color(ctx, color_outline());
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_round_rect(ctx, box, 6);
    }
  }

  graphics_context_set_fill_color(ctx, color_sub());
  graphics_fill_circle(ctx, GPoint(g.colon_x, g.colon_y - 8), 3);
  graphics_fill_circle(ctx, GPoint(g.colon_x, g.colon_y + 8), 3);
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

  int16_t pbtn_x = bounds.size.w - 40;
  s_pk_btn_up_layer = text_layer_create(GRect(pbtn_x, 6, 36, 18));
  text_layer_set_background_color(s_pk_btn_up_layer, GColorClear);
  text_layer_set_text_color(s_pk_btn_up_layer, color_ink());
  text_layer_set_text_alignment(s_pk_btn_up_layer, GTextAlignmentRight);
  text_layer_set_font(s_pk_btn_up_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text(s_pk_btn_up_layer, "+");
  layer_add_child(window_layer, text_layer_get_layer(s_pk_btn_up_layer));

  s_pk_btn_sel_layer = text_layer_create(GRect(pbtn_x, bounds.size.h / 2 - 9, 36, 18));
  text_layer_set_background_color(s_pk_btn_sel_layer, GColorClear);
  text_layer_set_text_color(s_pk_btn_sel_layer, color_ink());
  text_layer_set_text_alignment(s_pk_btn_sel_layer, GTextAlignmentRight);
  text_layer_set_font(s_pk_btn_sel_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text(s_pk_btn_sel_layer, "OK");
  layer_add_child(window_layer, text_layer_get_layer(s_pk_btn_sel_layer));

  s_pk_btn_down_layer = text_layer_create(GRect(pbtn_x, bounds.size.h - 26, 36, 18));
  text_layer_set_background_color(s_pk_btn_down_layer, GColorClear);
  text_layer_set_text_color(s_pk_btn_down_layer, color_ink());
  text_layer_set_text_alignment(s_pk_btn_down_layer, GTextAlignmentRight);
  text_layer_set_font(s_pk_btn_down_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text(s_pk_btn_down_layer, "-");
  layer_add_child(window_layer, text_layer_get_layer(s_pk_btn_down_layer));

  picker_refresh();
}

static void picker_window_unload(Window *window) {
  text_layer_destroy(s_picker_title_layer);
  text_layer_destroy(s_pick_min_layer);
  text_layer_destroy(s_pick_sec_layer);
  text_layer_destroy(s_pick_min_label_layer);
  text_layer_destroy(s_pick_sec_label_layer);
  text_layer_destroy(s_pk_btn_up_layer);
  text_layer_destroy(s_pk_btn_sel_layer);
  text_layer_destroy(s_pk_btn_down_layer);
  layer_destroy(s_picker_canvas_layer);
  s_picker_title_layer = NULL;
  s_pick_min_layer = NULL;
  s_pick_sec_layer = NULL;
  s_pick_min_label_layer = NULL;
  s_pick_sec_label_layer = NULL;
  s_pk_btn_up_layer = NULL;
  s_pk_btn_sel_layer = NULL;
  s_pk_btn_down_layer = NULL;
  s_picker_canvas_layer = NULL;
}

static void show_picker(void) {
  if (!s_picker_window) {
    s_picker_window = window_create();
    window_set_background_color(s_picker_window, GColorWhite);
    window_set_click_config_provider(s_picker_window, picker_click_config_provider);
    window_set_window_handlers(s_picker_window, (WindowHandlers) {
      .load = picker_window_load,
      .unload = picker_window_unload
    });
  }
  if (!window_stack_contains_window(s_picker_window)) {
    window_stack_push(s_picker_window, true);
  }
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
  compute_layout(bounds);

  s_fill_layer = layer_create(bounds);
  layer_set_update_proc(s_fill_layer, fill_update_proc);
  layer_add_child(window_layer, s_fill_layer);

  s_clock_layer = text_layer_create(GRect(0, 3, bounds.size.w, 20));
  text_layer_set_background_color(s_clock_layer, GColorClear);
  text_layer_set_text_color(s_clock_layer, color_ink());
  text_layer_set_text_alignment(s_clock_layer, GTextAlignmentCenter);
  text_layer_set_font(s_clock_layer, fonts_get_system_font(FONT_KEY_LECO_20_BOLD_NUMBERS));
  layer_add_child(window_layer, text_layer_get_layer(s_clock_layer));

  s_status_layer = text_layer_create(GRect(0, 24, bounds.size.w, 20));
  text_layer_set_background_color(s_status_layer, GColorClear);
  text_layer_set_text_color(s_status_layer, color_accent());
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

  s_hero_layer = text_layer_create(GRect(0, s_center_y - 24, bounds.size.w, 48));
  text_layer_set_background_color(s_hero_layer, GColorClear);
  text_layer_set_text_color(s_hero_layer, color_ink());
  text_layer_set_text_alignment(s_hero_layer, GTextAlignmentCenter);
  text_layer_set_font(s_hero_layer, fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS));
  layer_add_child(window_layer, text_layer_get_layer(s_hero_layer));

  s_next_layer = text_layer_create(GRect(0, s_center_y + 28, bounds.size.w, 20));
  text_layer_set_background_color(s_next_layer, GColorClear);
  text_layer_set_text_color(s_next_layer, color_next());
  text_layer_set_text_alignment(s_next_layer, GTextAlignmentCenter);
  text_layer_set_font(s_next_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_next_layer));

  int16_t btn_x = bounds.size.w - 44;
  s_btn_up_layer = text_layer_create(GRect(btn_x, 6, 40, 18));
  text_layer_set_background_color(s_btn_up_layer, GColorClear);
  text_layer_set_text_color(s_btn_up_layer, color_ink());
  text_layer_set_text_alignment(s_btn_up_layer, GTextAlignmentRight);
  text_layer_set_font(s_btn_up_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_btn_up_layer));

  s_btn_sel_layer = text_layer_create(GRect(btn_x, s_center_y - 9, 40, 18));
  text_layer_set_background_color(s_btn_sel_layer, GColorClear);
  text_layer_set_text_color(s_btn_sel_layer, color_ink());
  text_layer_set_text_alignment(s_btn_sel_layer, GTextAlignmentRight);
  text_layer_set_font(s_btn_sel_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_btn_sel_layer));

  s_btn_down_layer = text_layer_create(GRect(btn_x, bounds.size.h - 26, 40, 18));
  text_layer_set_background_color(s_btn_down_layer, GColorClear);
  text_layer_set_text_color(s_btn_down_layer, color_ink());
  text_layer_set_text_alignment(s_btn_down_layer, GTextAlignmentRight);
  text_layer_set_font(s_btn_down_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_btn_down_layer));

  window_set_click_config_provider(window, main_click_config_provider);
  update_ui();
  schedule_ui_tick();
}

static void main_window_unload(Window *window) {
  cancel_ui_tick();
  text_layer_destroy(s_btn_down_layer);
  text_layer_destroy(s_btn_sel_layer);
  text_layer_destroy(s_btn_up_layer);
  text_layer_destroy(s_next_layer);
  text_layer_destroy(s_hero_layer);
  text_layer_destroy(s_status_layer);
  text_layer_destroy(s_clock_layer);
  layer_destroy(s_fill_layer);
  s_btn_down_layer = NULL;
  s_btn_sel_layer = NULL;
  s_btn_up_layer = NULL;
  s_next_layer = NULL;
  s_hero_layer = NULL;
  s_status_layer = NULL;
  s_clock_layer = NULL;
  s_fill_layer = NULL;
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
  window_set_background_color(s_main_window, GColorWhite);
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
  while (window_stack_get_top_window() && window_stack_get_top_window() != s_main_window) {
    window_stack_pop(false);
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
