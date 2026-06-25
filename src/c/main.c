#include <pebble.h>

#define DEFAULT_INTERVAL_SECONDS (5 * 60)
#define MIN_INTERVAL_SECONDS (1 * 60)
#define MAX_INTERVAL_SECONDS (120 * 60)
#define INTERVAL_STEP_SECONDS (1 * 60)

enum {
  PERSIST_INITIALIZED = 1,
  PERSIST_RUNNING = 2,
  PERSIST_ELAPSED_ACCUM = 3,
  PERSIST_RUN_STARTED = 4,
  PERSIST_INTERVAL = 5,
  PERSIST_LAST_VIBE_ELAPSED = 6
};

enum {
  WORKER_MSG_SYNC = 1,
  WORKER_MSG_VIBED = 2
};

typedef struct {
  bool initialized;
  bool running;
  int32_t elapsed_accum;
  int32_t run_started_epoch;
  int32_t interval_seconds;
  int32_t last_vibe_elapsed;
} TimerState;

static Window *s_main_window;
static Window *s_interval_window;
static Layer *s_canvas_layer;
static TextLayer *s_title_layer;
static TextLayer *s_time_layer;
static TextLayer *s_status_layer;
static TextLayer *s_interval_layer;
static TextLayer *s_hint_layer;
static TextLayer *s_interval_menu_layer;
static AppTimer *s_ui_timer;
static TimerState s_state;
static char s_time_text[24];
static char s_status_text[32];
static char s_interval_text[48];
static char s_hint_text[64];
static char s_interval_menu_text[96];
static char s_glance_text[96];

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

static int32_t current_elapsed(void) {
  if (!s_state.running) {
    return s_state.elapsed_accum;
  }

  int32_t delta = now_seconds() - s_state.run_started_epoch;
  if (delta < 0) {
    delta = 0;
  }
  return s_state.elapsed_accum + delta;
}

static void state_save(void) {
  persist_write_int(PERSIST_INITIALIZED, 1);
  persist_write_int(PERSIST_RUNNING, s_state.running ? 1 : 0);
  persist_write_int(PERSIST_ELAPSED_ACCUM, s_state.elapsed_accum);
  persist_write_int(PERSIST_RUN_STARTED, s_state.run_started_epoch);
  persist_write_int(PERSIST_INTERVAL, s_state.interval_seconds);
  persist_write_int(PERSIST_LAST_VIBE_ELAPSED, s_state.last_vibe_elapsed);
}

static void state_load_or_default(void) {
  if (!persist_exists(PERSIST_INITIALIZED)) {
    s_state.initialized = true;
    s_state.running = true;
    s_state.elapsed_accum = 0;
    s_state.run_started_epoch = now_seconds();
    s_state.interval_seconds = DEFAULT_INTERVAL_SECONDS;
    s_state.last_vibe_elapsed = 0;
    state_save();
    return;
  }

  s_state.initialized = true;
  s_state.running = persist_read_int(PERSIST_RUNNING) != 0;
  s_state.elapsed_accum = persist_read_int(PERSIST_ELAPSED_ACCUM);
  s_state.run_started_epoch = persist_read_int(PERSIST_RUN_STARTED);
  s_state.interval_seconds = persist_read_int(PERSIST_INTERVAL);
  if (s_state.interval_seconds == 0) {
    s_state.interval_seconds = DEFAULT_INTERVAL_SECONDS;
  }
  s_state.interval_seconds = clamp_interval(s_state.interval_seconds);
  s_state.last_vibe_elapsed = persist_read_int(PERSIST_LAST_VIBE_ELAPSED);
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

static void update_app_glance(void) {
#if !PBL_PLATFORM_APLITE && defined(APP_GLANCE_SLICE_NO_EXPIRATION)
  char elapsed_text[24];
  format_elapsed(current_elapsed(), elapsed_text, sizeof(elapsed_text));
  snprintf(s_glance_text, sizeof(s_glance_text), "%s %s, vibe %ld min",
           s_state.running ? "Running" : "Stopped",
           elapsed_text,
           (long)(s_state.interval_seconds / 60));
  app_glance_reload(glance_reload_handler, NULL);
#endif
}

static void sync_worker(void) {
  if (!s_state.running) {
    return;
  }

  if (!app_worker_is_running()) {
    app_worker_launch();
  }

  AppWorkerMessage message = {
    .data0 = (uint16_t)s_state.interval_seconds,
    .data1 = 0,
    .data2 = 0
  };
  app_worker_send_message(WORKER_MSG_SYNC, &message);
}

static void kill_worker(void) {
  if (app_worker_is_running()) {
    app_worker_kill();
  }
}

static void update_ui(void);

static void schedule_ui_tick(void);

static void ui_tick_handler(void *context) {
  s_ui_timer = NULL;
  update_ui();
  if (s_state.running) {
    schedule_ui_tick();
  }
}

static void schedule_ui_tick(void) {
  if (!s_ui_timer && s_state.running) {
    s_ui_timer = app_timer_register(1000, ui_tick_handler, NULL);
  }
}

static void cancel_ui_tick(void) {
  if (s_ui_timer) {
    app_timer_cancel(s_ui_timer);
    s_ui_timer = NULL;
  }
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromHEX(0x081820));
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  int16_t margin = 8;
  GRect frame = GRect(margin, margin, bounds.size.w - (margin * 2), bounds.size.h - (margin * 2));
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorFromHEX(0x00ffaa));
#else
  graphics_context_set_stroke_color(ctx, GColorWhite);
#endif
  graphics_draw_rect(ctx, frame);

  int32_t basis = s_state.last_vibe_elapsed;
  int32_t elapsed = current_elapsed();
  int32_t since_vibe = elapsed - basis;
  if (basis == 0) {
    since_vibe = elapsed;
  }
  if (since_vibe < 0) {
    since_vibe = 0;
  }
  if (since_vibe > s_state.interval_seconds) {
    since_vibe = s_state.interval_seconds;
  }

  int16_t bar_width = frame.size.w;
  if (s_state.interval_seconds > 0) {
    bar_width = (int16_t)((frame.size.w * since_vibe) / s_state.interval_seconds);
  }
  GRect bar = GRect(frame.origin.x, frame.origin.y + frame.size.h - 6, bar_width, 4);
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromHEX(0xffcc33));
#else
  graphics_context_set_fill_color(ctx, GColorWhite);
#endif
  graphics_fill_rect(ctx, bar, 0, GCornerNone);
}

static void set_text_colors(TextLayer *layer, bool accent) {
  text_layer_set_background_color(layer, GColorClear);
#ifdef PBL_COLOR
  text_layer_set_text_color(layer, accent ? GColorFromHEX(0xffcc33) : GColorFromHEX(0xd8ffe8));
#else
  text_layer_set_text_color(layer, GColorWhite);
#endif
}

static void update_ui(void) {
  int32_t elapsed = current_elapsed();
  format_elapsed(elapsed, s_time_text, sizeof(s_time_text));
  snprintf(s_status_text, sizeof(s_status_text), "%s", s_state.running ? "RUNNING" : (elapsed > 0 ? "PAUSED" : "RESET"));
  snprintf(s_interval_text, sizeof(s_interval_text), "Interval: %ld min", (long)(s_state.interval_seconds / 60));

  if (s_state.running) {
    snprintf(s_hint_text, sizeof(s_hint_text), "SEL pause | UP interval\nDOWN reset");
  } else if (elapsed > 0) {
    snprintf(s_hint_text, sizeof(s_hint_text), "SEL resume | UP interval\nDOWN reset");
  } else {
    snprintf(s_hint_text, sizeof(s_hint_text), "DOWN start | UP interval\nSEL pause/resume");
  }

  if (s_time_layer) {
    text_layer_set_text(s_time_layer, s_time_text);
  }
  if (s_status_layer) {
    text_layer_set_text(s_status_layer, s_status_text);
  }
  if (s_interval_layer) {
    text_layer_set_text(s_interval_layer, s_interval_text);
  }
  if (s_hint_layer) {
    text_layer_set_text(s_hint_layer, s_hint_text);
  }
  if (s_canvas_layer) {
    layer_mark_dirty(s_canvas_layer);
  }
}

static void start_timer(void) {
  s_state.running = true;
  s_state.run_started_epoch = now_seconds();
  state_save();
  sync_worker();
  schedule_ui_tick();
  update_app_glance();
  update_ui();
}

static void pause_timer(void) {
  s_state.elapsed_accum = current_elapsed();
  s_state.running = false;
  s_state.run_started_epoch = 0;
  state_save();
  kill_worker();
  cancel_ui_tick();
  update_app_glance();
  update_ui();
}

static void reset_timer(void) {
  s_state.running = false;
  s_state.elapsed_accum = 0;
  s_state.run_started_epoch = 0;
  s_state.last_vibe_elapsed = 0;
  state_save();
  kill_worker();
  cancel_ui_tick();
  update_app_glance();
  update_ui();
}

static void adjust_interval(int32_t delta_seconds) {
  int32_t next_interval = clamp_interval(s_state.interval_seconds + delta_seconds);
  if (next_interval == s_state.interval_seconds) {
    return;
  }

  s_state.interval_seconds = next_interval;
  state_save();
  sync_worker();
  update_app_glance();
  update_ui();
}

static void update_interval_menu(void) {
  snprintf(s_interval_menu_text, sizeof(s_interval_menu_text),
           "Vibe interval\n\n%ld minutes\n\nUP/DOWN adjust\nSELECT accept",
           (long)(s_state.interval_seconds / 60));
  if (s_interval_menu_layer) {
    text_layer_set_text(s_interval_menu_layer, s_interval_menu_text);
  }
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_state.running) {
    pause_timer();
  } else if (s_state.elapsed_accum > 0) {
    start_timer();
  }
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_state.running || s_state.elapsed_accum > 0 || s_state.last_vibe_elapsed > 0) {
    reset_timer();
  } else {
    start_timer();
  }
}

static void interval_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  adjust_interval(INTERVAL_STEP_SECONDS);
  update_interval_menu();
}

static void interval_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  adjust_interval(-INTERVAL_STEP_SECONDS);
  update_interval_menu();
}

static void interval_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  window_stack_pop(true);
}

static void interval_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, interval_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, interval_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, interval_select_click_handler);
}

static void interval_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  int16_t margin = 10;
  s_interval_menu_layer = text_layer_create(GRect(margin, margin, bounds.size.w - (margin * 2), bounds.size.h - (margin * 2)));
  set_text_colors(s_interval_menu_layer, true);
  text_layer_set_text_alignment(s_interval_menu_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_interval_menu_layer, GTextOverflowModeWordWrap);
  text_layer_set_font(s_interval_menu_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  update_interval_menu();
  layer_add_child(window_layer, text_layer_get_layer(s_interval_menu_layer));
}

static void interval_window_unload(Window *window) {
  text_layer_destroy(s_interval_menu_layer);
  s_interval_menu_layer = NULL;
}

static void show_interval_window(void) {
  if (!s_interval_window) {
    s_interval_window = window_create();
    window_set_background_color(s_interval_window, GColorBlack);
    window_set_click_config_provider(s_interval_window, interval_click_config_provider);
    window_set_window_handlers(s_interval_window, (WindowHandlers) {
      .load = interval_window_load,
      .unload = interval_window_unload
    });
  }
  window_stack_push(s_interval_window, true);
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  show_interval_window();
}

static void main_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
}

static void worker_message_handler(uint16_t type, AppWorkerMessage *data) {
  if (type == WORKER_MSG_VIBED) {
    s_state.last_vibe_elapsed = current_elapsed();
    s_state.interval_seconds = clamp_interval(persist_read_int(PERSIST_INTERVAL));
    update_app_glance();
    update_ui();
  }
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  int16_t margin = 10;
  int16_t content_width = bounds.size.w - (margin * 2);

  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, canvas_update_proc);
  layer_add_child(window_layer, s_canvas_layer);

  s_title_layer = text_layer_create(GRect(margin, margin, content_width, 24));
  set_text_colors(s_title_layer, true);
  text_layer_set_text_alignment(s_title_layer, GTextAlignmentCenter);
  text_layer_set_font(s_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text(s_title_layer, "COUNTUPVIBE");
  layer_add_child(window_layer, text_layer_get_layer(s_title_layer));

  s_time_layer = text_layer_create(GRect(margin, (bounds.size.h / 2) - 34, content_width, 44));
  set_text_colors(s_time_layer, false);
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_time_layer));

  s_status_layer = text_layer_create(GRect(margin, (bounds.size.h / 2) + 8, content_width, 28));
  set_text_colors(s_status_layer, true);
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

  s_interval_layer = text_layer_create(GRect(margin, (bounds.size.h / 2) + 38, content_width, 24));
  set_text_colors(s_interval_layer, false);
  text_layer_set_text_alignment(s_interval_layer, GTextAlignmentCenter);
  text_layer_set_font(s_interval_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  layer_add_child(window_layer, text_layer_get_layer(s_interval_layer));

  s_hint_layer = text_layer_create(GRect(margin, bounds.size.h - 48, content_width, 40));
  set_text_colors(s_hint_layer, false);
  text_layer_set_text_alignment(s_hint_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_hint_layer, GTextOverflowModeWordWrap);
  text_layer_set_font(s_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  layer_add_child(window_layer, text_layer_get_layer(s_hint_layer));

  update_ui();
  schedule_ui_tick();
}

static void main_window_unload(Window *window) {
  cancel_ui_tick();
  text_layer_destroy(s_hint_layer);
  text_layer_destroy(s_interval_layer);
  text_layer_destroy(s_status_layer);
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_title_layer);
  layer_destroy(s_canvas_layer);
  s_hint_layer = NULL;
  s_interval_layer = NULL;
  s_status_layer = NULL;
  s_time_layer = NULL;
  s_title_layer = NULL;
  s_canvas_layer = NULL;
}

static void init(void) {
  state_load_or_default();
  app_worker_message_subscribe(worker_message_handler);

  s_main_window = window_create();
  window_set_background_color(s_main_window, GColorBlack);
  window_set_click_config_provider(s_main_window, main_click_config_provider);
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });
  window_stack_push(s_main_window, true);

  if (s_state.running) {
    sync_worker();
  }
  update_app_glance();
}

static void deinit(void) {
  if (s_interval_window) {
    window_destroy(s_interval_window);
  }
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
