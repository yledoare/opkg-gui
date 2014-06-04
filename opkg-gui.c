/*
 * TODO:
 *  - edit config file
 *  - install from file
 *  - display full info about a package
 *  - error handling!
 *  - search
 */

#include <stdio.h>
#include <stdlib.h>

#include <linux/limits.h>

#include <glib.h>
#include <gtk/gtk.h>

typedef unsigned int uint; // For opkg because of C99

#include <libopkg/opkg.h>
#include <libopkg/pkg_parse.h>

// TODO: elminate these global variables
static GtkWidget *window;
static GtkTreeStore *store;
static GtkWidget *progress_bar;
static GtkTextBuffer *log_buf;
static GtkTextBuffer *pkg_buf;
GtkWidget *notebook;
GtkWidget *pkg_info;
GtkWidget *toolbar;
GtkToolItem *log_button;

// Structure with packages to add or remove
GSList *tasks; // we don't expect too many packages, so we just use a list

typedef enum ePage {
    PAGE_LIST,
    PAGE_LOG
} ePage;

enum {
    COL_SECTION = 0,
    COL_INSTALLED,
    COL_NAME,
    COL_VERS,
    COL_DESC,

    COL_VISIBLE,
    COL_CHANGED,
    NUM_COLS
};

// Single item of the task list
typedef struct tTaskItem {
    const char *pkg_name;
    char *pkg_version;
    int pkg_status;
    const GtkTreeIter *iter;
} tTaskItem;

// user_data for ocb_add_package
typedef struct tAddPkgData {
    GtkTreeStore* store;        // tree store
    GHashTable* section_hash;   // hash with section names and tree iterators
} tAddPkgData;

// user_data for ocb_progress
typedef struct tProgressData {
    uint actions;               // number of actions to perform
    uint current;               // which action is being performed
                                // (0 <= current < actions)
    gboolean success;           // whehter the proces as a whole was successfull
} tProgressData;


// ============================================================================
//   Function Prototypes
// ============================================================================

// opkg Callbacks
void ocb_add_package(pkg_t *pkg, void *user_data);
void ocb_vmessage(int level, const char *fmt, va_list ap);
void ocb_progress(const opkg_progress_data_t *prog, void *user_data);
// GTK Callbacks
gboolean window_on_destroy(GtkWidget *widget, GdkEvent  *event,
    gpointer user_data);
gboolean apply_on_click(GtkWidget *widget, GdkEvent *event, gpointer data);
gboolean update_on_click(GtkWidget *widget, GdkEvent *event, gpointer data);
gboolean upgrade_on_click(GtkWidget *widget, GdkEvent *event, gpointer data);
void installed_on_toggled(GtkCellRendererToggle *cell, gchar *path_str,
    gpointer data);
gboolean row_on_select(GtkTreeSelection *selection, GtkTreeModel *model,
  GtkTreePath *path, gboolean path_currently_selected, gpointer user_data);
// Handling the task list
gint tasklist_cmp(gconstpointer a, gconstpointer b);
void tasklist_add_remove(pkg_t *pkg, GtkTreeIter *iter);
gboolean tasklist_foreach(GFunc func);
void tasklist_clear();
void tasklist_free_item(gpointer data, gpointer user_data);
// Routines
gboolean log_message(int level, const char *msg);
void do_add_remove(gpointer data, gpointer user_data);
void dlg(GtkMessageType type, const char *fmt, ...);
void switch_page(ePage page, gboolean working);
void populate_tree(GtkTreeStore *store);
GtkWidget* create_pkgtree();
GtkWidget* create_toolbar();

#define ERRDLG(msg) dlg(GTK_MESSAGE_ERROR, msg)
#define ERRDLGF(fmt, ...) dlg(GTK_MESSAGE_ERROR, fmt, __VA_ARGS__)
#define INFODLG(msg) dlg(GTK_MESSAGE_INFO, msg)
#define INFODLGF(fmt, ...) dlg(GTK_MESSAGE_INFO, fmt, __VA_ARGS__)


// ============================================================================
//   opkg Callbacks
// ============================================================================

void ocb_add_package(pkg_t *pkg, void *user_data)
{

    if (user_data == NULL)
        return; // or abort?

    tAddPkgData *data = (tAddPkgData*)user_data;

    char *vstr;
    GtkTreeIter iter, *section, tmp;
    GtkTreeStore *store = data->store;
    GHashTable *hash = data->section_hash;
    gboolean installed;

    // TODO: need option to filter these
    //if (strstr(pkg->name, "-dbg") || strstr(pkg->name, "-dev"))
    //  return;

    vstr = pkg_version_str_alloc(pkg);
    installed = (pkg->state_status == SS_INSTALLED ? TRUE : FALSE);

    // Find the section
    section = (GtkTreeIter*) g_hash_table_lookup(hash, pkg->section);
    if (section == NULL) {
        gtk_tree_store_append(store, &tmp, NULL);
        gtk_tree_store_set(store, &tmp,
                COL_SECTION, pkg->section,
                COL_INSTALLED, FALSE,
                COL_NAME, "",
                COL_VERS, "",
                COL_DESC, "",
                COL_CHANGED, FALSE,
                COL_VISIBLE, FALSE,
                -1);
        section = &tmp;

        g_hash_table_insert(hash,
            g_strdup(pkg->section), gtk_tree_iter_copy(section));
    }

    gtk_tree_store_append(store, &iter, section);
    gtk_tree_store_set(store, &iter,
            COL_SECTION, "",
            COL_INSTALLED, installed,
            COL_NAME, pkg->name,
            COL_VERS, vstr,
            COL_DESC, pkg->description,
            COL_CHANGED, FALSE,
            COL_VISIBLE, TRUE,
            -1);

    free(vstr);
}

void ocb_progress(const opkg_progress_data_t *prog, void *user_data)
{
    if (user_data == NULL) {
        ERRDLGF("%s: Internal error, user_data == NULL!", __FUNCTION__);
        return;
    }

    tProgressData *data = (tProgressData*)user_data;
    gdouble fraction;

    fraction = data->current + (gdouble)prog->percentage/100.0;
    fraction /= data->actions;

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), fraction);

    // Handle GTK any events
    while (gtk_events_pending())
        gtk_main_iteration();
}

// Callback that handles opkg error/notification messages
void ocb_vmessage(int level, const char *fmt, va_list ap)
{
    char *str;

    str = g_strdup_vprintf(fmt, ap);

    fprintf(stderr, "opkg message(%d): %s\n", level, str);

    if (log_message(level, str) == FALSE) {
        // Show dialog
        ERRDLGF("opkg error:\n\n%s\n", str);
    }

    free(str);
}


// ============================================================================
//   GTK callbacks
// ============================================================================

gboolean window_on_destroy(GtkWidget *widget, GdkEvent  *event,
    gpointer user_data)
{
    tasklist_clear();
    opkg_free();

    gtk_main_quit();

    return FALSE;
}

gboolean apply_on_click(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    switch_page(PAGE_LOG, TRUE);

    if (tasklist_foreach(do_add_remove) == FALSE) {
        ERRDLG("Failed to add/remove one or more pacakges!");
        switch_page(PAGE_LOG, FALSE);
    } else {
        switch_page(PAGE_LIST, FALSE);
    }
    // Reset the progress bar, just to be sure
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 1.0);

    // Clear the task list
    tasklist_clear();

    // Refresh the tree
    populate_tree(store);

    return FALSE;
}

gboolean update_on_click(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    // Tree iterators and all the pkg_t pointers may become invalid
    tasklist_clear();

    switch_page(PAGE_LOG, TRUE);

    tProgressData pdata = { 1, 0 };
    if (opkg_update_package_lists(ocb_progress, &pdata)) {
        ERRDLG("Failed to update list of pacakges!");
        switch_page(PAGE_LOG, FALSE);
    } else {
        switch_page(PAGE_LIST, FALSE);
    }

    // Refresh the package list
    populate_tree(store);

    return FALSE;
}

gboolean upgrade_on_click(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    // Tree iterators and all the pkg_t pointers may become invalid
    tasklist_clear();

    switch_page(PAGE_LOG, TRUE);

    tProgressData pdata = { 1, 0 };
    if (opkg_upgrade_all(ocb_progress, &pdata)) {
        ERRDLG("Failed to upgrade one or more packages!");
        switch_page(PAGE_LOG, FALSE);
    } else {
        switch_page(PAGE_LIST, FALSE);
    }

    // Refresh the package list
    populate_tree(store);

    return FALSE;
}

void log_on_toggled(GtkToggleToolButton *button, gpointer data) {
    gboolean active;
    g_object_get(button, "active", &active, NULL);

    if (active) {
        switch_page(PAGE_LOG, FALSE);
    } else {
        switch_page(PAGE_LIST, FALSE);
    }
}

void installed_on_toggled(GtkCellRendererToggle *cell, gchar *path_str,
    gpointer data) {

    GtkTreeModel *model = (GtkTreeModel*)data;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter iter;

    gboolean state;
    gboolean changed;
    gchar *name, *version;

    // Get package and its current state
    gtk_tree_model_get_iter(model, &iter, path);
    gtk_tree_model_get(model, &iter,
        COL_INSTALLED, &state,
        COL_NAME, &name,
        COL_VERS, &version,
        COL_CHANGED, &changed,
        -1);

    // Toggle the value
    state = !state;
    changed = !changed;
    gtk_tree_store_set(GTK_TREE_STORE (model), &iter,
        COL_INSTALLED, state,
        COL_CHANGED, changed,
        -1);

    // Find the package info
    pkg_t *pkg = pkg_hash_fetch_by_name_version(name, version);
    if (pkg == NULL) {
        ERRDLGF("%s: Internal error, can't find package '%s' ver. '%s'!",
            __FUNCTION__, name, version);
        g_free(name);
        g_free(version);
    }

    tasklist_add_remove(pkg, &iter);

    // Clean up
    gtk_tree_path_free(path);
    g_free(name);
    g_free(version);
}

gboolean row_on_select(GtkTreeSelection *selection, GtkTreeModel *model,
  GtkTreePath *path, gboolean selected, gpointer user_data)
{
  if (selected == TRUE) {
      // Row is being deselected, do nothing
      return TRUE;
  }

  GtkTreeIter iter;
  gtk_tree_model_get_iter(model, &iter, path);

  gchar *section, *name, *version;
  gtk_tree_model_get(model, &iter,
      COL_SECTION, &section,
      COL_NAME, &name,
      COL_VERS, &version,
      -1);

  if ((section != NULL) && (*section != 0)) {
      // This is a section
      g_free(section);
      g_free(name);
      g_free(version);

      return TRUE;
  }
  g_free(section);

  pkg_t *pkg = pkg_hash_fetch_by_name_version(name, version);
  if (pkg == NULL) {
      ERRDLGF("%s: Internal error, can't find package '%s' ver. '%s'!",
          __FUNCTION__, name, version);
      return FALSE;
  }

  // Clear the text box
  gtk_text_buffer_set_text(pkg_buf, "", 0);

  // ... add section, name and version
  GtkTextIter textiter;
  gchar *str = g_strdup_printf("[%s] %s %s",
      pkg->section, name, version);
  gtk_text_buffer_get_end_iter(pkg_buf, &textiter);
  gtk_text_buffer_insert_with_tags_by_name(pkg_buf, &textiter, str, -1, "bold",
      NULL);
  g_free(str);

  // ... add package description
  str = g_strdup_printf("\n%s", pkg->description);
  gtk_text_buffer_get_end_iter(pkg_buf, &textiter);
  gtk_text_buffer_insert(pkg_buf, &textiter, str, -1);
  g_free(str);

  g_free(name);
  g_free(version);

  return TRUE;
}

// ============================================================================
// Handling the task list
// ============================================================================

// Compare function
gint tasklist_cmp(gconstpointer a, gconstpointer b) {
    return strcmp(((tTaskItem*)a)->pkg_name, ((tTaskItem*)b)->pkg_name);
}

void tasklist_free_item(gpointer data, gpointer user_data) {
    tTaskItem *item = (tTaskItem*)data;
  //  free(item);
/*
    free(item->pkg_version);
	// YLD
        fprintf(stderr, "1.\n");
    if(item != NULL) free(item);
        fprintf(stderr, "2.\n");
*/
}

void tasklist_add_remove(pkg_t *pkg, GtkTreeIter *iter) {
    // We could probably simply use the pointer to the pkt_t structure as key
    // in tree structure. The pointer cannot change except possibly during the
    // add/remove procedure. Using a list is a little bit safer.
    //
    // Still, we rely on the fact that:
    //  "[...] most often we only create new pkg_t structs, we don't often free
    //  them." -- pkg.h

    tTaskItem *item = malloc(sizeof(tTaskItem));
    item->pkg_name = pkg->name;
    item->pkg_version = pkg_version_str_alloc(pkg);
    item->pkg_status = pkg->state_status;
    item->iter = iter;

    GSList *old = g_slist_find_custom(tasks, item, tasklist_cmp);
    if (old == NULL) {
        tasks = g_slist_prepend(tasks, item);
    } else {
        // Was already in the list, remove it
        tasklist_free_item(item, NULL);
        tasks = g_slist_delete_link(tasks, old);
        tasklist_free_item(old, NULL);
    }
}

gboolean tasklist_foreach(GFunc func) {
    // Prepare the data
    tProgressData pdata;
    pdata.actions = g_slist_length(tasks);
    pdata.current = 0;
    pdata.success = TRUE;
    // Do the job
    g_slist_foreach(tasks, func, &pdata);
    return pdata.success;
}

// Create/clear the task list
void tasklist_clear() {
    if (tasks != NULL) {
        g_slist_foreach(tasks, tasklist_free_item, NULL);
       g_slist_free (tasks);
        tasks = NULL;
    }
    // NOTE: there is no 'new' function, just having NULL value is enough
}

// ============================================================================
//   Routines
// ============================================================================

gboolean log_message(int level, const char *msg) {
    GtkTextIter iter;
    if (log_buf != NULL) {
        gtk_text_buffer_get_end_iter(log_buf, &iter);

        if (level == ERROR) {
            GtkTextTag *tag;
            tag = gtk_text_buffer_create_tag(log_buf, NULL,
                "background", "red",
                "foreground", "black", NULL);
            gtk_text_buffer_insert_with_tags(log_buf, &iter, msg, -1,
                tag, NULL);
        } else {
            gtk_text_buffer_insert(log_buf, &iter, msg, -1);

            // Handle GTK any events
            while (gtk_events_pending())
                gtk_main_iteration();
        }
    } else {
        return FALSE;
    }

    return TRUE;
}

//
// Show info in popup dialog
//
void dlg(GtkMessageType type, const char *fmt, ...)
{
    char *str;
    va_list ap;
    GtkWidget *dialog;

    va_start(ap, fmt);
    str = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    // First log the message
    log_message((type == GTK_MESSAGE_ERROR ? ERROR : NOTICE), str);

    // Now show the dialog
    dialog = gtk_message_dialog_new(GTK_WINDOW(window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        type, GTK_BUTTONS_OK,
        str);
    gtk_window_set_title(GTK_WINDOW(dialog), "L:D_N:dialog_ID:opkg-gui");
    gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);

    g_free(str);
}

void switch_page(ePage page, gboolean working) {
    gboolean log_active;
    log_active = gtk_toggle_tool_button_get_active(
        GTK_TOGGLE_TOOL_BUTTON(log_button));

    if (page == PAGE_LIST) {
        if (log_active == TRUE) {
            gtk_toggle_tool_button_set_active(
                GTK_TOGGLE_TOOL_BUTTON(log_button), FALSE);
        }
        g_object_set(toolbar, "sensitive", TRUE, NULL);
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 0);
    } else {
        if (log_active == FALSE) {
            gtk_toggle_tool_button_set_active(
                GTK_TOGGLE_TOOL_BUTTON(log_button), TRUE);
        }
        if (working == TRUE) {
            g_object_set(toolbar, "sensitive", FALSE, NULL);
        } else {
            g_object_set(toolbar, "sensitive", TRUE, NULL);
        }
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 1);
    }
}

//
// Fill tree with the list of packages
//
void populate_tree(GtkTreeStore *store)
{
    // First clear the content
    gtk_tree_store_clear(store);

    // Prepare user data for callback
    tAddPkgData data;
    data.store = store;
    data.section_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
        free, (GDestroyNotify) gtk_tree_iter_free);

    // Fill her up
    opkg_list_packages(ocb_add_package, &data);
}

void do_add_remove(gpointer data, gpointer user_data)
{
    tTaskItem *item = (tTaskItem*)data;
    tProgressData *pdata = (tProgressData*)user_data;

    pkg_t *pkg_new = pkg_hash_fetch_by_name_version(
        item->pkg_name, item->pkg_version);

    if (pkg_new == NULL) {
        ERRDLGF("%s: Internal error, package '%s' ver. '%s' disappeared!\n",
            __FUNCTION__, item->pkg_name, item->pkg_version);
        pdata->success = FALSE;
        return;
    } else if (item->pkg_status != pkg_new->state_status) {
        // State has changed, skip the package
        gchar *str = g_strdup_printf(
            ":: NOTE: The state of the package '%s' has changed! "
            "Skipping it ...\n", item->pkg_name);
        log_message(NOTICE, str);
        g_free(str);
        // Not fatal, the package might have got installed as a dependency
        return;
    }

    if (item->pkg_status == SS_INSTALLED) {
        if (opkg_remove_package(item->pkg_name, ocb_progress, pdata) != 0)
            pdata->success = FALSE;
    } else {
        if (opkg_install_package(item->pkg_name, ocb_progress, pdata) != 0)
            pdata->success = FALSE;
    }

    pdata->current++;
}

GtkWidget* create_pkgtree()
{
    GtkTreeModel *model;
    GtkTreeView *treeview;

    treeview = GTK_TREE_VIEW(gtk_tree_view_new());

    GtkTreeSelection *selection = gtk_tree_view_get_selection(treeview);
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);

    gtk_tree_selection_set_select_function(selection,
        row_on_select, NULL, NULL);

    store = gtk_tree_store_new(NUM_COLS,
            G_TYPE_STRING,
            G_TYPE_BOOLEAN,
            G_TYPE_STRING,
            G_TYPE_STRING,
            G_TYPE_STRING,

            G_TYPE_BOOLEAN,
            G_TYPE_BOOLEAN);
    populate_tree(store);

    model = GTK_TREE_MODEL(store);

    // Section name
    gtk_tree_view_insert_column_with_attributes(treeview,
        -1, "Section", gtk_cell_renderer_text_new(),
        "text", COL_SECTION,
        /*"visible", COL_VISIBLE,*/
        NULL);

    // TODO: do something here, the highlighting is disgusting!

    // Installed checkbox
    GtkCellRenderer *renderer = gtk_cell_renderer_toggle_new();
    gtk_tree_view_insert_column_with_attributes(treeview,
        -1, "I", renderer,
        "active", COL_INSTALLED,
        "visible", COL_VISIBLE,
        "cell-background-set", COL_CHANGED,
        NULL);
    g_signal_connect (renderer, "toggled", G_CALLBACK (installed_on_toggled),
        model);
    g_object_set(renderer, "cell-background", "#ff8888", NULL);

    // Package name
    renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(treeview,
        -1, "Package", renderer,
        "text", COL_NAME,
        "cell-background-set", COL_CHANGED,
    //    "weight-set", COL_CHANGED,
        NULL);
    //g_object_set(GTK_CELL_RENDERER_TEXT(renderer), "weight-set", TRUE, NULL);
    //g_object_set(GTK_CELL_RENDERER_TEXT(renderer), "weight", PANGO_WEIGHT_BOLD, NULL);
    g_object_set(renderer,    "cell-background", "#ff8888", NULL);

    // Package version
    gtk_tree_view_insert_column_with_attributes(treeview,
        -1, "Version", gtk_cell_renderer_text_new(),
        "text", COL_VERS,
        "visible", COL_VISIBLE,
        NULL);
    g_object_set(renderer,    "cell-background", "#ff8888", NULL);

    // Package description
    gtk_tree_view_insert_column_with_attributes(treeview,
        -1, "Description", gtk_cell_renderer_text_new(),
        "text", COL_DESC,
        "visible", COL_VISIBLE,
        NULL);

    gtk_tree_view_columns_autosize(treeview);
    //gtk_tree_view_set_rules_hint(treeview, TRUE);

    gtk_tree_view_set_model(treeview, model);
    g_object_unref(model);

    return GTK_WIDGET(treeview);
}


GtkWidget* create_toolbar()
{
    GtkWidget *toolbar;
    GtkToolItem *item;

    toolbar = gtk_toolbar_new();

    // Apply changes
    item = gtk_tool_button_new_from_stock(GTK_STOCK_APPLY);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);
    g_signal_connect(G_OBJECT(item), "clicked",
            G_CALLBACK(apply_on_click), NULL);

    // Update list of packages
    item = gtk_tool_button_new_from_stock(GTK_STOCK_REFRESH);
    gtk_tool_button_set_label(GTK_TOOL_BUTTON(item), "Update");
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);
    g_signal_connect(G_OBJECT(item), "clicked",
            G_CALLBACK(update_on_click), NULL);

    // Upgrade packages
    item = gtk_tool_button_new_from_stock(GTK_STOCK_NETWORK);
    gtk_tool_button_set_label(GTK_TOOL_BUTTON(item), "Upgrade");
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);
    g_signal_connect(item, "clicked",
            G_CALLBACK(upgrade_on_click), NULL);

    // Log
    item = log_button = gtk_toggle_tool_button_new_from_stock(GTK_STOCK_EDIT);
    gtk_tool_button_set_label(GTK_TOOL_BUTTON(item), "Log");
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);
    g_signal_connect(item, "toggled",
            G_CALLBACK(log_on_toggled), NULL);

    // Separator
    item = gtk_separator_tool_item_new();
    gtk_tool_item_set_expand(GTK_TOOL_ITEM(item), TRUE);
    gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(item), FALSE);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);

    // Close
    item = gtk_tool_button_new_from_stock(GTK_STOCK_CLOSE);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);
    g_signal_connect_swapped (item, "clicked",
        G_CALLBACK(gtk_widget_destroy), window);

    return toolbar;
}

// ============================================================================
//   Main
// ============================================================================

int
main(int argc, char **argv)
{
    GtkWidget *pkg_view, *log_view;
    GtkWidget *pkg_window, *log_window;
    GtkWidget *window_frame;
    GtkWidget *pkg_frame;
    GtkWidget *log_frame;

    gtk_init(&argc, &argv);

    // Don't filter any fields (for now?)
    conf->pfm = 0;
    // Just notices and errors -- the important stuff
    conf->verbosity = NOTICE;
    // Automatically remove packages auto-installed as dependency
    conf->autoremove = 1;

#ifdef _DEBUG
    //conf->conf_file = g_strdup("opkg.conf");
    char pwd[PATH_MAX+1];
    getcwd(pwd, PATH_MAX+1); /* full path is required here */
    conf->offline_root = g_strdup_printf("%s/root/", pwd);
#endif

    conf->opkg_vmessage = ocb_vmessage;

    if (opkg_new()) {
        fprintf(stderr, "Failed to initialise libopkg, bailing.\n");
        return -1;
    }

    // Window
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    g_signal_connect (window, "destroy",
        G_CALLBACK(window_on_destroy), NULL);

    // This is how windows should be named on Kindle Touch
    // layer: application layer, type: application, name: opkg-gui */
    gtk_window_set_title(GTK_WINDOW(window), "L:A_N:application_ID:opkg-gui");

    window_frame = gtk_vbox_new(FALSE, 0);

    // Toolbar
    toolbar = create_toolbar();

    // Notebook (with invisible tabs)
    notebook = gtk_notebook_new();
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);

    // Tab with packages
    pkg_frame = gtk_vbox_new(FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), pkg_frame, NULL);

    // ... scrolled window
    pkg_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pkg_window),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(pkg_frame), pkg_window, TRUE, TRUE, 0);

    // ... and tree with packages
    pkg_view = create_pkgtree();
    gtk_container_add(GTK_CONTAINER(pkg_window), pkg_view);

    // ... package info text box
    pkg_info = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(pkg_info), FALSE);
    g_object_set(pkg_info, "wrap-mode", GTK_WRAP_WORD_CHAR, NULL);

    pkg_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(pkg_info));
    gtk_text_buffer_set_text(pkg_buf, "Welcome into Dibab package manager", -1);
    gtk_text_buffer_create_tag(pkg_buf, "bold",
        "weight", PANGO_WEIGHT_BOLD,
        NULL);
    gtk_box_pack_end(GTK_BOX(pkg_frame), pkg_info, FALSE, FALSE, 0);

    // Tab with opkg log
    log_frame = gtk_vbox_new(FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), log_frame, NULL);

    // ... scrolled window
    log_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(log_window),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(log_frame), log_window, TRUE, TRUE, 0);

    // ... text box with the log
    log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_container_add(GTK_CONTAINER(log_window), log_view);

    log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));

    // ... progress bar
    progress_bar = gtk_progress_bar_new();
    gtk_box_pack_end(GTK_BOX(log_frame), progress_bar, FALSE, FALSE, 0);

    // Add all to the window
    gtk_box_pack_start(GTK_BOX(window_frame), toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(window_frame), notebook, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(window), window_frame);

    gtk_window_set_default_size(GTK_WINDOW(window), 400, 300);
    gtk_widget_show_all(window);

    gtk_main();

    return 0;
}

// vim: et sw=4
