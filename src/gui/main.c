#include <Raven.h>
#include "log_viewer/app/options.h"
#include "log_viewer/core/search.h"
#include "log_viewer/core/query.h"
#include "log_viewer/core/source_tree.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

/* The UI owns widgets; this mutex protects only the worker mailbox and copied rows. */
typedef struct {
  pthread_mutex_t mutex;
  Search *search;
  bool stop, follow, paused, configure;
  size_t detail_index;
  bool detail_requested, detail_ready;
  char *detail_text;
  Rid detail, detail_panel;
  bool details_visible;
  size_t start, loaded, count;
  unsigned generation;
  char expression[QUERY_CAPACITY], error[160];
  char rows[512][4097];
  SearchStatus status;
  Rid table, status_label, input;
} Gui;

static void *reader(void *context) {
  Gui *gui = context;
  RecordReader reader = { 0 };
  TextBuffer buffer = { 0 };
  char (*rows)[4097] = calloc(512, 4097);
  if (!rows) return nullptr;

  for (;;) {
    pthread_mutex_lock(&gui->mutex);
    bool stop = gui->stop, configure = gui->configure, paused = gui->paused;
    bool detail_requested = gui->detail_requested;
    size_t detail_index = gui->detail_index;
    gui->detail_requested = false;
    size_t start = gui->start;
    unsigned generation = gui->generation;
    char expression[QUERY_CAPACITY];
    memcpy(expression, gui->expression, sizeof(expression));
    gui->configure = false;
    pthread_mutex_unlock(&gui->mutex);
    if (stop) break;

    if (configure) {
      Query query = { 0 };
      if (query_compile(&query, expression)) search_configure(gui->search, expression, SEARCH_ALL_SOURCES, search_levels(gui->search));
      pthread_mutex_lock(&gui->mutex);
      snprintf(gui->error, sizeof(gui->error), "%s", query.error);
      pthread_mutex_unlock(&gui->mutex);
      query_destroy(&query);
    }

    if (detail_requested) {
      LogRecord record;
      int read = search_read_result(gui->search, detail_index, &reader, &buffer, &record, 1024 * 1024);
      char *copy = strdup(read == 1 ? record.text : "Record unavailable");
      pthread_mutex_lock(&gui->mutex);
      free(gui->detail_text);
      gui->detail_text = copy;
      gui->detail_ready = true;
      pthread_mutex_unlock(&gui->mutex);
    }
    if (paused) {
      usleep(100000);
      continue;
    }

    SearchStatus status = search_status(gui->search);
    size_t count = 0;
    for (; count < 512 && start + count < status.matches; count++) {
      LogRecord record;
      int result = search_read_result(gui->search, start + count, &reader, &buffer, &record, 4096);
      if (result != 1) break;
      snprintf(rows[count], 4097, "%s", record.text);
    }

    SearchStatus after = search_status(gui->search);
    pthread_mutex_lock(&gui->mutex);
    if (generation == gui->generation && status.revision == after.revision && status.history_epoch == after.history_epoch) {
      gui->loaded = start;
      gui->count = count;
      memcpy(gui->rows, rows, count * 4097);
      gui->status = status;
    }
    pthread_mutex_unlock(&gui->mutex);
    Rwake_up();
    usleep(100000);
  }

  free(rows);
  record_reader_destroy(&reader);
  text_buffer_destroy(&buffer);
  return nullptr;
}

static const char *cell(Rid table, int row, int column, void *context) {
  (void)table;
  (void)column;
  Gui *gui = context;
  static char text[4097];
  pthread_mutex_lock(&gui->mutex);
  if ((size_t)row >= gui->loaded && (size_t)row - gui->loaded < gui->count) {
    memcpy(text, gui->rows[(size_t)row - gui->loaded], sizeof(text));
  } else {
    strcpy(text, "Loading…");
    if ((size_t)row < gui->start || (size_t)row >= gui->start + 512) {
      gui->start = row > 20 ? (size_t)row - 20 : 0;
      gui->generation++;
    }
  }
  pthread_mutex_unlock(&gui->mutex);
  return text;
}

static void filter(Rid widget, const char *text, void *context) {
  (void)widget;
  Gui *gui = context;
  pthread_mutex_lock(&gui->mutex);
  snprintf(gui->expression, sizeof(gui->expression), "%s", text);
  gui->configure = true;
  gui->generation++;
  gui->count = 0;
  pthread_mutex_unlock(&gui->mutex);
}

static void follow(Rid widget, void *context) {
  (void)widget;
  Gui *gui = context;
  gui->follow = !gui->follow;
  Rbutton_set_text(widget, gui->follow ? "Follow: on" : "Follow: off");
}

static void pause_updates(Rid widget, void *context) {
  (void)widget;
  Gui *gui = context;
  pthread_mutex_lock(&gui->mutex);
  gui->paused = !gui->paused;
  bool paused = gui->paused;
  pthread_mutex_unlock(&gui->mutex);
  Rbutton_set_text(widget, paused ? "Resume viewport" : "Pause viewport");
}

static void details(Rid widget, int row, void *context) {
  (void)widget;
  Gui *gui = context;
  pthread_mutex_lock(&gui->mutex);
  gui->detail_index = (size_t)row;
  gui->detail_requested = true;
  gui->details_visible = true;
  Rwidget_set_visibility(gui->detail_panel, R_VISIBLE);
  pthread_mutex_unlock(&gui->mutex);
}

static void toggle_details(Rid widget, void *context) {
  (void)widget;
  Gui *gui = context;
  gui->details_visible = !gui->details_visible;
  Rwidget_set_visibility(gui->detail_panel, gui->details_visible ? R_VISIBLE : R_COLLAPSED);
}

/* SourceTree supplies labels and hierarchy; this wireframe does not change source scope. */
static void populate_tree(Rid widget, Search *search) {
  SourceTree tree = { 0 };
  if (!source_tree_sync(&tree, search)) return;

  Rid *parents = calloc(tree.count + 1, sizeof(*parents));
  if (!parents) {
    source_tree_destroy(&tree);
    return;
  }

  for (size_t i = 0; i < tree.count; i++) {
    SourceTreeRow row = tree.rows[i];
    char *name = strndup(row.name, row.length);
    if (!name) break;

    Rid parent = row.depth ? parents[row.depth - 1] : R_INVALID_ID;
    Rid item = Rtree_view_add_item(widget, parent, name);
    parents[row.depth] = item;
    Rtree_item_set_expanded(item, R_TRUE);
    free(name);
  }

  free(parents);
  source_tree_destroy(&tree);
}

static void build_workspace(Gui *gui, const Options *options) {
  Rid window = Rwindow_create("Log viewer", 1280, 820, R_WINDOW_DEFAULT);
  Rwindow_set_min_size(window, 720, 480);
  Rid root = Rwidget_create(R_WIDGET_VBOX);
  Rwindow_set_content(window, root);

  Rid workspace = Rwidget_add_child(root, Rwidget_create(R_WIDGET_SPLITTER));
  Rsplitter_set_orientation(workspace, R_HORIZONTAL);
  Rsplitter_set_min_pane_size(workspace, 180.0f);
  Rslot_set_fill(workspace, 1.0f);

  Rid explorer = Rwidget_add_child(workspace, Rwidget_create(R_WIDGET_VBOX));
  Rslot_set_fill(explorer, 0.22f);
  Rid heading = Rwidget_add_child(explorer, Rlabel_create("EXPLORER"));
  Rslot_set_padding(heading, 14, 12, 14, 12);
  Rid tree = Rwidget_add_child(explorer, Rwidget_create(R_WIDGET_TREE_VIEW));
  Rslot_set_fill(tree, 1.0f);
  populate_tree(tree, gui->search);

  Rid tabs = Rwidget_add_child(workspace, Rwidget_create(R_WIDGET_TAB_VIEW));
  Rslot_set_fill(tabs, 0.78f);
  Rtab_view_set_documents(tabs, R_TRUE);
  Rid editor = Rwidget_create(R_WIDGET_VBOX);
  Rtab_view_add_tab(tabs, "All logs", editor);

  Rid toolbar = Rwidget_add_child(editor, Rwidget_create(R_WIDGET_HBOX));
  Rpanel_set_spacing(toolbar, 8.0f);
  Rslot_set_padding(toolbar, 10, 8, 10, 8);
  gui->input = Rwidget_add_child(toolbar, Rwidget_create(R_WIDGET_TEXT_INPUT));
  Rslot_set_fill(gui->input, 1.0f);
  Rtext_input_set_placeholder(gui->input, "Filter logs…  Enter to apply");
  Rtext_input_set_text(gui->input, options->filter);
  Rtext_input_set_committed_callback(gui->input, filter, gui);
  Rid button = Rwidget_add_child(toolbar, Rbutton_create("Follow: off"));
  Rbutton_set_clicked_callback(button, follow, gui);
  button = Rwidget_add_child(toolbar, Rbutton_create("Pause viewport"));
  Rbutton_set_clicked_callback(button, pause_updates, gui);
  button = Rwidget_add_child(toolbar, Rbutton_create("Details"));
  Rbutton_set_clicked_callback(button, toggle_details, gui);

  Rid panes = Rwidget_add_child(editor, Rwidget_create(R_WIDGET_SPLITTER));
  Rsplitter_set_orientation(panes, R_VERTICAL);
  Rslot_set_fill(panes, 1.0f);
  gui->table = Rwidget_add_child(panes, Rwidget_create(R_WIDGET_TABLE));
  Rwidget_set_font(gui->table, Rfont_get_monospace());
  Rtable_set_activated_callback(gui->table, details, gui);
  Rslot_set_fill(gui->table, 1.0f);
  Rtable_add_column(gui->table, "Message", 1800);
  Rtable_set_column_sortable(gui->table, 0, R_FALSE);

  gui->detail_panel = Rwidget_add_child(panes, Rwidget_create(R_WIDGET_VBOX));
  Rslot_set_fill(gui->detail_panel, 0.35f);
  Rwidget_set_visibility(gui->detail_panel, R_COLLAPSED);
  heading = Rwidget_add_child(gui->detail_panel, Rlabel_create("RECORD DETAILS  ·  Enter or double-click a log"));
  Rslot_set_padding(heading, 12, 8, 12, 8);
  gui->detail = Rwidget_add_child(gui->detail_panel, Rwidget_create(R_WIDGET_TEXT_EDITOR));
  Rslot_set_fill(gui->detail, 1.0f);
  Rtext_editor_set_read_only(gui->detail, R_TRUE);
  Rtext_editor_set_word_wrap(gui->detail, R_FALSE);

  Rid status = Rwidget_add_child(root, Rwidget_create(R_WIDGET_HBOX));
  Rslot_set_padding(status, 12, 5, 12, 5);
  gui->status_label = Rwidget_add_child(status, Rlabel_create("Opening logs…"));
  Rslot_set_fill(gui->status_label, 1.0f);
  Rwidget_add_child(status, Rlabel_create("UTF-8  ·  Log viewer"));
}

int main(int argc, char **argv) {
  Options options;
  OptionsResult parsed = options_parse(&options, argc, argv);
  if (parsed != OPTIONS_RUN) {
    puts(parsed == OPTIONS_HELP ? options_usage() : options.error);
    free(options.inputs);
    return parsed == OPTIONS_HELP ? 0 : 2;
  }

  Gui *gui = calloc(1, sizeof(*gui));
  if (!gui) {
    free(options.inputs);
    return 1;
  }
  pthread_mutex_init(&gui->mutex, nullptr);
  if (search_open_inputs(&gui->search, options.inputs, options.input_count, options.docker_tail, options.filter, true) != SOURCE_OPENED) {
    perror("Log inputs");
    free(options.inputs);
    free(gui);
    return 1;
  }
  search_set_levels(gui->search, options.levels);
  if (!Rinit(R_BACKEND_VULKAN)) {
    search_close(gui->search);
    free(options.inputs);
    free(gui);
    return 1;
  }
  Rtheme_apply(R_THEME_DARK);

  build_workspace(gui, &options);

  pthread_t worker;
  if (pthread_create(&worker, nullptr, reader, gui)) {
    Rshutdown();
    search_close(gui->search);
    free(options.inputs);
    free(gui);
    return 1;
  }
  while (Rwait_frame(0.1)) {
    pthread_mutex_lock(&gui->mutex);
    SearchStatus status = gui->status;
    if (gui->detail_ready) {
      Rtext_editor_set_text(gui->detail, gui->detail_text ? gui->detail_text : "Allocation failed");
      gui->detail_ready = false;
    }
    char label[256];
    snprintf(label, sizeof(label), "%zu indexed · %zu matches%s %s", status.count, status.matches, status.searching ? " · searching" : "", gui->error);
    pthread_mutex_unlock(&gui->mutex);
    Rlabel_set_text(gui->status_label, label);
    int count = status.matches > INT_MAX ? INT_MAX : (int)status.matches;
    Rtable_set_virtual_rows(gui->table, count, cell, gui);
    if (gui->follow && count) Rtable_scroll_to_row(gui->table, count - 1);
  }

  pthread_mutex_lock(&gui->mutex);
  gui->stop = true;
  pthread_mutex_unlock(&gui->mutex);
  pthread_join(worker, nullptr);
  search_close(gui->search);
  Rshutdown();
  pthread_mutex_destroy(&gui->mutex);
  free(gui->detail_text);
  free(gui);
  free(options.inputs);
  return 0;
}
