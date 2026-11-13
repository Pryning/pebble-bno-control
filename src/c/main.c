// B&O Control - Pebble watchapp for Bang & Olufsen speakers
// Copyright (C) 2026 Tobias Pryning
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version. See the LICENSE file for details.

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
// Device the user switched to on the home page (-1 = automatic). Home
// returns to it while it's still active (playing or paused).
static int s_home_choice = -1;
// home: red "only one speaker" arrows shaking on an overlay
static Layer *s_shake_layer;
static Animation *s_shake_anim;
static bool s_shaking = false;
static int s_shake_dx = 0;

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
static Window *s_list_window;
static StatusBarLayer *s_list_status;
static MenuLayer *s_list_menu;

static Window *s_settings_window;
static MenuLayer *s_settings_menu;

// marquee
#define SCROLL_MS 60
#define SCROLL_STEP 2
#define SCROLL_PAUSE 25
static AppTimer *s_scroll_timer;
static uint32_t s_tick = 0;
static bool s_scroll_needed = false;
static bool s_scroll_done = false;   // scrolled once since the page was shown
static int s_visible_windows = 0;

#define ACCENT GColorIslamicGreen
#define GROUP_COLOR GColorVividCerulean
#define GROUP_COLOR_HL GColorBlueMoon

static void list_refresh(void);
static void user_activity(void);
static void pages_state_changed(void);
static void pages_redraw(void);
static Layer *top_page_layer(void);
static void open_device(int idx);
static bool volume_held_for(int dev);
static bool page_shows(int dev);
static void scroll_restart(void);
static void remove_device_page_if(int count);

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
  pages_redraw();
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
  bool changed = false, restart = false;
  if ((t = dict_find(it, MESSAGE_KEY_COUNT))) {
    int c = t->value->int32;
    s_count = c < 0 ? 0 : (c > MAX_DEVICES ? MAX_DEVICES : c);
    s_loaded = true;
    remove_device_page_if(s_count);
    if (s_home_choice >= s_count) s_home_choice = -1;
  }
  if ((t = dict_find(it, MESSAGE_KEY_IDX))) {
    int i = t->value->int32;
    if (i >= 0 && i < MAX_DEVICES) {
      Device *d = &s_devices[i];
      copy_str(d->name, sizeof(d->name), dict_find(it, MESSAGE_KEY_NAME));
      copy_str(d->source, sizeof(d->source), dict_find(it, MESSAGE_KEY_SOURCE));
      Tuple *tt = dict_find(it, MESSAGE_KEY_TITLE);
      if (tt && page_shows(i) && strcmp(d->title, tt->value->cstring) != 0) restart = true;
      copy_str(d->title, sizeof(d->title), tt);
      copy_str(d->artist, sizeof(d->artist), dict_find(it, MESSAGE_KEY_ARTIST));
      if ((t = dict_find(it, MESSAGE_KEY_TYPE))) d->type = t->value->int32;
      if ((t = dict_find(it, MESSAGE_KEY_ONLINE))) d->online = t->value->int32;
      if ((t = dict_find(it, MESSAGE_KEY_STATE))) d->state = t->value->int32;
      bool hold = volume_held_for(i);
      if (!hold && (t = dict_find(it, MESSAGE_KEY_VOLUME))) d->volume = t->value->int32;
      if ((t = dict_find(it, MESSAGE_KEY_VOLMAX))) d->vol_max = t->value->int32;
      if ((t = dict_find(it, MESSAGE_KEY_GROUP))) d->group = t->value->int32;
      if ((t = dict_find(it, MESSAGE_KEY_ROLE))) d->role = t->value->int32;
      changed = true;
    }
  }
  if ((t = dict_find(it, MESSAGE_KEY_SCAN))) s_scan = t->value->int32;
  if ((t = dict_find(it, MESSAGE_KEY_FOUND))) s_found = t->value->int32;
  if ((t = dict_find(it, MESSAGE_KEY_TOAST))) show_toast(t->value->cstring);
  if (changed || dict_find(it, MESSAGE_KEY_COUNT)) pages_state_changed();
  if (restart) scroll_restart();  // new track on screen: scroll it once
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
  const int gap = 36;
  int period = sz.w + gap;
  int steps = period / SCROLL_STEP;
  // Scroll through once after the page is shown, then rest (saves power).
  if (s_scroll_done || (int)s_tick >= SCROLL_PAUSE + steps) {
    graphics_draw_text(ctx, text, font, area, GTextOverflowModeTrailingEllipsis, align, NULL);
    return;
  }
  s_scroll_needed = true;
  int p = s_tick;
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
// Texts that don't fit scroll through once when a page is shown; once they
// are all back at their start the timer stops until the next page/track.
static void scroll_tick(void *ctx) {
  s_scroll_timer = NULL;
  if (s_visible_windows <= 0) return;
  s_tick++;
  bool needed = s_scroll_needed;
  s_scroll_needed = false;
  // Keep going through the initial pause even if nothing has reported yet:
  // the first redraw can be delayed (e.g. by a window transition).
  if (!needed && (int)s_tick >= SCROLL_PAUSE) {
    s_scroll_done = true;  // nothing left to scroll: stop
    return;
  }
  Window *top = window_stack_get_top_window();
  Layer *pl = top_page_layer();
  if (top == s_list_window && s_list_menu) layer_mark_dirty(menu_layer_get_layer(s_list_menu));
  else if (pl) layer_mark_dirty(pl);
  s_scroll_timer = app_timer_register(SCROLL_MS, scroll_tick, NULL);
}

static void scroll_restart(void) {
  s_tick = 0;
  s_scroll_done = false;
  s_scroll_needed = true;
  if (!s_scroll_timer && s_visible_windows > 0) {
    s_scroll_timer = app_timer_register(SCROLL_MS, scroll_tick, NULL);
  }
}

static void scroll_start(void) {
  s_visible_windows++;
  scroll_restart();
}

static void scroll_stop(void) {
  s_visible_windows--;
  if (s_visible_windows <= 0 && s_scroll_timer) {
    app_timer_cancel(s_scroll_timer);
    s_scroll_timer = NULL;
  }
}

// ---------------------------------------------------------------------------
// Settings window
// ---------------------------------------------------------------------------
// Scan modes (must match SCAN_MODES in src/pkjs/index.js)
enum { SCAN_FAST = 0, SCAN_NORMAL = 1, SCAN_THOROUGH = 2, SCAN_MODE_COUNT = 3 };
#define PERSIST_SCAN_MODE 1
static int s_scan_mode = SCAN_NORMAL;
static const char *const SCAN_MODE_NAMES[] = { "Fast", "Normal", "Thorough" };
static const char *const SCAN_MODE_DESC[] = {
  "Likely network, quick", "Detected networks", "More networks, slow",
};

enum { ROW_SCAN = 0, ROW_MODE = 1, ROW_CLEAR = 2, ROW_COUNT = 3 };

static uint16_t settings_rows(MenuLayer *m, uint16_t s, void *ctx) { return ROW_COUNT; }

static int16_t settings_header_h(MenuLayer *m, uint16_t s, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void settings_header(GContext *ctx, const Layer *cell, uint16_t s, void *data) {
  menu_cell_basic_header_draw(ctx, cell, "  SETTINGS");
}

static void settings_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  static char sub[40];
  if (idx->row == ROW_SCAN) {
    if (s_scan >= 0 && s_scan <= 100) snprintf(sub, sizeof(sub), "Scanning... %d%%", s_scan);
    else if (s_scan == SCAN_DONE) snprintf(sub, sizeof(sub), "Found %d device%s", s_found, s_found == 1 ? "" : "s");
    else snprintf(sub, sizeof(sub), "%s scan", SCAN_MODE_NAMES[s_scan_mode]);
    menu_cell_basic_draw(ctx, cell, "Scan network", sub, NULL);
  } else if (idx->row == ROW_MODE) {
    static char title[32];
    snprintf(title, sizeof(title), "Mode: %s", SCAN_MODE_NAMES[s_scan_mode]);
    menu_cell_basic_draw(ctx, cell, title, SCAN_MODE_DESC[s_scan_mode], NULL);
  } else {
    if (s_confirm_clear) snprintf(sub, sizeof(sub), "Press again to confirm");
    else snprintf(sub, sizeof(sub), "%d known device%s", s_count, s_count == 1 ? "" : "s");
    menu_cell_basic_draw(ctx, cell, "Delete all devices", sub, NULL);
  }
  // progress bar while scanning
  if (idx->row == ROW_SCAN && s_scan >= 0 && s_scan <= 100) {
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
  if (idx->row == ROW_SCAN) {
    if (s_scan >= 0 && s_scan <= 100) return;  // already scanning
    s_scan = 0;
    s_found = 0;
    send_cmd_value(CMD_SCAN, 0, s_scan_mode);
  } else if (idx->row == ROW_MODE) {
    s_scan_mode = (s_scan_mode + 1) % SCAN_MODE_COUNT;
    persist_write_int(PERSIST_SCAN_MODE, s_scan_mode);
    if (s_scan == SCAN_DONE) s_scan = SCAN_NONE;
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
// What the list menu currently shows. rebuild_order() is also used by the
// home page, so the menu keeps its own copy, refreshed only on reload.
static uint8_t s_menu_order[MAX_DEVICES];
static int s_menu_active = 0, s_menu_other = 0;
static int s_list_sel_dev = -1;   // device the list cursor is on
static bool s_list_touched = false;  // user moved the selection

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
  if (idx.section == SEC_ACTIVE && idx.row < s_menu_active) return s_menu_order[idx.row];
  if (idx.section == SEC_DEVICES && idx.row < s_menu_other) return s_menu_order[s_menu_active + idx.row];
  return -1;
}

static MenuIndex index_of_device(int dev) {
  for (int r = 0; r < s_count; r++) {
    if (s_menu_order[r] == dev) {
      return r < s_menu_active ? (MenuIndex){ SEC_ACTIVE, r } : (MenuIndex){ SEC_DEVICES, r - s_menu_active };
    }
  }
  return (MenuIndex){ SEC_DEVICES, 0 };
}

static void list_refresh(void) {
  rebuild_order();
  memcpy(s_menu_order, s_order, sizeof(s_menu_order));
  s_menu_active = s_active_n;
  s_menu_other = s_other_n;
  if (!s_list_menu) return;
  int dev = (s_list_sel_dev >= 0 && s_list_sel_dev < s_count) ? s_list_sel_dev : -1;
  menu_layer_reload_data(s_list_menu);
  // keep the same device selected when rows move around
  // (until the user scrolls, stay on the top row)
  if (s_list_touched && dev >= 0) {
    // follow the device; if its row moved (e.g. it started playing and went
    // up to ACTIVE), scroll the list so the cursor is visible again
    MenuIndex want = index_of_device(dev);
    MenuIndex cur = menu_layer_get_selected_index(s_list_menu);
    bool moved = want.section != cur.section || want.row != cur.row;
    menu_layer_set_selected_index(s_list_menu, want, moved ? MenuRowAlignCenter : MenuRowAlignNone, false);
  } else if (!s_list_touched) {
    menu_layer_set_selected_index(s_list_menu, (MenuIndex){ s_menu_active ? SEC_ACTIVE : SEC_DEVICES, 0 },
                                  MenuRowAlignNone, false);
  }
}

// Only called when the user moves the cursor with the buttons (never for
// menu_layer_set_selected_index or the menu adjusting itself after a
// reload), so it reliably tracks which speaker the user is on.
static void list_selection_will_change(MenuLayer *m, MenuIndex *new_idx, MenuIndex old_idx, void *ctx) {
  s_list_touched = true;
  s_list_sel_dev = device_at(*new_idx);
  user_activity();
}

static uint16_t list_sections(MenuLayer *m, void *ctx) { return 3; }

static uint16_t list_rows(MenuLayer *m, uint16_t s, void *ctx) {
  if (s == SEC_ACTIVE) return s_menu_active;
  if (s == SEC_DEVICES) return s_count == 0 ? 1 : s_menu_other;
  return 1;
}

static int16_t list_cell_h(MenuLayer *m, MenuIndex *idx, void *ctx) {
  return idx->section == SEC_SETTINGS ? 40 : ROW_H;
}

static int16_t list_header_h(MenuLayer *m, uint16_t s, void *ctx) {
  if (s == SEC_ACTIVE && s_menu_active == 0) return 0;
  if (s == SEC_DEVICES && s_count > 0 && s_menu_other == 0) return 0;
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
    if (idx->row > 0) above = s_devices[s_menu_order[idx->row - 1]].group == d->group;
    if (idx->row + 1 < s_menu_active) below = s_devices[s_menu_order[idx->row + 1]].group == d->group;
  }
  draw_device_row(ctx, cell, d, hl, above, below);
}

static void list_select(MenuLayer *m, MenuIndex *idx, void *ctx) {
  user_activity();
  int dev = device_at(*idx);
  if (dev >= 0) { s_list_sel_dev = dev; s_list_touched = true; }
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
    .selection_will_change = list_selection_will_change,
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
// Device control page
//
// Works like the Pebble music app (system ActionBarLayer, music app icons,
// icon press animation, animated icon swaps). Select cycles button sets:
//   MODE_VOLUME : Up = volume up,  Down = volume down   (default)
//   MODE_PLAY   : Up = play/pause, Down = join / unlink (hold Up: power off)
//   MODE_TRACK  : Up = previous,   Down = next
// When nothing is playing there is one set: Up = play, Down = join.
//
// The same page is used twice: as the home page (showing the playing
// device, or the multiroom leader) and for a device opened from the list.
// ---------------------------------------------------------------------------
enum { MODE_VOLUME = 0, MODE_PLAY = 1, MODE_TRACK = 2, MODE_COUNT = 3 };

#define ANIM_FRAME_MS (1000 / 28)
#define BOUNCE_OFFSET 6
#define HOME_TIMEOUT_MS 20000

typedef struct {
  Window *window;
  Layer *layer;
  ActionBarLayer *bar;
  GBitmap *bar_icons[3];   // currently shown (up, select, down)
  bool home;               // home page: device chosen automatically
  int cur;                 // device index, -1 = none (home: nothing playing)
  int mode;
  bool was_playing;
  bool hint;               // home: show "hold for devices" hint
  AppTimer *hint_timer;
  Animation *track_anim;   // music-app style text animations
  int track_dy;
  Animation *state_anim;
  int state_dy;
  AppTimer *off_timer;     // "powered off" feedback
  int off_count;
  AppTimer *vol_timer;
  int click_key;           // button layout the click config was built for
  bool switching;          // home: holding +/- switches speaker (arrow icons)
  AppTimer *switch_timer;
} Page;

static Page s_home = { .home = true, .cur = -1 };
static Page s_dev = { .cur = -1 };

static GBitmap *s_icon_ellipsis, *s_icon_play, *s_icon_pause, *s_icon_skip_back,
               *s_icon_skip_fwd, *s_icon_vol_up, *s_icon_vol_down, *s_icon_join, *s_icon_unlink,
               *s_icon_arrow_up, *s_icon_arrow_down;

static int s_vol_dev = -1;   // device whose volume the user is changing
static time_t s_vol_hold_s;
static uint16_t s_vol_hold_ms;
static AppTimer *s_idle_timer;

static Device *page_device(Page *pg) {
  return (pg->cur >= 0 && pg->cur < s_count) ? &s_devices[pg->cur] : NULL;
}

static bool is_playing(const Device *d) {
  return d && d->online && d->state == ST_PLAYING;
}

// Button set actually in effect (one fixed set when nothing is playing).
static int effective_mode(Page *pg) {
  return is_playing(page_device(pg)) ? pg->mode : MODE_PLAY;
}

static bool volume_held(void) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  int64_t now = (int64_t)s * 1000 + ms;
  int64_t until = (int64_t)s_vol_hold_s * 1000 + s_vol_hold_ms;
  return now < until;
}

static void hold_volume(int dev, int ms_ahead) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  int64_t t = (int64_t)s * 1000 + ms + ms_ahead;
  s_vol_dev = dev;
  s_vol_hold_s = t / 1000;
  s_vol_hold_ms = t % 1000;
}

static bool volume_held_for(int dev) { return dev == s_vol_dev && volume_held(); }

// --- home page device choice ------------------------------------------------
// The multiroom leader if speakers play together, otherwise the first playing
// device in list order (the list puts groups first, leader on top).
static int pick_home_device(void) {
  rebuild_order();
  for (int r = 0; r < s_count; r++) {
    const Device *d = &s_devices[s_order[r]];
    if (d->online && d->state == ST_PLAYING) return s_order[r];
  }
  return -1;
}

static void page_reset(Page *pg, int cur) {
  pg->cur = cur;
  pg->mode = MODE_VOLUME;
  pg->was_playing = is_playing(page_device(pg));
}

// Re-pick the home device. Stays on the current one while it's still active
// (so pausing from home doesn't jump away) unless `fresh`.
// Leader of the multiroom group `d` follows, if it's known and active; else -1.
static int group_leader_of(const Device *d) {
  if (!d || !d->group || d->role != ROLE_FOLLOWER) return -1;
  for (int j = 0; j < s_count; j++) {
    const Device *o = &s_devices[j];
    if (o->group == d->group && o->role == ROLE_LEADER && is_active(o)) return j;
  }
  return -1;
}

static void home_pick(bool fresh) {
  Device *d = page_device(&s_home);
  // showing a follower whose group leader is known: show the leader instead
  // (unless the user picked this speaker on purpose)
  int leader = group_leader_of(d);
  if (leader >= 0 && s_home.cur != s_home_choice && s_home.off_count == 0) {
    page_reset(&s_home, leader);
    return;
  }
  if (!fresh && (s_home.off_count > 0 || (d && is_active(d)))) return;
  int idx = -1;
  if (s_home_choice >= 0 && s_home_choice < s_count && is_active(&s_devices[s_home_choice])) {
    idx = s_home_choice;
  } else {
    s_home_choice = -1;
    idx = pick_home_device();
  }
  if (idx != s_home.cur || fresh) page_reset(&s_home, idx);
}

// Things that can be shown on the home page, in list order: every playing
// speaker except multiroom followers (a group is shown by its leader), plus
// the one currently shown. Returns the count.
static int home_units(int *out) {
  rebuild_order();
  int n = 0;
  for (int r = 0; r < s_count; r++) {
    int i = s_order[r];
    const Device *d = &s_devices[i];
    bool unit = d->online && d->state == ST_PLAYING && !(d->group && d->role == ROLE_FOLLOWER);
    if (unit || i == s_home.cur) out[n++] = i;
  }
  return n;
}

// --- animations --------------------------------------------------------------
// One animation = "slide up and out" (ease-in) followed by "appear slightly
// above and settle" (ease-out), like the music app's track change.
static int bounce_offset(AnimationProgress progress, int travel) {
  int32_t half = ANIMATION_NORMALIZED_MAX / 2;
  if (progress < half) {
    int32_t t = progress * 1000 / half;
    return -(int)(travel * t * t / 1000000);
  }
  int32_t t = (progress - half) * 1000 / half;
  int32_t inv = 1000 - t;
  return -(int)(BOUNCE_OFFSET * inv * inv / 1000000);
}

static void track_anim_update(Animation *a, const AnimationProgress p) {
  Page *pg = animation_get_context(a);
  pg->track_dy = bounce_offset(p, 40);
  if (pg->layer) layer_mark_dirty(pg->layer);
}

static void state_anim_update(Animation *a, const AnimationProgress p) {
  Page *pg = animation_get_context(a);
  pg->state_dy = bounce_offset(p, 22);
  if (pg->layer) layer_mark_dirty(pg->layer);
}

static void track_anim_teardown(Animation *a) {
  Page *pg = animation_get_context(a);
  pg->track_anim = NULL;
  pg->track_dy = 0;
}

static void state_anim_teardown(Animation *a) {
  Page *pg = animation_get_context(a);
  pg->state_anim = NULL;
  pg->state_dy = 0;
}

static const AnimationImplementation s_track_impl = {
  .update = track_anim_update, .teardown = track_anim_teardown,
};
static const AnimationImplementation s_state_impl = {
  .update = state_anim_update, .teardown = state_anim_teardown,
};

static void run_anim(Page *pg, Animation **slot, const AnimationImplementation *impl) {
  if (*slot) animation_unschedule(*slot);
  Animation *a = animation_create();
  animation_set_duration(a, 8 * ANIM_FRAME_MS);
  animation_set_curve(a, AnimationCurveLinear);
  animation_set_implementation(a, impl);
  animation_set_handlers(a, (AnimationHandlers){ 0 }, pg);
  *slot = a;
  animation_schedule(a);
}

// --- action bar ----------------------------------------------------------------
static void page_click_config(void *ctx);

static void set_bar_icon(Page *pg, ButtonId id, GBitmap *icon, bool animated) {
  int slot = id == BUTTON_ID_UP ? 0 : (id == BUTTON_ID_SELECT ? 1 : 2);
  if (pg->bar_icons[slot] == icon) return;
  pg->bar_icons[slot] = icon;
  if (icon) action_bar_layer_set_icon_animated(pg->bar, id, icon, animated);
  else action_bar_layer_clear_icon(pg->bar, id);
}

static void update_action_bar(Page *pg, bool animated) {
  if (!pg->bar) return;
  Device *d = page_device(pg);
  // home page with nothing playing: no controls, any button opens the list
  layer_set_hidden(action_bar_layer_get_layer(pg->bar), d == NULL);
  if (d) {
    bool playing = is_playing(d);
    switch (effective_mode(pg)) {
      case MODE_VOLUME:
        if (pg->home && s_shaking) {  // red shaking arrows are drawn on an overlay
          set_bar_icon(pg, BUTTON_ID_UP, NULL, false);
          set_bar_icon(pg, BUTTON_ID_DOWN, NULL, false);
        } else if (pg->switching) {  // a held +/- is switching speakers
          set_bar_icon(pg, BUTTON_ID_UP, s_icon_arrow_up, animated);
          set_bar_icon(pg, BUTTON_ID_DOWN, s_icon_arrow_down, animated);
        } else {
          set_bar_icon(pg, BUTTON_ID_UP, s_icon_vol_up, animated);
          set_bar_icon(pg, BUTTON_ID_DOWN, s_icon_vol_down, animated);
        }
        break;
      case MODE_PLAY:
        set_bar_icon(pg, BUTTON_ID_UP, playing ? s_icon_pause : s_icon_play, animated);
        // already in a multiroom group: offer to unlink instead of join
        set_bar_icon(pg, BUTTON_ID_DOWN, d->group ? s_icon_unlink : s_icon_join, animated);
        break;
      case MODE_TRACK:
        set_bar_icon(pg, BUTTON_ID_UP, s_icon_skip_back, animated);
        set_bar_icon(pg, BUTTON_ID_DOWN, s_icon_skip_fwd, animated);
        break;
    }
    // ••• is always shown: hold it for the device list (press: cycle button sets)
    set_bar_icon(pg, BUTTON_ID_SELECT, s_icon_ellipsis, animated);
  }
  // Re-subscribe only when the button layout changes (volume buttons repeat
  // while held, play/pause has a long press...); re-subscribing on every
  // update would interrupt a held button.
  int key = d ? (1 + effective_mode(pg) * 2 + (is_playing(d) ? 8 : 0) +
                 (pg->switching ? 32 : 0)) : 0;
  if (key != pg->click_key || !animated) {
    pg->click_key = key;
    action_bar_layer_set_click_config_provider(pg->bar, page_click_config);
  }
}

// Called whenever device data changes.
static void page_state_changed(Page *pg) {
  if (pg->home) {
    int before = pg->cur;
    home_pick(false);
    if (pg->cur != before) {
      update_action_bar(pg, true);
      scroll_restart();  // a different device is shown: scroll its title once
      return;
    }
  }
  bool playing = is_playing(page_device(pg));
  if (playing != pg->was_playing) {
    // Starting playback from the single "nothing playing" set keeps the same
    // layout (play/pause + join) so the buttons don't jump around.
    if (playing) pg->mode = MODE_PLAY;
    pg->was_playing = playing;
  }
  update_action_bar(pg, true);
}

static void pages_state_changed(void) {
  if (s_home.window && window_stack_contains_window(s_home.window)) page_state_changed(&s_home);
  if (s_dev.window && window_stack_contains_window(s_dev.window)) page_state_changed(&s_dev);
}

// --- drawing -------------------------------------------------------------------
static void draw_nothing_playing(Page *pg, GContext *ctx, GRect b) {
  const int x = 10, cw = b.size.w - 2 * x;
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
  graphics_draw_text(ctx, t1, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), GRect(x, cy + 4, cw, 30),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, t2, fonts_get_system_font(FONT_KEY_GOTHIC_18), GRect(x, cy + 34, cw, 44),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  if (s_count > 0) {
    graphics_context_set_text_color(ctx, GColorDarkGray);
    graphics_draw_text(ctx, "Press any button for devices", fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(x, b.size.h - 24, cw, 18), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
  }
}

static void page_update_proc(Layer *layer, GContext *ctx) {
  Page *pg = *(Page **)layer_get_data(layer);
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  Device *d = page_device(pg);
  if (!d) {
    if (pg->home) draw_nothing_playing(pg, ctx, b);
    return;
  }
  b.size.w -= ACTION_BAR_WIDTH;

  const int x = 8;
  const int cw = b.size.w - x - 6;
  GFont f14 = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  GFont f18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont f18b = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont f24 = fonts_get_system_font(FONT_KEY_GOTHIC_24);
  GFont f24b = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GFont f28b = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  int mode = effective_mode(pg);

  // Now-playing block (y 104..170): scrolls horizontally, animates vertically.
  const int top = 104, bottom = 170;
  const char *title = d->title[0] ? d->title : d->source;
  int ty = pg->track_dy;
  if (pg->off_count > 0) {
    static char obuf[32];
    if (pg->off_count > 1) snprintf(obuf, sizeof(obuf), "Group of %d", pg->off_count);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, pg->off_count > 1 ? "Group off" : "Speaker off", f28b,
                       GRect(x, top + 2 + ty, cw, 34), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft, NULL);
    if (pg->off_count > 1) {
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
  static char sub[64];
  const char *plat = d->type == TYPE_MOZART ? "Mozart" : "ASE";
  const char *leader_name = NULL;
  int members = 0;
  for (int j = 0; j < s_count; j++) {
    if (!d->group || s_devices[j].group != d->group) continue;
    members++;
    if (s_devices[j].role == ROLE_LEADER) leader_name = s_devices[j].name;
  }
  if (d->group && d->role == ROLE_LEADER) snprintf(sub, sizeof(sub), "Multiroom leader · %d speakers", members);
  else if (d->group && leader_name) snprintf(sub, sizeof(sub), "%s  ·  With %s", plat, leader_name);
  else if (d->group) snprintf(sub, sizeof(sub), "%s  ·  Multiroom", plat);
  else snprintf(sub, sizeof(sub), "%s", d->type == TYPE_MOZART ? "Mozart" : "ASE");
  graphics_context_set_text_color(ctx, d->group ? GROUP_COLOR : GColorLightGray);
  graphics_draw_text(ctx, sub, f14,
                     GRect(x, 32, cw, 18), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_draw_line(ctx, GPoint(x, 54), GPoint(x + cw, 54));

  // State (bounces on play/pause), clipped to its own row
  const char *label;
  GColor scol;
  bool off = pg->off_count > 0;
  if (off) { label = "POWERED OFF"; scol = GColorRed; }
  else if (!d->online) { label = "OFFLINE"; scol = GColorDarkGray; }
  else if (d->state == ST_PLAYING) { label = "PLAYING"; scol = GColorGreen; }
  else if (d->state == ST_PAUSED) { label = "PAUSED"; scol = GColorChromeYellow; }
  else { label = "IDLE"; scol = GColorLightGray; }
  int sy = pg->state_dy;
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

  // Bottom line: toast, home hint, or other speakers playing
  static char foot[48];
  foot[0] = '\0';
  GColor fcol = GColorWhite;
  if (s_toast[0]) {
    snprintf(foot, sizeof(foot), "%s", s_toast);
  } else if (pg->home && pg->hint) {
    snprintf(foot, sizeof(foot), "Hold ••• for all devices");
    fcol = GColorLightGray;
  } else if (pg->home) {
    int others = 0;
    for (int i = 0; i < s_count; i++) {
      const Device *o = &s_devices[i];
      if (i != pg->cur && is_playing(o) && !(d->group && o->group == d->group)) others++;
    }
    if (others) snprintf(foot, sizeof(foot), "+%d more \u00b7 hold \u00b1 to switch", others);
    fcol = GColorLightGray;
  }
  if (foot[0]) {
    graphics_context_set_text_color(ctx, fcol);
    graphics_draw_text(ctx, foot, f14, GRect(x, b.size.h - 26, cw, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }

  // hairline separating the content from the (black) action bar
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_draw_line(ctx, GPoint(b.size.w - 1, 0), GPoint(b.size.w - 1, b.size.h));
}

// --- buttons -------------------------------------------------------------------
static void vol_send(void *ctx) {
  Page *pg = ctx;
  pg->vol_timer = NULL;
  Device *d = page_device(pg);
  if (d) send_cmd_value(CMD_VOLUME, pg->cur, d->volume);
}

static void change_volume(Page *pg, int dir) {
  Device *d = page_device(pg);
  if (!d || d->vol_max == 0) return;
  int step = d->vol_max >= 60 ? 2 : 1;
  int v = d->volume + dir * step;
  if (v < 0) v = 0;
  if (v > d->vol_max) v = d->vol_max;
  d->volume = v;
  hold_volume(pg->cur, 3000);
  if (pg->vol_timer) app_timer_reschedule(pg->vol_timer, 200);
  else pg->vol_timer = app_timer_register(200, vol_send, pg);
  layer_mark_dirty(pg->layer);
}

static bool check_online(Page *pg) {
  Device *d = page_device(pg);
  if (!d) return false;
  if (!d->online) {
    show_toast("Device not reachable");
    refresh_all();
    return false;
  }
  return true;
}

static void do_play_pause(Page *pg) {
  Device *d = page_device(pg);
  bool was_playing = is_playing(d);
  d->state = was_playing ? ST_PAUSED : ST_PLAYING;  // optimistic
  send_cmd(CMD_TOGGLE, pg->cur);
  run_anim(pg, &pg->state_anim, &s_state_impl);
  page_state_changed(pg);
  refresh_all();
}

static void do_join(Page *pg) {
  send_cmd(CMD_JOIN, pg->cur);
  show_toast("Joining...");
  vibes_short_pulse();
  refresh_all();
}

static void do_unlink(Page *pg) {
  bool leader = page_device(pg)->role == ROLE_LEADER;
  send_cmd(CMD_UNLINK, pg->cur);
  show_toast(leader ? "Releasing group..." : "Leaving group...");
  vibes_short_pulse();
  refresh_all();
}

static void do_skip(Page *pg, bool forward) {
  send_cmd(forward ? CMD_NEXT : CMD_PREV, pg->cur);
  run_anim(pg, &pg->track_anim, &s_track_impl);
}

static void open_list(void);

static void page_up(ClickRecognizerRef r, void *ctx) {
  Page *pg = ctx;
  user_activity();
  if (!page_device(pg)) { open_list(); return; }
  if (!check_online(pg)) return;
  switch (effective_mode(pg)) {
    case MODE_VOLUME: change_volume(pg, 1); break;
    case MODE_PLAY: do_play_pause(pg); break;
    case MODE_TRACK: do_skip(pg, false); break;
  }
}

static void page_down(ClickRecognizerRef r, void *ctx) {
  Page *pg = ctx;
  user_activity();
  if (!page_device(pg)) { open_list(); return; }
  if (!check_online(pg)) return;
  switch (effective_mode(pg)) {
    case MODE_VOLUME: change_volume(pg, -1); break;
    case MODE_PLAY: if (page_device(pg)->group) do_unlink(pg); else do_join(pg); break;
    case MODE_TRACK: do_skip(pg, true); break;
  }
}

static void off_done(void *ctx) {
  Page *pg = ctx;
  pg->off_timer = NULL;
  pg->off_count = 0;
  page_state_changed(pg);
  if (pg->layer) layer_mark_dirty(pg->layer);
}

// Long press on pause: standby (the whole group when this is the leader).
static void page_up_long(ClickRecognizerRef r, void *ctx) {
  Page *pg = ctx;
  user_activity();
  Device *d = page_device(pg);
  if (!is_playing(d) || effective_mode(pg) != MODE_PLAY) return;
  int n = 1;
  if (d->group && d->role == ROLE_LEADER) {
    n = 0;
    for (int j = 0; j < s_count; j++) {
      if (s_devices[j].group == d->group) { s_devices[j].state = ST_IDLE; n++; }
    }
  }
  d->state = ST_IDLE;
  send_cmd(CMD_POWER_OFF, pg->cur);
  static const uint32_t pattern[] = { 60, 80, 200 };
  vibes_enqueue_custom_pattern((VibePattern){ .durations = pattern, .num_segments = 3 });
  pg->off_count = n;
  if (pg->off_timer) app_timer_reschedule(pg->off_timer, 3500);
  else pg->off_timer = app_timer_register(3500, off_done, pg);
  run_anim(pg, &pg->state_anim, &s_state_impl);
  run_anim(pg, &pg->track_anim, &s_track_impl);
  page_state_changed(pg);
  refresh_all();
}

static void page_select(ClickRecognizerRef r, void *ctx) {
  Page *pg = ctx;
  user_activity();
  if (!page_device(pg)) { open_list(); return; }
  if (!is_playing(page_device(pg))) return;  // only one button set
  pg->mode = (pg->mode + 1) % MODE_COUNT;
  update_action_bar(pg, true);
  layer_mark_dirty(pg->layer);
}

// Hold Select (•••) on any page: go to the device list.
static void page_select_long(ClickRecognizerRef r, void *ctx) {
  Page *pg = ctx;
  user_activity();
  if (pg->home) {
    open_list();
  } else if (window_stack_contains_window(s_list_window)) {
    window_stack_remove(pg->window, true);  // the list is right below
  } else {
    open_list();
  }
}

// Home page: hold volume +/- to switch between the speakers that are playing.
// The page scrolls like a list: the current speaker slides out and the
// next one slides in (down = next = content moves up).
static Animation *s_slide_anim;
static int s_slide_dir = 0;       // +1 next, -1 previous
static int s_slide_target = -1;   // device switched to at the midpoint
static int s_slide_pos = 0, s_slide_n = 0;
static bool s_slide_switched = false;

static void set_page_offset(Page *pg, int dy) {
  if (!pg->layer) return;
  GRect b = layer_get_bounds(pg->layer);
  b.origin.y = dy;
  layer_set_bounds(pg->layer, b);
}

// Show the target speaker (at the animation's midpoint).
static void slide_apply(void) {
  if (s_slide_switched) return;
  s_slide_switched = true;
  int mode = s_home.mode;
  page_reset(&s_home, s_slide_target);
  s_home.mode = mode;
  s_home_choice = s_slide_target;  // home returns here after the timeout
  static char msg[24];
  snprintf(msg, sizeof(msg), "Speaker %d of %d", s_slide_pos + 1, s_slide_n);
  show_toast(msg);
  update_action_bar(&s_home, true);
  scroll_restart();
  refresh_all();
}

static void slide_update(Animation *a, const AnimationProgress p) {
  if (!s_home.layer) return;
  int h = layer_get_frame(s_home.layer).size.h;
  int32_t half = ANIMATION_NORMALIZED_MAX / 2;
  int dy;
  if (p < half) {             // old speaker slides out (ease in)
    int32_t t = p * 1000 / half;
    dy = -s_slide_dir * (int)(h * t * t / 1000000);
  } else {                    // new speaker slides in (ease out)
    slide_apply();
    int32_t t = (p - half) * 1000 / half, inv = 1000 - t;
    dy = s_slide_dir * (int)(h * inv * inv / 1000000);
  }
  set_page_offset(&s_home, dy);
}

static void slide_teardown(Animation *a) {
  slide_apply();
  set_page_offset(&s_home, 0);
  s_slide_anim = NULL;
}

static const AnimationImplementation s_slide_impl = {
  .update = slide_update, .teardown = slide_teardown,
};

static void home_switch(int dir) {
  user_activity();
  if (s_slide_anim) animation_unschedule(s_slide_anim);  // finish a running one
  int units[MAX_DEVICES];
  int n = home_units(units);
  if (n < 2) return;
  int pos = 0;
  for (int k = 0; k < n; k++) if (units[k] == s_home.cur) pos = k;
  pos = (pos + dir + n) % n;
  s_slide_dir = dir;
  s_slide_target = units[pos];
  s_slide_pos = pos;
  s_slide_n = n;
  s_slide_switched = false;
  vibes_short_pulse();
  Animation *anim = animation_create();
  animation_set_duration(anim, 10 * ANIM_FRAME_MS);
  animation_set_curve(anim, AnimationCurveLinear);
  animation_set_implementation(anim, &s_slide_impl);
  s_slide_anim = anim;
  animation_schedule(anim);
}

// Arrow mode: after a hold has switched speaker, Up/Down show arrows for a
// few seconds and a short press also switches. Each switch restarts the
// countdown; after it, the buttons are volume +/- again.
#define SWITCH_MODE_MS 3000

static void switch_icons_done(void *ctx) {
  s_home.switch_timer = NULL;
  s_home.switching = false;
  update_action_bar(&s_home, true);
}

// Only one speaker playing: red arrows shake with a matching buzz ("can't
// switch"), then volume +/- returns.
// The action bar can't move or tint its icons, so during the shake its
// up/down icons are hidden and red ones are drawn on an overlay instead.
#define SHAKE_MS 420

static void draw_triangle(GContext *ctx, GPoint c, bool up) {
  int hw = 9, hh = 6;  // 19x13: bigger than the icons so the red reads on the real screen
  GPathInfo info = {
    .num_points = 3,
    .points = up ? (GPoint[]){ {0, -hh}, {hw, hh}, {-hw, hh} }
                 : (GPoint[]){ {0, hh}, {hw, -hh}, {-hw, -hh} },
  };
  GPath *p = gpath_create(&info);
  gpath_move_to(p, c);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

static void shake_update_proc(Layer *layer, GContext *ctx) {
  if (!s_shaking) return;
  GRect b = layer_get_bounds(layer);
  int cx = b.size.w / 2 + s_shake_dx;
  graphics_context_set_fill_color(ctx, GColorRed);
  draw_triangle(ctx, GPoint(cx, 54), true);
  draw_triangle(ctx, GPoint(cx, b.size.h - 54), false);
}

static void shake_update(Animation *a, const AnimationProgress p) {
  // three decaying wiggles
  int32_t angle = (int32_t)p * 3 * TRIG_MAX_ANGLE / ANIMATION_NORMALIZED_MAX;
  int32_t amp = 4 * (ANIMATION_NORMALIZED_MAX - p) / ANIMATION_NORMALIZED_MAX + 1;
  s_shake_dx = (int)(amp * sin_lookup(angle) / TRIG_MAX_RATIO);
  if (s_shake_layer) layer_mark_dirty(s_shake_layer);
}

static void shake_teardown(Animation *a) {
  s_shake_anim = NULL;
  s_shaking = false;
  s_shake_dx = 0;
  if (s_shake_layer) layer_mark_dirty(s_shake_layer);
  s_home.switching = false;
  update_action_bar(&s_home, true);  // back to volume +/-
}

static const AnimationImplementation s_shake_impl = {
  .update = shake_update, .teardown = shake_teardown,
};

static void shake_start(void *ctx) {
  if (!s_home.layer) return;
  s_shaking = true;
  update_action_bar(&s_home, false);  // hides the white arrows
  static const uint32_t pattern[] = { 45, 90, 45, 90, 45 };  // buzz with each wiggle
  vibes_enqueue_custom_pattern((VibePattern){ .durations = pattern, .num_segments = 5 });
  Animation *anim = animation_create();
  animation_set_duration(anim, SHAKE_MS);
  animation_set_curve(anim, AnimationCurveLinear);
  animation_set_implementation(anim, &s_shake_impl);
  s_shake_anim = anim;
  animation_schedule(anim);
}

static void home_switch_start(int dir) {
  int units[MAX_DEVICES];
  if (home_units(units) < 2) {
    user_activity();
    if (s_shaking) return;  // already showing it
    if (s_home.switch_timer) { app_timer_cancel(s_home.switch_timer); s_home.switch_timer = NULL; }
    shake_start(NULL);      // red arrows shake right away
    return;
  }
  if (s_home.switch_timer) app_timer_reschedule(s_home.switch_timer, SWITCH_MODE_MS);
  else s_home.switch_timer = app_timer_register(SWITCH_MODE_MS, switch_icons_done, NULL);
  if (!s_home.switching) {
    s_home.switching = true;
    update_action_bar(&s_home, true);  // arrows (and arrow-mode buttons) right away
  }
  home_switch(dir);
}

static void home_switch_prev(ClickRecognizerRef r, void *ctx) { home_switch_start(-1); }
static void home_switch_next(ClickRecognizerRef r, void *ctx) { home_switch_start(1); }

static void page_click_config(void *ctx) {
  Page *pg = ctx;
  if (!page_device(pg)) {
    // home with nothing playing: any button opens the device list
    window_single_click_subscribe(BUTTON_ID_UP, page_up);
    window_single_click_subscribe(BUTTON_ID_SELECT, page_select);
    window_single_click_subscribe(BUTTON_ID_DOWN, page_down);
    return;
  }
  window_single_click_subscribe(BUTTON_ID_SELECT, page_select);
  window_long_click_subscribe(BUTTON_ID_SELECT, 600, page_select_long, NULL);
  if (effective_mode(pg) == MODE_VOLUME && pg->home && pg->switching) {
    // arrow mode: press or hold Up/Down to switch speaker
    window_single_click_subscribe(BUTTON_ID_UP, home_switch_prev);
    window_single_click_subscribe(BUTTON_ID_DOWN, home_switch_next);
  } else if (effective_mode(pg) == MODE_VOLUME && pg->home) {
    // home: press = volume, hold = switch speaker
    window_single_click_subscribe(BUTTON_ID_UP, page_up);
    window_single_click_subscribe(BUTTON_ID_DOWN, page_down);
    window_long_click_subscribe(BUTTON_ID_UP, 600, home_switch_prev, NULL);
    window_long_click_subscribe(BUTTON_ID_DOWN, 600, home_switch_next, NULL);
  } else if (effective_mode(pg) == MODE_VOLUME) {
    // hold to keep changing the volume
    window_single_repeating_click_subscribe(BUTTON_ID_UP, 150, page_up);
    window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, page_down);
  } else {
    window_single_click_subscribe(BUTTON_ID_UP, page_up);
    window_single_click_subscribe(BUTTON_ID_DOWN, page_down);
    if (effective_mode(pg) == MODE_PLAY && is_playing(page_device(pg))) {
      // hold pause to power off (single click still pauses)
      window_long_click_subscribe(BUTTON_ID_UP, 700, page_up_long, NULL);
    }
  }
}

// --- window lifecycle ----------------------------------------------------------
static Page *window_page(Window *w) { return window_get_user_data(w); }

static void page_window_load(Window *w) {
  Page *pg = window_page(w);
  Layer *root = window_get_root_layer(w);
  pg->layer = layer_create_with_data(layer_get_bounds(root), sizeof(Page *));
  *(Page **)layer_get_data(pg->layer) = pg;
  layer_set_update_proc(pg->layer, page_update_proc);
  layer_add_child(root, pg->layer);

  pg->bar = action_bar_layer_create();
  action_bar_layer_set_background_color(pg->bar, GColorBlack);
  action_bar_layer_set_context(pg->bar, pg);
  action_bar_layer_add_to_window(pg->bar, w);
  pg->bar_icons[0] = pg->bar_icons[1] = pg->bar_icons[2] = NULL;
  if (pg->home) {  // overlay on top of the action bar for the red shake
    GRect rb = layer_get_bounds(root);
    s_shake_layer = layer_create(GRect(rb.size.w - ACTION_BAR_WIDTH, 0, ACTION_BAR_WIDTH, rb.size.h));
    layer_set_update_proc(s_shake_layer, shake_update_proc);
    layer_add_child(root, s_shake_layer);
  }
  update_action_bar(pg, false);
}

static void page_window_unload(Window *w) {
  Page *pg = window_page(w);
  if (pg->off_timer) { app_timer_cancel(pg->off_timer); pg->off_timer = NULL; }
  if (pg->hint_timer) { app_timer_cancel(pg->hint_timer); pg->hint_timer = NULL; }
  if (pg->switch_timer) { app_timer_cancel(pg->switch_timer); pg->switch_timer = NULL; }
  pg->switching = false;
  pg->off_count = 0;
  if (pg->home && s_slide_anim) animation_unschedule(s_slide_anim);
  if (pg->home && s_shake_anim) animation_unschedule(s_shake_anim);
  if (pg->home && s_shake_layer) { layer_destroy(s_shake_layer); s_shake_layer = NULL; }
  if (pg->track_anim) animation_unschedule(pg->track_anim);
  if (pg->state_anim) animation_unschedule(pg->state_anim);
  action_bar_layer_destroy(pg->bar);
  pg->bar = NULL;
  layer_destroy(pg->layer);
  pg->layer = NULL;
}

static void hint_done(void *ctx) {
  Page *pg = ctx;
  pg->hint_timer = NULL;
  pg->hint = false;
  if (pg->layer) layer_mark_dirty(pg->layer);
}

static void page_window_appear(Window *w) {
  Page *pg = window_page(w);
  if (pg->home) {
    if (s_idle_timer) { app_timer_cancel(s_idle_timer); s_idle_timer = NULL; }
    home_pick(true);  // fresh choice every time home is shown
    pg->hint = true;
    if (pg->hint_timer) app_timer_reschedule(pg->hint_timer, 4000);
    else pg->hint_timer = app_timer_register(4000, hint_done, pg);
    update_action_bar(pg, false);
  }
  scroll_start();
  if (!pg->home) send_cmd(CMD_FOCUS, pg->cur);
  layer_mark_dirty(pg->layer);
}

static void page_window_disappear(Window *w) {
  Page *pg = window_page(w);
  scroll_stop();
  if (pg->vol_timer) { app_timer_cancel(pg->vol_timer); vol_send(pg); }
  if (!pg->home) send_cmd(CMD_FOCUS, -1);
}

static void page_window_init(Page *pg) {
  pg->window = window_create();
  window_set_background_color(pg->window, GColorBlack);
  window_set_user_data(pg->window, pg);
  window_set_window_handlers(pg->window, (WindowHandlers){
    .load = page_window_load,
    .unload = page_window_unload,
    .appear = page_window_appear,
    .disappear = page_window_disappear,
  });
}

static void open_device(int idx) {
  s_toast[0] = '\0';
  page_reset(&s_dev, idx);
  window_stack_push(s_dev.window, true);
}

// --- home timeout ----------------------------------------------------------------
static void go_home(void *ctx) {
  s_idle_timer = NULL;
  if (s_scan >= 0 && s_scan <= 100) {  // don't interrupt a running scan
    s_idle_timer = app_timer_register(HOME_TIMEOUT_MS, go_home, NULL);
    return;
  }
  if (window_stack_contains_window(s_settings_window)) window_stack_remove(s_settings_window, false);
  if (window_stack_contains_window(s_dev.window)) window_stack_remove(s_dev.window, false);
  if (window_stack_contains_window(s_list_window)) window_stack_remove(s_list_window, true);
}

// Restart the inactivity countdown (called from every button handler).
static void user_activity(void) {
  if (window_stack_get_top_window() == s_home.window) return;
  if (s_idle_timer) app_timer_reschedule(s_idle_timer, HOME_TIMEOUT_MS);
  else s_idle_timer = app_timer_register(HOME_TIMEOUT_MS, go_home, NULL);
}

static void open_list(void) {
  s_list_touched = false;  // list opens on its top row
  s_list_sel_dev = -1;
  window_stack_push(s_list_window, true);
  user_activity();
}

static void pages_redraw(void) {
  if (s_home.layer) layer_mark_dirty(s_home.layer);
  if (s_dev.layer) layer_mark_dirty(s_dev.layer);
}

static Layer *top_page_layer(void) {
  Window *top = window_stack_get_top_window();
  if (top == s_home.window) return s_home.layer;
  if (top == s_dev.window) return s_dev.layer;
  return NULL;
}

static bool page_shows(int dev) {
  return (s_home.layer && s_home.cur == dev) || (s_dev.layer && s_dev.cur == dev);
}

static void remove_device_page_if(int count) {
  if (s_dev.cur >= count && s_dev.window && window_stack_contains_window(s_dev.window)) {
    window_stack_remove(s_dev.window, true);
  }
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------
static void init(void) {
  if (persist_exists(PERSIST_SCAN_MODE)) {
    int m = persist_read_int(PERSIST_SCAN_MODE);
    if (m >= 0 && m < SCAN_MODE_COUNT) s_scan_mode = m;
  }
  s_icon_ellipsis = gbitmap_create_with_resource(RESOURCE_ID_ICON_ELLIPSIS);
  s_icon_play = gbitmap_create_with_resource(RESOURCE_ID_ICON_PLAY);
  s_icon_pause = gbitmap_create_with_resource(RESOURCE_ID_ICON_PAUSE);
  s_icon_skip_back = gbitmap_create_with_resource(RESOURCE_ID_ICON_SKIP_BACKWARD);
  s_icon_skip_fwd = gbitmap_create_with_resource(RESOURCE_ID_ICON_SKIP_FORWARD);
  s_icon_vol_up = gbitmap_create_with_resource(RESOURCE_ID_ICON_VOLUME_UP);
  s_icon_vol_down = gbitmap_create_with_resource(RESOURCE_ID_ICON_VOLUME_DOWN);
  s_icon_join = gbitmap_create_with_resource(RESOURCE_ID_ICON_JOIN);
  s_icon_unlink = gbitmap_create_with_resource(RESOURCE_ID_ICON_UNLINK);
  s_icon_arrow_up = gbitmap_create_with_resource(RESOURCE_ID_ICON_ARROW_UP);
  s_icon_arrow_down = gbitmap_create_with_resource(RESOURCE_ID_ICON_ARROW_DOWN);

  page_window_init(&s_home);
  page_window_init(&s_dev);

  s_list_window = window_create();
  window_set_background_color(s_list_window, GColorBlack);
  window_set_window_handlers(s_list_window, (WindowHandlers){
    .load = list_window_load,
    .unload = list_window_unload,
    .appear = list_window_appear,
    .disappear = list_window_disappear,
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

  window_stack_push(s_home.window, true);
}

static void deinit(void) {
  if (s_idle_timer) app_timer_cancel(s_idle_timer);
  window_destroy(s_settings_window);
  window_destroy(s_list_window);
  window_destroy(s_dev.window);
  window_destroy(s_home.window);
  GBitmap *icons[] = { s_icon_ellipsis, s_icon_play, s_icon_pause, s_icon_skip_back, s_icon_skip_fwd,
                       s_icon_vol_up, s_icon_vol_down, s_icon_join, s_icon_unlink,
                       s_icon_arrow_up, s_icon_arrow_down };
  for (unsigned i = 0; i < ARRAY_LENGTH(icons); i++) gbitmap_destroy(icons[i]);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
