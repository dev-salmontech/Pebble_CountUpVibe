#include <pebble_worker.h>

#define DEFAULT_INTERVAL_SECONDS (5 * 60)
#define MIN_INTERVAL_SECONDS (1 * 60)
#define MAX_INTERVAL_SECONDS (120 * 60)
#define MAX_ONE_SHOT_SECONDS (60 * 60)

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

static AppTimer *s_timer;
static bool s_running;
static int32_t s_elapsed_accum;
static int32_t s_run_started_epoch;
static int32_t s_interval_seconds;
static int32_t s_last_vibe_elapsed;
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

static bool load_state(void) {
  if (!persist_exists(PERSIST_INITIALIZED)) {
    return false;
  }

  s_running = persist_read_int(PERSIST_RUNNING) != 0;
  s_elapsed_accum = persist_read_int(PERSIST_ELAPSED_ACCUM);
  s_run_started_epoch = persist_read_int(PERSIST_RUN_STARTED);
  s_interval_seconds = persist_read_int(PERSIST_INTERVAL);
  if (s_interval_seconds == 0) {
    s_interval_seconds = DEFAULT_INTERVAL_SECONDS;
  }
  s_interval_seconds = clamp_interval(s_interval_seconds);
  s_last_vibe_elapsed = persist_read_int(PERSIST_LAST_VIBE_ELAPSED);
  return s_running;
}

static int32_t current_elapsed(void) {
  int32_t delta = now_seconds() - s_run_started_epoch;
  if (delta < 0) {
    delta = 0;
  }
  return s_elapsed_accum + delta;
}

static void format_elapsed(int32_t elapsed_seconds, char *buffer, size_t buffer_size) {
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

static void update_app_glance(int32_t elapsed) {
#if !PBL_PLATFORM_APLITE && defined(APP_GLANCE_SLICE_NO_EXPIRATION)
  char elapsed_text[24];
  format_elapsed(elapsed, elapsed_text, sizeof(elapsed_text));
  snprintf(s_glance_text, sizeof(s_glance_text), "Vibed at %s, next %ld min",
           elapsed_text,
           (long)(s_interval_seconds / 60));
  app_glance_reload(glance_reload_handler, NULL);
#endif
}

static void schedule_next(void);

static void timer_handler(void *context) {
  s_timer = NULL;
  schedule_next();
}

static void send_vibed_message(int32_t elapsed) {
  AppWorkerMessage message = {
    .data0 = (uint16_t)(elapsed & 0xffff),
    .data1 = (uint16_t)s_interval_seconds,
    .data2 = 0
  };
  app_worker_send_message(WORKER_MSG_VIBED, &message);
}

static void schedule_seconds(int32_t delay_seconds) {
  if (delay_seconds < 1) {
    delay_seconds = 1;
  }
  if (delay_seconds > MAX_ONE_SHOT_SECONDS) {
    delay_seconds = MAX_ONE_SHOT_SECONDS;
  }

  s_timer = app_timer_register((uint32_t)delay_seconds * 1000, timer_handler, NULL);
}

static void schedule_next(void) {
  if (!load_state()) {
    return;
  }

  int32_t elapsed = current_elapsed();
  int32_t basis = s_last_vibe_elapsed;
  int32_t next_due = basis + s_interval_seconds;

  if (elapsed >= next_due) {
    vibes_short_pulse();
    s_last_vibe_elapsed = elapsed;
    persist_write_int(PERSIST_LAST_VIBE_ELAPSED, s_last_vibe_elapsed);
    update_app_glance(elapsed);
    send_vibed_message(elapsed);
    next_due = elapsed + s_interval_seconds;
  }

  schedule_seconds(next_due - elapsed);
}

static void worker_message_handler(uint16_t type, AppWorkerMessage *data) {
  if (type != WORKER_MSG_SYNC) {
    return;
  }

  if (s_timer) {
    app_timer_cancel(s_timer);
    s_timer = NULL;
  }
  schedule_next();
}

static bool init(void) {
  if (!load_state()) {
    return false;
  }

  app_worker_message_subscribe(worker_message_handler);
  schedule_next();
  return true;
}

static void deinit(void) {
  if (s_timer) {
    app_timer_cancel(s_timer);
  }
}

int main(void) {
  if (init()) {
    worker_event_loop();
    deinit();
  }
}
