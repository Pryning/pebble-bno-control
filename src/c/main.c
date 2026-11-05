#include <pebble.h>

// ---------------------------------------------------------------------------
// Protocol (must match src/pkjs/index.js)
// ---------------------------------------------------------------------------
#define MAX_DEVICES 16
enum { CMD_LIST = 1, CMD_SCAN = 2, CMD_CLEAR = 3, CMD_TOGGLE = 4, CMD_JOIN = 5, CMD_FOCUS = 6, CMD_VOLUME = 7, CMD_PREV = 8, CMD_NEXT = 9, CMD_POWER_OFF = 10, CMD_UNLINK = 11 };
enum { ST_IDLE = 0, ST_PLAYING = 1, ST_PAUSED = 2 };
enum { TYPE_ASE = 0, TYPE_MOZART = 1 };
enum { ROLE_NONE = 0, ROLE_LEADER = 1, ROLE_FOLLOWER = 2 };
#define SCAN_NONE -1
#define SCAN_DONE 101

typedef struct {
  char name[32];
  char source[24];
  char title[64];
  char artist[48];
  uint8_t type;
  uint8_t online;
  uint8_t state;
  uint8_t volume;
  uint8_t vol_max;
  uint8_t group;   // multiroom group id (0 = not grouped)
  uint8_t role;    // ROLE_*
} Device;

static Device s_devices[MAX_DEVICES];
static int s_count = 0;
static bool s_loaded = false;      // received at least one list from phone
static int s_scan = SCAN_NONE;     // 0..100 progress, SCAN_DONE when finished
static int s_found = 0;
static char s_toast[48];
static AppTimer *s_toast_timer;
static bool s_confirm_clear = false;
static AppTimer *s_confirm_timer;

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
static Window *s_home_window;
static Layer *s_home_layer;
static Window *s_list_window;
static StatusBarLayer *s_list_status;
static MenuLayer *s_list_menu;

static Window *s_device_window;
static Layer *s_device_layer;
static int s_cur = -1;

static Window *s_settings_window;
static MenuLayer *s_settings_menu;

// marquee
#define SCROLL_MS 60
#define SCROLL_STEP 2
#define SCROLL_PAUSE 25
static AppTimer *s_scroll_timer;
static uint32_t s_tick = 0;
static bool s_scroll_needed = false;
static int s_visible_windows = 0;

#define ACCENT GColorIslamicGreen
#define GROUP_COLOR GColorVividCerulean
#define GROUP_COLOR_HL GColorBlueMoon

static bool volume_held(void);
static void list_refresh(void);
static void user_activity(void);
static void device_state_changed(void);

// ---------------------------------------------------------------------------
// Messaging
// ---------------------------------------------------------------------------
// Small outgoing queue: AppMessage allows only one message in flight.
#define OUTQ_LEN 8
typedef struct { int32_t cmd, idx, value; } OutMsg;
static OutMsg s_outq[OUTQ_LEN];
static int s_outq_len = 0;
static bool s_out_busy = false;

static void outq_pump(void) {
  if (s_out_busy || s_outq_len == 0) return;
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return;
  OutMsg m = s_outq[0];
  dict_write_int32(it, MESSAGE_KEY_CMD, m.cmd);
  dict_write_int32(it, MESSAGE_KEY_IDX, m.idx);
  dict_write_int32(it, MESSAGE_KEY_VALUE, m.value);
  if (app_message_outbox_send() == APP_MSG_OK) s_out_busy = true;
}

static void outq_pop(void) {
  if (s_outq_len > 0) {
    memmove(&s_outq[0], &s_outq[1], sizeof(OutMsg) * (s_outq_len - 1));
    s_outq_len--;
  }
}

static void outbox_sent(DictionaryIterator *it, void *ctx) {
  s_out_busy = false;
  outq_pop();
  outq_pump();
}

static void outbox_failed(DictionaryIterator *it, AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "outbox failed: %d", (int)reason);
  s_out_busy = false;
  outq_pop();  // drop it; the phone may not be connected
  outq_pump();
}

static void send_cmd_value(int cmd, int idx, int value) {
  // coalesce repeated volume commands
  if (cmd == CMD_VOLUME) {
    for (int i = s_out_busy ? 1 : 0; i < s_outq_len; i++) {
      if (s_outq[i].cmd == CMD_VOLUME && s_outq[i].idx == idx) { s_outq[i].value = value; return; }
    }
  }
  if (s_outq_len == OUTQ_LEN) return;  // queue full: drop newest
  s_outq[s_outq_len++] = (OutMsg){ cmd, idx, value };
  outq_pump();
}

static void send_cmd(int cmd, int idx) { send_cmd_value(cmd, idx, 0); }

static void refresh_all(void) {
  list_refresh();
  if (s_settings_menu) menu_layer_reload_data(s_settings_menu);
  if (s_device_layer) layer_mark_dirty(s_device_layer);
  if (s_home_layer) layer_mark_dirty(s_home_layer);
}

static void toast_clear(void *ctx) {
  s_toast_timer = NULL;
  s_toast[0] = '\0';
  refresh_all();
}

static void show_toast(const char *msg) {
  snprintf(s_toast, sizeof(s_toast), "%s", msg);
  if (s_toast_timer) app_timer_cancel(s_toast_timer);
  s_toast_timer = app_timer_register(4000, toast_clear, NULL);
}

static void copy_str(char *dst, size_t n, Tuple *t) {
  if (t) snprintf(dst, n, "%s", t->value->cstring);
}

static void inbox_received(DictionaryIterator *it, void *ctx) {
  Tuple *t;
  if ((t = dict_find(it, MESSAGE_KEY_COUNT))) {
    int c = t->value->int32;
    s_count = c < 0 ? 0 : (c > MAX_DEVICES ? MAX_DEVICES : c);
    s_loaded = true;
    if (s_cur >= s_count && s_device_window && window_stack_contains_window(s_device_window)) {
      window_stack_remove(s_device_window, true);
    }
  }
  if ((t = dict_find(it, MESSAGE_KEY_IDX))) {
    int i = t->value->int32;
    if (i >= 0 && i < MAX_DEVICES) {
      Device *d = &s_devices[i];
      copy_str(d->name, sizeof(d->name), dict_find(it, MESSAGE_KEY_NAME));
      copy_str(d->source, sizeof(d->source), dict_find(it, MESSAGE_KEY_SOURCE));
      copy_str(d->title, sizeof(d->title), dict_find(it, MESSAGE_KEY_TITLE));
      copy_str(d->artist, sizeof(d->artist), dict_find(it, MESSAGE_KEY_ARTIST));
      if ((t = dict_find(it, MESSAGE_KEY_TYPE))) d->type = t->value->int32;
      if ((t = dict_find(it, MESSAGE_KEY_ONLINE))) d->online = t->value->int32;
      if ((t = dict_find(it, MESSAGE_KEY_STATE))) d->state = t->value->int32;
      bool hold = (i == s_cur) && volume_held();
      if (!hold && (t = dict_find(it, MESSAGE_KEY_VOLUME))) d->volume = t->value->int32;
      if ((t = dict_find(it, MESSAGE_KEY_VOLMAX))) d->vol_max = t->value->int32;
      if ((t = dict_find(it, MESSAGE_KEY_GROUP))) d->group = t->value->int32;
      if ((t = dict_find(it, MESSAGE_KEY_ROLE))) d->role = t->value->int32;
      if (i == s_cur) device_state_changed();
    }
  }
  if ((t = dict_find(it, MESSAGE_KEY_SCAN))) s_scan = t->value->int32;
  if ((t = dict_find(it, MESSAGE_KEY_FOUND))) s_found = t->value->int32;
  if ((t = dict_find(it, MESSAGE_KEY_TOAST))) show_toast(t->value->cstring);
  refresh_all();
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

// Draws text inside `area`; if it doesn't fit it scrolls as a marquee.
// Text may spill outside `area` horizontally: callers mask the margins.
static void draw_marquee(GContext *ctx, const char *text, GFont font, GRect area,
                         GTextAlignment align) {
  GSize sz = graphics_text_layout_get_content_size(
      text, font, GRect(0, 0, 2000, area.size.h), GTextOverflowModeTrailingEllipsis,
      GTextAlignmentLeft);
  if (sz.w <= area.size.w) {
    graphics_draw_text(ctx, text, font, area, GTextOverflowModeTrailingEllipsis, align, NULL);
    return;
  }
  s_scroll_needed = true;
  const int gap = 36;
  int period = sz.w + gap;
  int steps = period / SCROLL_STEP;
  int p = s_tick % (steps + SCROLL_PAUSE);
  int off = p < SCROLL_PAUSE ? 0 : (p - SCROLL_PAUSE) * SCROLL_STEP;
  GRect r = GRect(area.origin.x - off, area.origin.y, sz.w + 8, area.size.h);
  graphics_draw_text(ctx, text, font, r, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  r.origin.x += period;
  graphics_draw_text(ctx, text, font, r, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void draw_play_icon(GContext *ctx, GPoint c, int s, GColor color) {
  // right-pointing triangle, height 2s, centered on c
  GPathInfo info = {
    .num_points = 3,
    .points = (GPoint[]){ {-s * 2 / 3, -s}, {s * 4 / 3 - s * 2 / 3, 0}, {-s * 2 / 3, s} },
  };
  GPath *p = gpath_create(&info);
  gpath_move_to(p, c);
  graphics_context_set_fill_color(ctx, color);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

static void draw_pause_icon(GContext *ctx, GPoint c, int s, GColor color) {
  int bw = s * 2 / 3 > 2 ? s * 2 / 3 : 2;
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_rect(ctx, GRect(c.x - s + 1, c.y - s, bw, s * 2), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(c.x + s - bw, c.y - s, bw, s * 2), 0, GCornerNone);
}

static void draw_power_icon(GContext *ctx, GPoint c, int r, GColor color) {
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_arc(ctx, GRect(c.x - r, c.y - r, r * 2 + 1, r * 2 + 1), GOvalScaleModeFitCircle,
                    DEG_TO_TRIGANGLE(40), DEG_TO_TRIGANGLE(320));
  graphics_draw_line(ctx, GPoint(c.x, c.y - r - 1), GPoint(c.x, c.y - 1));
  graphics_context_set_stroke_width(ctx, 1);
}

static void device_line(const Device *d, char *buf, size_t n) {
  if (!d->online) {
    snprintf(buf, n, "Not reachable");
  } else if (d->state == ST_IDLE) {
    snprintf(buf, n, "Idle");
  } else if (d->title[0] && d->artist[0]) {
    snprintf(buf, n, "%s - %s", d->title, d->artist);
  } else if (d->title[0]) {
    snprintf(buf, n, "%s", d->title);
  } else if (d->source[0]) {
    snprintf(buf, n, "%s", d->source);
  } else {
    snprintf(buf, n, d->state == ST_PLAYING ? "Playing" : "Paused");
  }
}

// ---------------------------------------------------------------------------
// Marquee timer (only runs while one of our windows is visible)
// ---------------------------------------------------------------------------
static void scroll_tick(void *ctx) {
  s_scroll_timer = NULL;
  if (s_visible_windows <= 0) return;
  s_tick++;
  bool needed = s_scroll_needed;
  s_scroll_needed = false;
  if (needed) {
    Window *top = window_stack_get_top_window();
    if (top == s_list_window && s_list_menu) layer_mark_dirty(menu_layer_get_layer(s_list_menu));
    else if (top == s_device_window && s_device_layer) layer_mark_dirty(s_device_layer);
    else if (top == s_home_window && s_home_layer) layer_mark_dirty(s_home_layer);
    else s_scroll_needed = true;
  }
  s_scroll_timer = app_timer_register(needed ? SCROLL_MS : 400, scroll_tick, NULL);
}

static void scroll_start(void) {
  s_visible_windows++;
  s_scroll_needed = true;
  if (!s_scroll_timer) s_scroll_timer = app_timer_register(SCROLL_MS, scroll_tick, NULL);
}

static void scroll_stop(void) {
  s_visible_windows--;
  if (s_visible_windows <= 0 && s_scroll_timer) {
    app_timer_cancel(s_scroll_timer);
    s_scroll_timer = NULL;
  }
}

// ---------------------------------------------------------------------------
// Device window
//
// Works like the Pebble music app (system ActionBarLayer, music app icons,
// icon press animation, animated icon swaps). Select cycles button sets:
//   MODE_VOLUME : Up = volume up,  Down = volume down   (default)
//   MODE_PLAY   : Up = play/pause, Down = join
//   MODE_TRACK  : Up = previous,   Down = next
// When nothing is playing there is one set: Up = play, Down = join.
// ---------------------------------------------------------------------------
enum { MODE_VOLUME = 0, MODE_PLAY = 1, MODE_TRACK = 2, MODE_COUNT = 3 };

static ActionBarLayer *s_action_bar;
static GBitmap *s_icon_ellipsis, *s_icon_play, *s_icon_pause, *s_icon_skip_back,
               *s_icon_skip_fwd, *s_icon_vol_up, *s_icon_vol_down, *s_icon_join, *s_icon_unlink;
static GBitmap *s_bar_icons[3];  // currently shown (up, select, down)

static int s_mode = MODE_VOLUME;
static bool s_was_playing = false;
static AppTimer *s_vol_timer;
static time_t s_vol_hold_s;
static uint16_t s_vol_hold_ms;

// Music-app style text animations (offsets in px applied while drawing)
#define ANIM_FRAME_MS (1000 / 28)
#define BOUNCE_OFFSET 6
static Animation *s_track_anim;
static int s_track_dy = 0;    // now-playing title/artist
static Animation *s_state_anim;
static int s_state_dy = 0;    // PLAYING/PAUSED label

// "Powered off" feedback after a long press on pause
static AppTimer *s_off_timer;
static int s_off_count = 0;   // speakers turned off (0 = not showing)

static Device *cur_device(void) {
  return (s_cur >= 0 && s_cur < s_count) ? &s_devices[s_cur] : NULL;
}

static bool is_playing(const Device *d) {
  return d && d->online && d->state == ST_PLAYING;
}

// Button set actually in effect (one fixed set when nothing is playing).
static int effective_mode(const Device *d) {
  return is_playing(d) ? s_mode : MODE_PLAY;
}

static bool volume_held(void) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  int64_t now = (int64_t)s * 1000 + ms;
  int64_t until = (int64_t)s_vol_hold_s * 1000 + s_vol_hold_ms;
  return now < until;
}

static void hold_volume(int ms_ahead) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  int64_t t = (int64_t)s * 1000 + ms + ms_ahead;
  s_vol_hold_s = t / 1000;
  s_vol_hold_ms = t % 1000;
}

// --- animations ------------------------------------------------------------
// One animation = "slide up and out" (ease-in) followed by "appear slightly
// above and settle" (ease-out), like the music app's track change.
static int bounce_offset(AnimationProgress progress, int travel) {
  int32_t half = ANIMATION_NORMALIZED_MAX / 2;
  if (progress < half) {
    int32_t t = progress * 1000 / half;              // 0..1000
    return -(int)(travel * t * t / 1000000);          // ease in
  }
  int32_t t = (progress - half) * 1000 / half;       // 0..1000
  int32_t inv = 1000 - t;
  return -(int)(BOUNCE_OFFSET * inv * inv / 1000000);  // ease out
}

static void track_anim_update(Animation *a, const AnimationProgress p) {
  s_track_dy = bounce_offset(p, 40);
  if (s_device_layer) layer_mark_dirty(s_device_layer);
  if (s_home_layer) layer_mark_dirty(s_home_layer);
}

static void state_anim_update(Animation *a, const AnimationProgress p) {
  s_state_dy = bounce_offset(p, 22);
  if (s_device_layer) layer_mark_dirty(s_device_layer);
  if (s_home_layer) layer_mark_dirty(s_home_layer);
}

static void track_anim_teardown(Animation *a) { s_track_anim = NULL; s_track_dy = 0; }
static void state_anim_teardown(Animation *a) { s_state_anim = NULL; s_state_dy = 0; }

static const AnimationImplementation s_track_impl = {
  .update = track_anim_update, .teardown = track_anim_teardown,
};
static const AnimationImplementation s_state_impl = {
  .update = state_anim_update, .teardown = state_anim_teardown,
};

static void run_anim(Animation **slot, const AnimationImplementation *impl) {
  if (*slot) animation_unschedule(*slot);
  Animation *a = animation_create();
  animation_set_duration(a, 8 * ANIM_FRAME_MS);
  animation_set_curve(a, AnimationCurveLinear);
  animation_set_implementation(a, impl);
  *slot = a;
  animation_schedule(a);
}

// --- action bar ------------------------------------------------------------
static void device_click_config(void *ctx);

static void set_bar_icon(ButtonId id, GBitmap *icon, bool animated) {
  int slot = id == BUTTON_ID_UP ? 0 : (id == BUTTON_ID_SELECT ? 1 : 2);
  if (s_bar_icons[slot] == icon) return;
  s_bar_icons[slot] = icon;
  if (icon) action_bar_layer_set_icon_animated(s_action_bar, id, icon, animated);
  else action_bar_layer_clear_icon(s_action_bar, id);
}

static void update_action_bar(bool animated) {
  if (!s_action_bar) return;
  Device *d = cur_device();
  bool playing = is_playing(d);
  switch (effective_mode(d)) {
    case MODE_VOLUME:
      set_bar_icon(BUTTON_ID_UP, s_icon_vol_up, animated);
      set_bar_icon(BUTTON_ID_DOWN, s_icon_vol_down, animated);
      break;
    case MODE_PLAY:
      set_bar_icon(BUTTON_ID_UP, playing ? s_icon_pause : s_icon_play, animated);
      // already in a multiroom group: offer to unlink instead of join
      set_bar_icon(BUTTON_ID_DOWN, d && d->group ? s_icon_unlink : s_icon_join, animated);
      break;
    case MODE_TRACK:
      set_bar_icon(BUTTON_ID_UP, s_icon_skip_back, animated);
      set_bar_icon(BUTTON_ID_DOWN, s_icon_skip_fwd, animated);
      break;
  }
  set_bar_icon(BUTTON_ID_SELECT, playing ? s_icon_ellipsis : NULL, animated);
  // re-subscribe: volume buttons repeat while held, the others don't
  action_bar_layer_set_click_config_provider(s_action_bar, device_click_config);
}

// Called whenever the current device's data changes.
static void device_state_changed(void) {
  Device *d = cur_device();
  bool playing = is_playing(d);
  if (playing != s_was_playing) {
    // Starting playback from the single "nothing playing" set keeps the same
    // layout (play/pause + join) so the buttons don't jump around.
    if (playing) s_mode = MODE_PLAY;
    s_was_playing = playing;
  }
  update_action_bar(true);
}

// --- drawing ---------------------------------------------------------------
static void device_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  Device *d = cur_device();
  if (!d) return;

  const int x = 8;
  const int cw = b.size.w - x - 6;  // layer already excludes the action bar
  GFont f14 = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  GFont f18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont f18b = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont f24 = fonts_get_system_font(FONT_KEY_GOTHIC_24);
  GFont f24b = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GFont f28b = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  int mode = effective_mode(d);

  // Now-playing block (y 104..170): scrolls horizontally, animates vertically.
  const int top = 104, bottom = 170;
  const char *title = d->title[0] ? d->title : d->source;
  int ty = s_track_dy;
  if (s_off_count > 0) {
    static char obuf[32];
    if (s_off_count > 1) snprintf(obuf, sizeof(obuf), "Group of %d", s_off_count);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_off_count > 1 ? "Group off" : "Speaker off", f28b,
                       GRect(x, top + 2 + ty, cw, 34), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft, NULL);
    if (s_off_count > 1) {
      graphics_context_set_text_color(ctx, GColorLightGray);
      graphics_draw_text(ctx, obuf, f24, GRect(x, top + 34 + ty, cw, 30),
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    }
  } else if (d->online && d->state != ST_IDLE) {
    graphics_context_set_text_color(ctx, GColorWhite);
    draw_marquee(ctx, title, f28b, GRect(x, top + 2 + ty, cw, 34), GTextAlignmentLeft);
    if (d->title[0]) {
      graphics_context_set_text_color(ctx, GColorLightGray);
      draw_marquee(ctx, d->artist, f24, GRect(x, top + 34 + ty, cw, 30), GTextAlignmentLeft);
    }
  } else if (d->online) {
    graphics_context_set_text_color(ctx, GColorDarkGray);
    graphics_draw_text(ctx, "Nothing playing", f24, GRect(x, top + 6 + ty, cw, 30),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
  // mask everything outside the now-playing box
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 0, b.size.w, top), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(0, top, x, bottom - top), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(x + cw, top, b.size.w - x - cw, bottom - top), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(0, bottom, b.size.w, b.size.h - bottom), 0, GCornerNone);

  // Header
  graphics_context_set_text_color(ctx, d->online ? GColorWhite : GColorDarkGray);
  graphics_draw_text(ctx, d->name, f24b, GRect(x, 2, cw, 30), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, GColorLightGray);
  static char sub[64];
  const char *plat = d->type == TYPE_MOZART ? "Mozart" : "ASE";
  const char *leader_name = NULL;
  if (d->group && d->role == ROLE_FOLLOWER) {
    for (int j = 0; j < s_count; j++) {
      if (s_devices[j].group == d->group && s_devices[j].role == ROLE_LEADER) leader_name = s_devices[j].name;
    }
  }
  if (d->group && d->role == ROLE_LEADER) snprintf(sub, sizeof(sub), "%s  \u00b7  Multiroom leader", plat);
  else if (leader_name) snprintf(sub, sizeof(sub), "%s  \u00b7  With %s", plat, leader_name);
  else if (d->group) snprintf(sub, sizeof(sub), "%s  \u00b7  Multiroom", plat);
  else snprintf(sub, sizeof(sub), "%s", d->type == TYPE_MOZART ? "Mozart" : "ASE / BeoZone");
  graphics_context_set_text_color(ctx, d->group ? GROUP_COLOR : GColorLightGray);
  graphics_draw_text(ctx, sub, f14,
                     GRect(x, 32, cw, 18), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_draw_line(ctx, GPoint(x, 54), GPoint(x + cw, 54));

  // State (bounces on play/pause), clipped to its own row
  const char *label;
  GColor scol;
  bool off = s_off_count > 0;
  if (off) { label = "POWERED OFF"; scol = GColorRed; }
  else if (!d->online) { label = "OFFLINE"; scol = GColorDarkGray; }
  else if (d->state == ST_PLAYING) { label = "PLAYING"; scol = GColorGreen; }
  else if (d->state == ST_PAUSED) { label = "PAUSED"; scol = GColorChromeYellow; }
  else { label = "IDLE"; scol = GColorLightGray; }
  int sy = s_state_dy;
  GPoint sc = GPoint(x + 6, 70 + sy);
  if (off) draw_power_icon(ctx, sc, 7, scol);
  else if (d->online && d->state == ST_PLAYING) draw_play_icon(ctx, sc, 7, scol);
  else if (d->online && d->state == ST_PAUSED) draw_pause_icon(ctx, sc, 6, scol);
  else {
    graphics_context_set_fill_color(ctx, scol);
    graphics_fill_circle(ctx, sc, 4);
  }
  graphics_context_set_text_color(ctx, scol);
  graphics_draw_text(ctx, label, f18b, GRect(x + 18, 58 + sy, cw - 18, 22),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 55, b.size.w, 3), 0, GCornerNone);  // clip top of state row
  graphics_fill_rect(ctx, GRect(0, 80, b.size.w, 24), 0, GCornerNone);   // clear source row
  if (!off && d->online && d->source[0] && (d->title[0] || d->state == ST_IDLE)) {
    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, d->source, f18, GRect(x, 80, cw, 22), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft, NULL);
  }

  // Volume bar (highlighted while the volume buttons are active)
  if (!off && d->online && d->vol_max > 0) {
    bool active = mode == MODE_VOLUME;
    int vy = 178;
    static char vbuf[8];
    snprintf(vbuf, sizeof(vbuf), "%d", d->volume);
    graphics_context_set_text_color(ctx, active ? GColorWhite : GColorDarkGray);
    graphics_draw_text(ctx, "VOL", f14, GRect(x, vy - 5, 28, 18), GTextOverflowModeFill,
                       GTextAlignmentLeft, NULL);
    graphics_draw_text(ctx, vbuf, f14, GRect(x + cw - 26, vy - 5, 26, 18), GTextOverflowModeFill,
                       GTextAlignmentRight, NULL);
    int bx = x + 30, bw = cw - 30 - 30;
    int fill = bw * d->volume / d->vol_max;
    graphics_context_set_fill_color(ctx, GColorDarkGray);
    graphics_fill_rect(ctx, GRect(bx, vy + 2, bw, 5), 2, GCornersAll);
    graphics_context_set_fill_color(ctx, active ? GColorWhite : GColorLightGray);
    if (fill > 0) graphics_fill_rect(ctx, GRect(bx, vy + 2, fill, 5), 2, GCornersAll);
  }

  // Toast
  if (s_toast[0]) {
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_toast, f14, GRect(x, b.size.h - 26, cw, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }

  // hairline separating the content from the (black) action bar
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_draw_line(ctx, GPoint(b.size.w - 1, 0), GPoint(b.size.w - 1, b.size.h));
}

// --- buttons ---------------------------------------------------------------
static void vol_send(void *ctx) {
  s_vol_timer = NULL;
  Device *d = cur_device();
  if (d) send_cmd_value(CMD_VOLUME, s_cur, d->volume);
}

static void change_volume(int dir) {
  Device *d = cur_device();
  if (!d || d->vol_max == 0) return;
  int step = d->vol_max >= 60 ? 2 : 1;
  int v = d->volume + dir * step;
  if (v < 0) v = 0;
  if (v > d->vol_max) v = d->vol_max;
  d->volume = v;
  hold_volume(3000);
  if (s_vol_timer) app_timer_reschedule(s_vol_timer, 200);
  else s_vol_timer = app_timer_register(200, vol_send, NULL);
  layer_mark_dirty(s_device_layer);
}

static bool check_online(void) {
  Device *d = cur_device();
  if (!d) return false;
  if (!d->online) {
    show_toast("Device not reachable");
    refresh_all();
    return false;
  }
  return true;
}

static void do_play_pause(void) {
  Device *d = cur_device();
  bool was_playing = is_playing(d);
  d->state = was_playing ? ST_PAUSED : ST_PLAYING;  // optimistic
  send_cmd(CMD_TOGGLE, s_cur);
  run_anim(&s_state_anim, &s_state_impl);
  device_state_changed();
  refresh_all();
}

static void do_join(void) {
  send_cmd(CMD_JOIN, s_cur);
  show_toast("Joining...");
  vibes_short_pulse();
  refresh_all();
}

static void do_unlink(void) {
  Device *d = cur_device();
  bool leader = d->role == ROLE_LEADER;
  send_cmd(CMD_UNLINK, s_cur);
  show_toast(leader ? "Releasing group..." : "Leaving group...");
  vibes_short_pulse();
  refresh_all();
}

static void do_skip(bool forward) {
  send_cmd(forward ? CMD_NEXT : CMD_PREV, s_cur);
  run_anim(&s_track_anim, &s_track_impl);
}

static void device_up(ClickRecognizerRef r, void *ctx) {
  user_activity();
  if (!check_online()) return;
  switch (effective_mode(cur_device())) {
    case MODE_VOLUME: change_volume(1); break;
    case MODE_PLAY: do_play_pause(); break;
    case MODE_TRACK: do_skip(false); break;
  }
}

static void device_down(ClickRecognizerRef r, void *ctx) {
  user_activity();
  if (!check_online()) return;
  switch (effective_mode(cur_device())) {
    case MODE_VOLUME: change_volume(-1); break;
    case MODE_PLAY: if (cur_device()->group) do_unlink(); else do_join(); break;
    case MODE_TRACK: do_skip(true); break;
  }
}

static void off_done(void *ctx) {
  s_off_timer = NULL;
  s_off_count = 0;
  if (s_device_layer) layer_mark_dirty(s_device_layer);
  if (s_home_layer) layer_mark_dirty(s_home_layer);
}

// Long press on pause: standby (the whole group when this is the leader).
static void device_up_long(ClickRecognizerRef r, void *ctx) {
  user_activity();
  Device *d = cur_device();
  if (!is_playing(d) || effective_mode(d) != MODE_PLAY) return;
  int n = 1;
  if (d->group && d->role == ROLE_LEADER) {
    n = 0;
    for (int j = 0; j < s_count; j++) {
      if (s_devices[j].group == d->group) { s_devices[j].state = ST_IDLE; n++; }
    }
  }
  d->state = ST_IDLE;
  send_cmd(CMD_POWER_OFF, s_cur);
  static const uint32_t pattern[] = { 60, 80, 200 };
  vibes_enqueue_custom_pattern((VibePattern){ .durations = pattern, .num_segments = 3 });
  s_off_count = n;
  if (s_off_timer) app_timer_reschedule(s_off_timer, 3500);
  else s_off_timer = app_timer_register(3500, off_done, NULL);
  run_anim(&s_state_anim, &s_state_impl);
  run_anim(&s_track_anim, &s_track_impl);
  device_state_changed();
  refresh_all();
}

static void device_select(ClickRecognizerRef r, void *ctx) {
  user_activity();
  if (!is_playing(cur_device())) return;  // only one button set
  s_mode = (s_mode + 1) % MODE_COUNT;
  update_action_bar(true);
  layer_mark_dirty(s_device_layer);
}

static void device_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, device_select);
  if (effective_mode(cur_device()) == MODE_VOLUME) {
    // hold to keep changing the volume
    window_single_repeating_click_subscribe(BUTTON_ID_UP, 150, device_up);
    window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, device_down);
  } else {
    window_single_click_subscribe(BUTTON_ID_UP, device_up);
    window_single_click_subscribe(BUTTON_ID_DOWN, device_down);
    if (effective_mode(cur_device()) == MODE_PLAY && is_playing(cur_device())) {
      // hold pause to power off (single click still pauses)
      window_long_click_subscribe(BUTTON_ID_UP, 700, device_up_long, NULL);
    }
  }
}

static void device_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_device_layer = layer_create(GRect(0, 0, b.size.w - ACTION_BAR_WIDTH, b.size.h));
  layer_set_update_proc(s_device_layer, device_update_proc);
  layer_add_child(root, s_device_layer);

  s_icon_ellipsis = gbitmap_create_with_resource(RESOURCE_ID_ICON_ELLIPSIS);
  s_icon_play = gbitmap_create_with_resource(RESOURCE_ID_ICON_PLAY);
  s_icon_pause = gbitmap_create_with_resource(RESOURCE_ID_ICON_PAUSE);
  s_icon_skip_back = gbitmap_create_with_resource(RESOURCE_ID_ICON_SKIP_BACKWARD);
  s_icon_skip_fwd = gbitmap_create_with_resource(RESOURCE_ID_ICON_SKIP_FORWARD);
  s_icon_vol_up = gbitmap_create_with_resource(RESOURCE_ID_ICON_VOLUME_UP);
  s_icon_vol_down = gbitmap_create_with_resource(RESOURCE_ID_ICON_VOLUME_DOWN);
  s_icon_join = gbitmap_create_with_resource(RESOURCE_ID_ICON_JOIN);
  s_icon_unlink = gbitmap_create_with_resource(RESOURCE_ID_ICON_UNLINK);

  s_action_bar = action_bar_layer_create();
  action_bar_layer_set_background_color(s_action_bar, GColorBlack);
  action_bar_layer_add_to_window(s_action_bar, w);
  s_bar_icons[0] = s_bar_icons[1] = s_bar_icons[2] = NULL;
  update_action_bar(false);
}

static void device_window_unload(Window *w) {
  if (s_off_timer) { app_timer_cancel(s_off_timer); s_off_timer = NULL; }
  s_off_count = 0;
  if (s_track_anim) animation_unschedule(s_track_anim);
  if (s_state_anim) animation_unschedule(s_state_anim);
  action_bar_layer_destroy(s_action_bar);
  s_action_bar = NULL;
  GBitmap **icons[] = { &s_icon_ellipsis, &s_icon_play, &s_icon_pause, &s_icon_skip_back,
                        &s_icon_skip_fwd, &s_icon_vol_up, &s_icon_vol_down, &s_icon_join,
                        &s_icon_unlink };
  for (unsigned i = 0; i < ARRAY_LENGTH(icons); i++) {
    gbitmap_destroy(*icons[i]);
    *icons[i] = NULL;
  }
  layer_destroy(s_device_layer);
  s_device_layer = NULL;
}

static void device_window_appear(Window *w) {
  scroll_start();
  send_cmd(CMD_FOCUS, s_cur);
}

static void device_window_disappear(Window *w) {
  scroll_stop();
  if (s_vol_timer) { app_timer_cancel(s_vol_timer); vol_send(NULL); }
  send_cmd(CMD_FOCUS, -1);
}

static void open_device(int idx) {
  s_cur = idx;
  s_toast[0] = '\0';
  s_mode = MODE_VOLUME;
  s_was_playing = is_playing(cur_device());
  window_stack_push(s_device_window, true);
}

// ---------------------------------------------------------------------------
// Settings window
// ---------------------------------------------------------------------------
static uint16_t settings_rows(MenuLayer *m, uint16_t s, void *ctx) { return 2; }

static int16_t settings_header_h(MenuLayer *m, uint16_t s, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void settings_header(GContext *ctx, const Layer *cell, uint16_t s, void *data) {
  menu_cell_basic_header_draw(ctx, cell, "  SETTINGS");
}

static void settings_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  static char sub[40];
  if (idx->row == 0) {
    if (s_scan >= 0 && s_scan <= 100) snprintf(sub, sizeof(sub), "Scanning... %d%%", s_scan);
    else if (s_scan == SCAN_DONE) snprintf(sub, sizeof(sub), "Found %d device%s", s_found, s_found == 1 ? "" : "s");
    else snprintf(sub, sizeof(sub), "Find Mozart & ASE devices");
    menu_cell_basic_draw(ctx, cell, "Scan network", sub, NULL);
  } else {
    if (s_confirm_clear) snprintf(sub, sizeof(sub), "Press again to confirm");
    else snprintf(sub, sizeof(sub), "%d known device%s", s_count, s_count == 1 ? "" : "s");
    menu_cell_basic_draw(ctx, cell, "Delete all devices", sub, NULL);
  }
  // progress bar while scanning
  if (idx->row == 0 && s_scan >= 0 && s_scan <= 100) {
    GRect b = layer_get_bounds(cell);
    int w = (b.size.w - 20) * s_scan / 100;
    graphics_context_set_fill_color(ctx, ACCENT);
    graphics_fill_rect(ctx, GRect(10, b.size.h - 5, w, 3), 0, GCornerNone);
  }
}

static void confirm_timeout(void *ctx) {
  s_confirm_timer = NULL;
  s_confirm_clear = false;
  refresh_all();
}

static void settings_selection_changed(MenuLayer *m, MenuIndex n, MenuIndex o, void *ctx) {
  user_activity();
}

static void settings_select(MenuLayer *m, MenuIndex *idx, void *ctx) {
  user_activity();
  if (idx->row == 0) {
    if (s_scan >= 0 && s_scan <= 100) return;  // already scanning
    s_scan = 0;
    s_found = 0;
    send_cmd(CMD_SCAN, 0);
  } else {
    if (!s_confirm_clear) {
      s_confirm_clear = true;
      if (s_confirm_timer) app_timer_cancel(s_confirm_timer);
      s_confirm_timer = app_timer_register(3000, confirm_timeout, NULL);
    } else {
      s_confirm_clear = false;
      if (s_confirm_timer) { app_timer_cancel(s_confirm_timer); s_confirm_timer = NULL; }
      s_count = 0;
      s_scan = SCAN_NONE;
      send_cmd(CMD_CLEAR, 0);
      vibes_short_pulse();
    }
  }
  refresh_all();
}

static void settings_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_settings_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_settings_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = settings_rows,
    .get_header_height = settings_header_h,
    .draw_header = settings_header,
    .draw_row = settings_draw_row,
    .select_click = settings_select,
    .selection_changed = settings_selection_changed,
  });
  menu_layer_set_normal_colors(s_settings_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_settings_menu, GColorWhite, GColorBlack);
  menu_layer_set_click_config_onto_window(s_settings_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_settings_menu));
}

static void settings_window_unload(Window *w) {
  menu_layer_destroy(s_settings_menu);
  s_settings_menu = NULL;
}

// ---------------------------------------------------------------------------
// Device list (main) window
// ---------------------------------------------------------------------------
#define ROW_H 52

enum { SEC_ACTIVE = 0, SEC_DEVICES = 1, SEC_SETTINGS = 2 };

// Display order: active devices first (multiroom groups with the leader on
// top, then single active devices), then everything else.
static uint8_t s_order[MAX_DEVICES];
static int s_active_n = 0;   // rows in the ACTIVE section
static int s_other_n = 0;    // rows in the DEVICES section
static bool s_list_touched = false;  // user moved the selection
static bool s_list_programmatic = false;

static bool is_active(const Device *d) { return d->online && d->state != ST_IDLE; }

// Name used to sort a group: its leader's (or first member's) name.
static const char *group_key(int i) {
  const Device *d = &s_devices[i];
  if (!d->group) return d->name;
  const char *best = NULL;
  for (int j = 0; j < s_count; j++) {
    const Device *o = &s_devices[j];
    if (o->group != d->group) continue;
    if (o->role == ROLE_LEADER) return o->name;
    if (!best || strcmp(o->name, best) < 0) best = o->name;
  }
  return best ? best : d->name;
}

static int order_cmp(int a, int b) {
  const Device *da = &s_devices[a], *db = &s_devices[b];
  bool aa = is_active(da), ab = is_active(db);
  if (aa != ab) return aa ? -1 : 1;
  if (aa) {
    bool ga = da->group != 0, gb = db->group != 0;
    if (ga != gb) return ga ? -1 : 1;   // multiroom groups above single devices
    if (ga) {
      int c = strcmp(group_key(a), group_key(b));
      if (c) return c;
      if (da->group != db->group) return da->group - db->group;
      bool la = da->role == ROLE_LEADER, lb = db->role == ROLE_LEADER;
      if (la != lb) return la ? -1 : 1;  // leader on top
    }
  } else if (da->online != db->online) {
    return da->online ? -1 : 1;          // unreachable last
  }
  int c = strcmp(da->name, db->name);
  return c ? c : a - b;
}

static void rebuild_order(void) {
  for (int i = 0; i < s_count; i++) s_order[i] = i;
  for (int i = 1; i < s_count; i++) {  // insertion sort, n <= 16
    uint8_t v = s_order[i];
    int j = i - 1;
    while (j >= 0 && order_cmp(s_order[j], v) > 0) { s_order[j + 1] = s_order[j]; j--; }
    s_order[j + 1] = v;
  }
  s_active_n = 0;
  while (s_active_n < s_count && is_active(&s_devices[s_order[s_active_n]])) s_active_n++;
  s_other_n = s_count - s_active_n;
}

// Device index shown at a menu position, or -1.
static int device_at(MenuIndex idx) {
  if (idx.section == SEC_ACTIVE && idx.row < s_active_n) return s_order[idx.row];
  if (idx.section == SEC_DEVICES && idx.row < s_other_n) return s_order[s_active_n + idx.row];
  return -1;
}

static MenuIndex index_of_device(int dev) {
  for (int r = 0; r < s_count; r++) {
    if (s_order[r] == dev) {
      return r < s_active_n ? (MenuIndex){ SEC_ACTIVE, r } : (MenuIndex){ SEC_DEVICES, r - s_active_n };
    }
  }
  return (MenuIndex){ SEC_DEVICES, 0 };
}

static void list_refresh(void) {
  if (!s_list_menu) { rebuild_order(); return; }
  MenuIndex sel = menu_layer_get_selected_index(s_list_menu);
  int dev = device_at(sel);
  rebuild_order();
  s_list_programmatic = true;
  menu_layer_reload_data(s_list_menu);
  // keep the same device selected when rows move around
  // (until the user scrolls, stay on the top row)
  if (s_list_touched && dev >= 0) {
    menu_layer_set_selected_index(s_list_menu, index_of_device(dev), MenuRowAlignNone, false);
  } else if (!s_list_touched) {
    menu_layer_set_selected_index(s_list_menu, (MenuIndex){ s_active_n ? SEC_ACTIVE : SEC_DEVICES, 0 },
                                  MenuRowAlignNone, false);
  }
  s_list_programmatic = false;
}

static void list_selection_changed(MenuLayer *m, MenuIndex new_idx, MenuIndex old_idx, void *ctx) {
  if (!s_list_programmatic) { s_list_touched = true; user_activity(); }
}

static uint16_t list_sections(MenuLayer *m, void *ctx) { return 3; }

static uint16_t list_rows(MenuLayer *m, uint16_t s, void *ctx) {
  if (s == SEC_ACTIVE) return s_active_n;
  if (s == SEC_DEVICES) return s_count == 0 ? 1 : s_other_n;
  return 1;
}

static int16_t list_cell_h(MenuLayer *m, MenuIndex *idx, void *ctx) {
  return idx->section == SEC_SETTINGS ? 40 : ROW_H;
}

static int16_t list_header_h(MenuLayer *m, uint16_t s, void *ctx) {
  if (s == SEC_ACTIVE && s_active_n == 0) return 0;
  if (s == SEC_DEVICES && s_count > 0 && s_other_n == 0) return 0;
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void list_header(GContext *ctx, const Layer *cell, uint16_t s, void *data) {
  menu_cell_basic_header_draw(ctx, cell, s == SEC_ACTIVE ? "ACTIVE" : (s == SEC_DEVICES ? "DEVICES" : ""));
}

static void draw_device_row(GContext *ctx, const Layer *cell, const Device *d, bool hl,
                            bool group_above, bool group_below) {
  GRect b = layer_get_bounds(cell);
  GColor bg = hl ? GColorWhite : GColorBlack;
  GColor fg = d->online ? (hl ? GColorBlack : GColorWhite) : (hl ? GColorLightGray : GColorDarkGray);
  GColor sub = d->online ? (hl ? GColorDarkGray : GColorLightGray) : fg;
  GColor gcol = hl ? GROUP_COLOR_HL : GROUP_COLOR;
  bool grouped = d->group != 0 && is_active(d);
  bool leader = grouped && d->role == ROLE_LEADER;
  const int tx = 26, rm = 8;
  GRect area = GRect(tx, 24, b.size.w - tx - rm, 22);

  static char line[120];
  device_line(d, line, sizeof(line));
  GFont f18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  graphics_context_set_text_color(ctx, sub);
  if (is_active(d)) draw_marquee(ctx, line, f18, area, GTextAlignmentLeft);
  else graphics_draw_text(ctx, line, f18, area, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // mask margins around the marquee
  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, GRect(0, 24, tx, 26), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(b.size.w - rm, 24, rm, 26), 0, GCornerNone);

  // name (+ LEADER tag)
  int name_w = b.size.w - 16;
  if (leader) {
    GFont fb = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
    GRect tag = GRect(b.size.w - 8 - 50, 6, 50, 16);
    graphics_context_set_text_color(ctx, gcol);
    graphics_draw_text(ctx, "LEADER", fb, tag, GTextOverflowModeFill, GTextAlignmentRight, NULL);
    name_w -= 54;
  }
  graphics_context_set_text_color(ctx, fg);
  graphics_draw_text(ctx, d->name, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(grouped ? 10 : 8, -2, name_w, 28), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);

  // multiroom bracket on the left edge, joining the rows of one group
  if (grouped) {
    int mid = 13;  // aligned with the name line
    int y0 = group_above ? 0 : mid, y1 = group_below ? b.size.h : mid;
    graphics_context_set_fill_color(ctx, gcol);
    if (y1 > y0) graphics_fill_rect(ctx, GRect(2, y0, 3, y1 - y0), 0, GCornerNone);
    if (leader) graphics_fill_circle(ctx, GPoint(3, mid), 3);
    else graphics_fill_rect(ctx, GRect(2, mid - 1, 6, 3), 0, GCornerNone);
  }

  GPoint ic = GPoint(15, 36);
  if (d->online && d->state == ST_PLAYING) draw_play_icon(ctx, ic, 6, hl ? ACCENT : GColorGreen);
  else if (d->online && d->state == ST_PAUSED) draw_pause_icon(ctx, ic, 5, hl ? GColorDarkGray : GColorLightGray);
  else if (!d->online) {
    graphics_context_set_stroke_color(ctx, fg);
    graphics_draw_circle(ctx, ic, 4);
  } else {
    graphics_context_set_fill_color(ctx, sub);
    graphics_fill_circle(ctx, ic, 3);
  }
}

static void list_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  bool hl = menu_cell_layer_is_highlighted(cell);
  if (idx->section == SEC_SETTINGS) {
    GRect b = layer_get_bounds(cell);
    graphics_context_set_text_color(ctx, hl ? GColorBlack : GColorWhite);
    graphics_draw_text(ctx, "Settings", fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                       GRect(8, 2, b.size.w - 30, 30), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft, NULL);
    // chevron
    graphics_context_set_stroke_color(ctx, hl ? GColorBlack : GColorLightGray);
    graphics_context_set_stroke_width(ctx, 2);
    int cx = b.size.w - 16, cy = b.size.h / 2;
    graphics_draw_line(ctx, GPoint(cx - 4, cy - 6), GPoint(cx + 2, cy));
    graphics_draw_line(ctx, GPoint(cx + 2, cy), GPoint(cx - 4, cy + 6));
    graphics_context_set_stroke_width(ctx, 1);
    return;
  }
  if (s_count == 0) {
    menu_cell_basic_draw(ctx, cell, s_loaded ? "No devices" : "Loading...",
                         s_loaded ? "Scan in Settings" : "Waiting for phone", NULL);
    return;
  }
  int dev = device_at(*idx);
  if (dev < 0) return;
  const Device *d = &s_devices[dev];
  bool above = false, below = false;
  if (idx->section == SEC_ACTIVE && d->group) {
    if (idx->row > 0) above = s_devices[s_order[idx->row - 1]].group == d->group;
    if (idx->row + 1 < s_active_n) below = s_devices[s_order[idx->row + 1]].group == d->group;
  }
  draw_device_row(ctx, cell, d, hl, above, below);
}

static void list_select(MenuLayer *m, MenuIndex *idx, void *ctx) {
  user_activity();
  int dev = device_at(*idx);
  if (idx->section == SEC_SETTINGS || s_count == 0) {
    window_stack_push(s_settings_window, true);
  } else if (dev >= 0) {
    open_device(dev);
  }
}

static void list_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_list_status = status_bar_layer_create();
  status_bar_layer_set_colors(s_list_status, GColorBlack, GColorWhite);
  status_bar_layer_set_separator_mode(s_list_status, StatusBarLayerSeparatorModeNone);
  layer_add_child(root, status_bar_layer_get_layer(s_list_status));

  s_list_menu = menu_layer_create(GRect(0, STATUS_BAR_LAYER_HEIGHT, b.size.w,
                                        b.size.h - STATUS_BAR_LAYER_HEIGHT));
  menu_layer_set_callbacks(s_list_menu, NULL, (MenuLayerCallbacks){
    .get_num_sections = list_sections,
    .get_num_rows = list_rows,
    .get_cell_height = list_cell_h,
    .get_header_height = list_header_h,
    .draw_header = list_header,
    .draw_row = list_draw_row,
    .select_click = list_select,
    .selection_changed = list_selection_changed,
  });
  menu_layer_set_normal_colors(s_list_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_list_menu, GColorWhite, GColorBlack);
  menu_layer_set_click_config_onto_window(s_list_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_list_menu));
}

static void list_window_unload(Window *w) {
  menu_layer_destroy(s_list_menu);
  s_list_menu = NULL;
  status_bar_layer_destroy(s_list_status);
}

static void list_window_appear(Window *w) { scroll_start(); refresh_all(); }
static void list_window_disappear(Window *w) { scroll_stop(); }

// ---------------------------------------------------------------------------
// Home (landing) window
//
// Shows what's playing: the multiroom leader if speakers play together,
// otherwise the first playing device in list order. Any button opens the
// device list. After 20 s without a button press the app returns here.
// ---------------------------------------------------------------------------
#define HOME_TIMEOUT_MS 20000

static StatusBarLayer *s_home_status;
static AppTimer *s_idle_timer;

// Device to feature on the home page, or -1 if nothing is playing.
static int home_device(void) {
  rebuild_order();
  for (int r = 0; r < s_count; r++) {
    const Device *d = &s_devices[s_order[r]];
    if (d->online && d->state == ST_PLAYING) return s_order[r];
  }
  return -1;
}

static void go_home(void *ctx) {
  s_idle_timer = NULL;
  if (s_scan >= 0 && s_scan <= 100) {  // don't interrupt a running scan
    s_idle_timer = app_timer_register(HOME_TIMEOUT_MS, go_home, NULL);
    return;
  }
  if (window_stack_contains_window(s_settings_window)) window_stack_remove(s_settings_window, false);
  if (window_stack_contains_window(s_device_window)) window_stack_remove(s_device_window, false);
  if (window_stack_contains_window(s_list_window)) window_stack_remove(s_list_window, true);
}

// Restart the inactivity countdown (called from every button handler).
static void user_activity(void) {
  if (window_stack_get_top_window() == s_home_window) return;
  if (s_idle_timer) app_timer_reschedule(s_idle_timer, HOME_TIMEOUT_MS);
  else s_idle_timer = app_timer_register(HOME_TIMEOUT_MS, go_home, NULL);
}

static void home_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  const int x = 10, cw = b.size.w - 2 * x;
  GFont f14 = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  GFont f18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont f18b = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont f24 = fonts_get_system_font(FONT_KEY_GOTHIC_24);
  GFont f24b = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GFont f28b = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);

  int idx = home_device();
  if (idx < 0) {
    // Nothing playing
    const char *t1, *t2;
    static char sub[40];
    if (!s_loaded) { t1 = "Connecting..."; t2 = "Waiting for phone"; }
    else if (s_count == 0) { t1 = "No devices"; t2 = "Press a button, then scan in Settings"; }
    else {
      int online = 0;
      for (int i = 0; i < s_count; i++) online += s_devices[i].online;
      snprintf(sub, sizeof(sub), "%d of %d speaker%s reachable", online, s_count, s_count == 1 ? "" : "s");
      t1 = "Nothing playing";
      t2 = sub;
    }
    int cy = b.size.h / 2 - 30;
    graphics_context_set_fill_color(ctx, GColorDarkGray);
    graphics_fill_circle(ctx, GPoint(b.size.w / 2, cy - 16), 14);
    draw_play_icon(ctx, GPoint(b.size.w / 2 + 2, cy - 16), 7, GColorBlack);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, t1, f24b, GRect(x, cy + 4, cw, 30), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, t2, f18, GRect(x, cy + 34, cw, 44), GTextOverflowModeWordWrap,
                       GTextAlignmentCenter, NULL);
  } else {
    const Device *d = &s_devices[idx];
    // now-playing block first (scrolling), then mask the margins
    const int top = 98, bottom = 162;
    const char *title = d->title[0] ? d->title : d->source;
    graphics_context_set_text_color(ctx, GColorWhite);
    draw_marquee(ctx, title, f28b, GRect(x, top, cw, 34), GTextAlignmentLeft);
    if (d->title[0]) {
      graphics_context_set_text_color(ctx, GColorLightGray);
      draw_marquee(ctx, d->artist, f24, GRect(x, top + 32, cw, 30), GTextAlignmentLeft);
    }
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, GRect(0, top, x, bottom - top), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(x + cw, top, b.size.w - x - cw, bottom - top), 0, GCornerNone);

    // speaker name + multiroom info
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, d->name, f24b, GRect(x, 0, cw, 30), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft, NULL);
    static char sub[48];
    int members = 0, others = 0;
    for (int i = 0; i < s_count; i++) {
      const Device *o = &s_devices[i];
      if (d->group && o->group == d->group) members++;
      else if (i != idx && o->online && o->state == ST_PLAYING) others++;
    }
    if (d->group) {
      snprintf(sub, sizeof(sub), "Multiroom · %d speakers", members);
      graphics_context_set_text_color(ctx, GROUP_COLOR);
    } else {
      // the source is already the big title when there's no track title
      snprintf(sub, sizeof(sub), "%s", d->title[0] ? d->source
                                       : (d->type == TYPE_MOZART ? "Mozart" : "ASE / BeoZone"));
      graphics_context_set_text_color(ctx, GColorLightGray);
    }
    graphics_draw_text(ctx, sub, f18, GRect(x, 28, cw, 22), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft, NULL);
    graphics_context_set_stroke_color(ctx, GColorDarkGray);
    graphics_draw_line(ctx, GPoint(x, 56), GPoint(x + cw, 56));

    draw_play_icon(ctx, GPoint(x + 6, 72), 7, GColorGreen);
    graphics_context_set_text_color(ctx, GColorGreen);
    graphics_draw_text(ctx, "PLAYING", f18b, GRect(x + 18, 60, cw - 18, 22),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    // volume
    if (d->vol_max > 0) {
      int vy = 172;
      static char vbuf[8];
      snprintf(vbuf, sizeof(vbuf), "%d", d->volume);
      graphics_context_set_text_color(ctx, GColorDarkGray);
      graphics_draw_text(ctx, "VOL", f14, GRect(x, vy - 5, 28, 18), GTextOverflowModeFill,
                         GTextAlignmentLeft, NULL);
      graphics_draw_text(ctx, vbuf, f14, GRect(x + cw - 26, vy - 5, 26, 18), GTextOverflowModeFill,
                         GTextAlignmentRight, NULL);
      int bx = x + 30, bw = cw - 60;
      graphics_context_set_fill_color(ctx, GColorDarkGray);
      graphics_fill_rect(ctx, GRect(bx, vy + 2, bw, 5), 2, GCornersAll);
      int fill = bw * d->volume / d->vol_max;
      graphics_context_set_fill_color(ctx, GColorLightGray);
      if (fill > 0) graphics_fill_rect(ctx, GRect(bx, vy + 2, fill, 5), 2, GCornersAll);
    }

    // other speakers playing separately
    if (others > 0) {
      static char obuf[32];
      snprintf(obuf, sizeof(obuf), "+%d more playing", others);
      graphics_context_set_text_color(ctx, GColorLightGray);
      graphics_draw_text(ctx, obuf, f14, GRect(x, b.size.h - 22, cw, 18),
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    }
  }
}

static void home_click(ClickRecognizerRef r, void *ctx) {
  s_list_touched = false;  // list opens on its top row
  window_stack_push(s_list_window, true);
  user_activity();
}

static void home_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, home_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, home_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, home_click);
}

static void home_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  s_home_status = status_bar_layer_create();
  status_bar_layer_set_colors(s_home_status, GColorBlack, GColorWhite);
  status_bar_layer_set_separator_mode(s_home_status, StatusBarLayerSeparatorModeNone);
  layer_add_child(root, status_bar_layer_get_layer(s_home_status));
  s_home_layer = layer_create(GRect(0, STATUS_BAR_LAYER_HEIGHT, b.size.w,
                                    b.size.h - STATUS_BAR_LAYER_HEIGHT));
  layer_set_update_proc(s_home_layer, home_update_proc);
  layer_add_child(root, s_home_layer);
}

static void home_window_unload(Window *w) {
  layer_destroy(s_home_layer);
  s_home_layer = NULL;
  status_bar_layer_destroy(s_home_status);
}

static void home_window_appear(Window *w) {
  if (s_idle_timer) { app_timer_cancel(s_idle_timer); s_idle_timer = NULL; }
  scroll_start();
  if (s_home_layer) layer_mark_dirty(s_home_layer);
}

static void home_window_disappear(Window *w) { scroll_stop(); }

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------
static void init(void) {
  s_home_window = window_create();
  window_set_background_color(s_home_window, GColorBlack);
  window_set_click_config_provider(s_home_window, home_click_config);
  window_set_window_handlers(s_home_window, (WindowHandlers){
    .load = home_window_load,
    .unload = home_window_unload,
    .appear = home_window_appear,
    .disappear = home_window_disappear,
  });

  s_list_window = window_create();
  window_set_background_color(s_list_window, GColorBlack);
  window_set_window_handlers(s_list_window, (WindowHandlers){
    .load = list_window_load,
    .unload = list_window_unload,
    .appear = list_window_appear,
    .disappear = list_window_disappear,
  });

  s_device_window = window_create();
  window_set_background_color(s_device_window, GColorBlack);
  window_set_window_handlers(s_device_window, (WindowHandlers){
    .load = device_window_load,
    .unload = device_window_unload,
    .appear = device_window_appear,
    .disappear = device_window_disappear,
  });

  s_settings_window = window_create();
  window_set_background_color(s_settings_window, GColorBlack);
  window_set_window_handlers(s_settings_window, (WindowHandlers){
    .load = settings_window_load,
    .unload = settings_window_unload,
  });

  app_message_register_inbox_received(inbox_received);
  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(512, 64);

  window_stack_push(s_home_window, true);
}

static void deinit(void) {
  if (s_idle_timer) app_timer_cancel(s_idle_timer);
  window_destroy(s_settings_window);
  window_destroy(s_device_window);
  window_destroy(s_list_window);
  window_destroy(s_home_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
