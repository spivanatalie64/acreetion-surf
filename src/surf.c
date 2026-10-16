/*
 * surf.c — AcreetionOS Surf, C browser shell
 * ===========================================
 * Talks to WebKitGTK 4.1's native C API directly (zero binding overhead).
 * Rust services (main.rs) handle all file I/O, JSON, and subprocess work via FFI.
 *
 * Tuned for natalie-inspiron5490aio:
 *   i5-10210U 4C/8T, 11 GiB RAM (~5 free), Intel UHD, spinning disk, dual 1080p.
 *
 * Build: cargo build --release  (build.rs compiles this via cc + links GTK/WebKit)
 */

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <JavaScriptCore/JavaScript.h>
#include <json-glib/json-glib.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

/* ─── Rust FFI declarations ─────────────────────────────────────────────────── */

extern void   surf_free_str(char *ptr);
extern void   surf_history_add(const char *url, const char *title);
extern char  *surf_history_json(int limit);
extern int    surf_bookmark_add(const char *url, const char *title);
extern char  *surf_bookmarks_json(void);
extern void   surf_geometry_load(int *w, int *h, int *x, int *y);
extern void   surf_geometry_save(int w, int h, int x, int y);

/* ─── Constants ─────────────────────────────────────────────────────────────── */

#define APP_TITLE       "AcreetionOS Surf"
#define HOMEPAGE        "https://acreetionos.org"
#define USER_AGENT      "Mozilla/5.0 (X11; Linux x86_64; AcreetionOS) " \
                        "AppleWebKit/605.1.15 (KHTML, like Gecko) " \
                        "Version/18.0 Safari/605.1.15 acreetion-surf/2.0"

static const char *CONFIG_DIR  = NULL;
static const char *DATA_DIR    = NULL;
static const char *CACHE_DIR   = NULL;
static const char *COOKIE_FILE = NULL;
static const char *IPC_SOCKET  = NULL;

/* ─── Dark mode CSS (injected into every page via UserStyleSheet) ───────────── */

static const char *DARK_MODE_CSS =
    ":root { color-scheme: dark !important; }\n"
    "html { background-color: #1a1a1a !important; }\n"
    "html:not([style*=\"background\"]), body:not([style*=\"background\"]) {\n"
    "    background-color: #1a1a1a !important;\n"
    "}\n"
    "body { color: #e5e5e5 !important; }\n"
    "a { color: #5dade2 !important; }\n"
    "a:visited { color: #bb8fce !important; }\n"
    "input, textarea, select, button {\n"
    "    color-scheme: dark !important;\n"
    "    background-color: #2a2a2a !important;\n"
    "    color: #e5e5e5 !important;\n"
    "    border-color: #444 !important;\n"
    "}\n"
    "img, video, picture, canvas, svg, iframe { background: none !important; }\n"
    "::-webkit-scrollbar { width: 12px; height: 12px; }\n"
    "::-webkit-scrollbar-track { background: #1a1a1a; }\n"
    "::-webkit-scrollbar-thumb { background: #444; border-radius: 6px; }\n"
    "::-webkit-scrollbar-thumb:hover { background: #555; }\n";

/* Dark GTK theme CSS for the browser chrome */
static const char *UI_CSS =
    "window { background-color: #1a1a1a; }\n"
    ".notebook tab { padding: 4px 12px; }\n"
    ".notebook tab-label { color: #ccc; }\n"
    ".notebook tab:checked { background-color: #2a2a2a; }\n"
    ".entry { background-color: #2a2a2a; color: #eee; border: 1px solid #444; }\n"
    ".toolbar { background-color: #1a1a1a; border-bottom: 1px solid #333; }\n"
    "button { padding: 2px 6px; }\n";

/* Autofill detection JS */
static const char *AUTOFILL_DETECT_JS =
    "(function(){"
    "  var p=document.querySelectorAll('input[type=\"password\"]');"
    "  if(!p.length)return null;"
    "  var u=null,a=Array.from(document.querySelectorAll('input'));"
    "  for(var i=0;i<p.length;i++){"
    "    var f=p[i].closest('form');"
    "    if(f){var fi=Array.from(f.querySelectorAll('input'));var pi=fi.indexOf(p[i]);"
    "      for(var j=pi-1;j>=0;j--){var t=fi[j];"
    "        if(t.type==='text'||t.type==='email'||t.type===''||t.type==='tel'){u=t;break;}}}}"
    "  return{hasLoginForm:true};"
    "})();";

/* Autofill helper JS (injected once per page load) */
static const char *AUTOFILL_INJECT_JS =
    "(function(){"
    "  window.__asAutofill=function(u,p){"
    "    var pf=document.querySelectorAll('input[type=\"password\"]');if(!pf.length)return false;"
    "    var pwd=pf[0];pwd.value=p;pwd.dispatchEvent(new Event('input',{bubbles:1}));"
    "    pwd.dispatchEvent(new Event('change',{bubbles:1}));"
    "    var a=Array.from(document.querySelectorAll('input'));var pi=a.indexOf(pwd);"
    "    for(var i=pi-1;i>=0;i--){var t=a[i];"
    "      if(t.type==='text'||t.type==='email'||t.type===''||t.type==='tel'){"
    "        t.value=u;t.dispatchEvent(new Event('input',{bubbles:1}));"
    "        t.dispatchEvent(new Event('change',{bubbles:1}));return true;}}"
    "    return true;"
    "  };"
    "  document.addEventListener('submit',function(e){"
    "    var f=e.target,pf=f.querySelectorAll('input[type=\"password\"]');if(!pf.length)return;"
    "    var a=Array.from(f.querySelectorAll('input')),un='',pi=a.indexOf(pf[0]);"
    "    for(var i=pi-1;i>=0;i--){var t=a[i];"
    "      if(t.type==='text'||t.type==='email'||t.type===''||t.type==='tel'){un=t.value;break;}}"
    "    window.__asLoginSubmit={username:un,password:pf[0].value,url:location.href};"
    "  },true);"
    "})();";

/* ─── Data structures ──────────────────────────────────────────────────────── */

typedef struct {
    WebKitWebView  *webview;
    GtkWidget      *scrolled;
    GtkWidget      *label;
    char           *last_uri;   /* last committed URI (for history dedup) */
} BrowserTab;

typedef struct {
    GtkWidget      *window;
    GtkWidget      *url_bar;
    GtkWidget      *notebook;
    WebKitWebContext *ctx;
    BrowserTab    **tabs;
    int             n_tabs;
    int             tab_cap;
    gboolean        fullscreen;
} BrowserApp;

static BrowserApp app;

/* Forward declarations */
static gboolean idle_new_tab(gchar *url);
void new_tab(const char *url);
static void close_tab(BrowserTab *tab);

/* ─── URL bar autocomplete (history-backed) ─────────────────────────────────── */

/* URL-bar autocomplete (history-backed) */
static GtkListStore *comp_store  = NULL;
static GHashTable   *comp_urls   = NULL;

static void comp_record(const char *url) {
    if (!comp_store || !url || !*url) return;
    if (g_hash_table_contains(comp_urls, url)) return;
    g_hash_table_insert(comp_urls, g_strdup(url), GINT_TO_POINTER(1));
    GtkTreeIter iter;
    gtk_list_store_append(comp_store, &iter);
    gtk_list_store_set(comp_store, &iter, 0, url, -1);
}

static void completion_init(void) {
    comp_store = gtk_list_store_new(1, G_TYPE_STRING);
    comp_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    char *json = surf_history_json(500);
    if (json && *json) {
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, json, -1, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_ARRAY(root)) {
                JsonArray *arr = json_node_get_array(root);
                guint len = json_array_get_length(arr);
                for (guint i = 0; i < len; i++) {
                    JsonObject *obj = json_array_get_object_element(arr, i);
                    const char *url = json_object_get_string_member(obj, "url");
                    if (url) comp_record(url);
                }
            }
        }
        g_object_unref(parser);
    }
    surf_free_str(json);

    GtkEntryCompletion *c = gtk_entry_completion_new();
    gtk_entry_completion_set_model(c, GTK_TREE_MODEL(comp_store));
    gtk_entry_completion_set_text_column(c, 0);
    gtk_entry_completion_set_inline_completion(c, TRUE);
    gtk_entry_completion_set_popup_completion(c, TRUE);
    gtk_entry_set_completion(GTK_ENTRY(app.url_bar), c);
}


/* ─── Helpers ───────────────────────────────────────────────────────────────── */

static char *strndup_safe(const char *s, gsize n) {
    char *r = g_strndup(s, n);
    return r;
}

static char *get_domain(const char *url) {
    if (!url) return g_strdup("");
    const char *p = strstr(url, "://");
    if (p) p += 3; else p = url;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    const char *end = slash;
    if (colon && (!end || colon < end)) end = colon;
    char *host = end ? g_strndup(p, end - p) : g_strdup(p);
    /* last two parts */
    char **parts = g_strsplit(host, ".", -1);
    int n = g_strv_length(parts);
    char *domain;
    if (n >= 2) {
        domain = g_strdup_printf("%s.%s", parts[n-2], parts[n-1]);
    } else {
        domain = g_strdup(host);
    }
    g_strfreev(parts);
    g_free(host);
    return domain;
}

static char *normalize_url(const char *input) {
    if (!input || !*input) return g_strdup(HOMEPAGE);
    if (g_str_has_prefix(input, "http://") || g_str_has_prefix(input, "https://") ||
        g_str_has_prefix(input, "file://") || g_str_has_prefix(input, "about:"))
        return g_strdup(input);
    if (strchr(input, ' ') || !strchr(input, '.')) {
        /* Startpage: real results, no bot-challenge on this network (DDG
           serves her an anomaly loop = 3s+). Light HTML → fast on WebKit. */
        return g_strdup_printf("https://www.startpage.com/sp/search?query=%s",
                               g_uri_escape_string(input, NULL, FALSE));
    }
    return g_strdup_printf("https://%s", input);
}

/* ─── Tab management ────────────────────────────────────────────────────────── */

static BrowserTab *find_tab_by_page(GtkWidget *page) {
    for (int i = 0; i < app.n_tabs; i++) {
        if (app.tabs[i]->scrolled == page)
            return app.tabs[i];
    }
    return NULL;
}

static BrowserTab *current_tab(void) {
    int idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(app.notebook));
    if (idx < 0) return NULL;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.notebook), idx);
    return find_tab_by_page(page);
}

static int tab_index(BrowserTab *tab) {
    for (int i = 0; i < app.n_tabs; i++) {
        if (app.tabs[i] == tab) return i;
    }
    return -1;
}

static gboolean is_current_tab(BrowserTab *tab) {
    return tab_index(tab) == gtk_notebook_get_current_page(GTK_NOTEBOOK(app.notebook));
}

static void on_title_changed(WebKitWebView *wv, GParamSpec *pspec, BrowserTab *tab) {
    const char *title = webkit_web_view_get_title(wv);
    if (!title) title = webkit_web_view_get_uri(wv);
    if (!title) title = "Untitled";
    char short_title[31];
    g_strlcpy(short_title, title, sizeof(short_title));
    gtk_label_set_text(GTK_LABEL(tab->label), short_title);
    if (is_current_tab(tab)) {
        char *win_title = g_strdup_printf("%s — %s", title, APP_TITLE);
        gtk_window_set_title(GTK_WINDOW(app.window), win_title);
        g_free(win_title);
    }
    /* JS-set titles can arrive AFTER load-finished; record them too —
       Rust dedupes same-URL consecutives by refreshing title/time in place. */
    const char *uri = webkit_web_view_get_uri(wv);
    if (title && uri)
        surf_history_add(uri, title);
}

static void on_load_finished(WebKitWebView *wv, WebKitLoadEvent event, BrowserTab *tab) {
    if (event != WEBKIT_LOAD_FINISHED) return;

    const char *uri = webkit_web_view_get_uri(wv);
    if (!uri) return;

    /* History: one write per finished navigation (Rust dedupes consecutive) */
    if (!tab->last_uri || strcmp(tab->last_uri, uri) != 0) {
        g_free(tab->last_uri);
        tab->last_uri = g_strdup(uri);
        const char *title = webkit_web_view_get_title(wv);
        surf_history_add(uri, title ? title : "");
    }

    /* Update URL bar if this is the active tab */
    if (is_current_tab(tab)) {
        gtk_entry_set_text(GTK_ENTRY(app.url_bar), uri);
    }

    /* Feed autocomplete with the new URL */
    comp_record(uri);

    /* Inject autofill helper */
    webkit_web_view_evaluate_javascript(wv, AUTOFILL_INJECT_JS, -1,
                                        NULL, NULL, NULL, NULL, NULL);
}

static WebKitWebView *on_create_window(WebKitWebView *wv, WebKitNavigationAction *act, gpointer data) {
    /* Open new windows / target=_blank / middle-click as a new tab.
       Defer to idle so we don't modify the notebook during signal emission. */
    WebKitURIRequest *req = webkit_navigation_action_get_request(act);
    const char *uri = webkit_uri_request_get_uri(req);
    if (uri) {
        char *url = g_strdup(uri);
        g_idle_add((GSourceFunc)idle_new_tab, url);
    }
    return NULL; /* prevent default new window */
}

/* We need a proper idle callback for new-tab-from-create */
static gboolean idle_new_tab(gchar *url) {
    extern void new_tab(const char *url);
    new_tab(url);
    g_free(url);
    return FALSE;
}

static void close_tab(BrowserTab *tab) {
    int idx = tab_index(tab);
    if (idx < 0) return;

    gtk_notebook_remove_page(GTK_NOTEBOOK(app.notebook), idx);

    /* Remove from array */
    for (int i = idx; i < app.n_tabs - 1; i++)
        app.tabs[i] = app.tabs[i + 1];
    app.n_tabs--;

    g_free(tab->last_uri);
    gtk_widget_destroy(tab->scrolled);
    g_free(tab);

    if (app.n_tabs == 0) {
        extern void new_tab(const char *url);
        new_tab(HOMEPAGE);
    }
}

/* ─── Tab creation ──────────────────────────────────────────────────────────── */

void new_tab(const char *url) {
    BrowserTab *tab = g_new0(BrowserTab, 1);

    /* Create webview with shared context */
    tab->webview = WEBKIT_WEB_VIEW(webkit_web_view_new_with_context(app.ctx));

    /* Settings */
    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_smooth_scrolling(settings, TRUE);
    webkit_settings_set_enable_page_cache(settings, TRUE);
    webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
    webkit_settings_set_enable_developer_extras(settings, FALSE);
    webkit_settings_set_user_agent(settings, USER_AGENT);
    webkit_web_view_set_settings(tab->webview, settings);

    /* Dark mode: inject user stylesheet into all frames */
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(tab->webview);
    if (ucm) {
        WebKitUserStyleSheet *ss = webkit_user_style_sheet_new(
            DARK_MODE_CSS,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_STYLE_LEVEL_USER,
            NULL, NULL);
        webkit_user_content_manager_add_style_sheet(ucm, ss);
    }

    /* Scroll container */
    tab->scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(tab->scrolled), GTK_WIDGET(tab->webview));
    gtk_widget_show_all(tab->scrolled);

    /* Tab label */
    tab->label = gtk_label_new("Loading…");
    gtk_label_set_max_width_chars(GTK_LABEL(tab->label), 25);
    gtk_label_set_ellipsize(GTK_LABEL(tab->label), PANGO_ELLIPSIZE_END);
    gtk_widget_show(tab->label);

    /* Close button */
    GtkWidget *close_btn = gtk_button_new_with_label("×");
    gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);
    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(close_tab), tab);

    /* Tab box */
    GtkWidget *tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(tab_box), tab->label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tab_box), close_btn, FALSE, FALSE, 0);
    gtk_widget_show_all(tab_box);

    /* Append to notebook */
    int idx = gtk_notebook_append_page(GTK_NOTEBOOK(app.notebook), tab->scrolled, tab_box);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app.notebook), idx);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(app.notebook), tab->scrolled, TRUE);

    /* Signals */
    g_signal_connect(tab->webview, "notify::title", G_CALLBACK(on_title_changed), tab);
    g_signal_connect(tab->webview, "load-changed", G_CALLBACK(on_load_finished), tab);
    g_signal_connect(tab->webview, "create", G_CALLBACK(on_create_window), NULL);

    /* Track in array */
    if (app.n_tabs >= app.tab_cap) {
        app.tab_cap = app.tab_cap ? app.tab_cap * 2 : 8;
        app.tabs = g_renew(BrowserTab*, app.tabs, app.tab_cap);
    }
    app.tabs[app.n_tabs++] = tab;

    /* Load URL */
    if (url) {
        char *normalized = normalize_url(url);
        webkit_web_view_load_uri(tab->webview, normalized);
        g_free(normalized);
    }
}

/* ─── Navigation actions ────────────────────────────────────────────────────── */

static void on_back(GtkButton *btn, gpointer data) {
    BrowserTab *tab = current_tab();
    if (tab && webkit_web_view_can_go_back(tab->webview))
        webkit_web_view_go_back(tab->webview);
}

static void on_forward(GtkButton *btn, gpointer data) {
    BrowserTab *tab = current_tab();
    if (tab && webkit_web_view_can_go_forward(tab->webview))
        webkit_web_view_go_forward(tab->webview);
}

static void on_refresh(GtkButton *btn, gpointer data) {
    BrowserTab *tab = current_tab();
    if (tab) webkit_web_view_reload(tab->webview);
}

static void on_url_activate(GtkEntry *entry, gpointer data) {
    BrowserTab *tab = current_tab();
    if (!tab) return;
    const char *text = gtk_entry_get_text(entry);
    char *url = normalize_url(text);
    webkit_web_view_load_uri(tab->webview, url);
    g_free(url);
}

static gboolean on_url_keypress(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    if (event->keyval == GDK_KEY_Escape) {
        BrowserTab *tab = current_tab();
        if (tab) {
            const char *uri = webkit_web_view_get_uri(tab->webview);
            gtk_entry_set_text(GTK_ENTRY(app.url_bar), uri ? uri : "");
            gtk_widget_grab_focus(GTK_WIDGET(tab->webview));
        }
        return TRUE;
    }
    return FALSE;
}

static void on_switch_tab(GtkNotebook *nb, GtkWidget *page, guint idx, gpointer data) {
    BrowserTab *tab = find_tab_by_page(page);
    if (!tab) return;
    const char *uri = webkit_web_view_get_uri(tab->webview);
    gtk_entry_set_text(GTK_ENTRY(app.url_bar), uri ? uri : "");
    const char *title = webkit_web_view_get_title(tab->webview);
    char *win_title = g_strdup_printf("%s — %s",
                                      title ? title : (uri ? uri : APP_TITLE),
                                      APP_TITLE);
    gtk_window_set_title(GTK_WINDOW(app.window), win_title);
    g_free(win_title);
}

/* ─── Geometry persistence ──────────────────────────────────────────────────── */

static gboolean on_configure(GtkWidget *win, GdkEventConfigure *event, gpointer data) {
    int w, h, x, y;
    gtk_window_get_size(GTK_WINDOW(win), &w, &h);
    gtk_window_get_position(GTK_WINDOW(win), &x, &y);
    surf_geometry_save(w, h, x, y);
    return FALSE;
}

/* ─── Bookmarks ─────────────────────────────────────────────────────────────── */

static void on_bookmark_activate(GtkMenuItem *item, const char *url) {
    new_tab(url);
}

static void on_add_bookmark(GtkButton *btn, gpointer data) {
    BrowserTab *tab = current_tab();
    if (!tab) return;
    const char *url = webkit_web_view_get_uri(tab->webview);
    if (!url) return;
    const char *title = webkit_web_view_get_title(tab->webview);
    if (!title) title = url;
    surf_bookmark_add(url, title);
}

/* ─── Fullscreen ────────────────────────────────────────────────────────────── */

static void on_toggle_fullscreen(GtkButton *btn, gpointer data) {
    if (app.fullscreen) {
        gtk_window_unfullscreen(GTK_WINDOW(app.window));
        app.fullscreen = FALSE;
    } else {
        gtk_window_fullscreen(GTK_WINDOW(app.window));
        app.fullscreen = TRUE;
    }
}

/* ─── Dev tools toggle (F12) ────────────────────────────────────────────────── */

static void on_toggle_devtools(GtkButton *btn, gpointer data) {
    BrowserTab *tab = current_tab();
    if (!tab) return;
    WebKitSettings *s = webkit_web_view_get_settings(tab->webview);
    gboolean enabled = webkit_settings_get_enable_developer_extras(s);
    webkit_settings_set_enable_developer_extras(s, !enabled);
    WebKitWebInspector *insp = webkit_web_view_get_inspector(tab->webview);
    if (!enabled)
        webkit_web_inspector_show(insp);
    else
        webkit_web_inspector_close(insp);
}

/* ─── Quit ──────────────────────────────────────────────────────────────────── */

static void on_quit(GtkButton *btn, gpointer data) {
    gtk_widget_destroy(app.window);
}

static void on_window_destroy(GtkWidget *win, gpointer data) {
    /* Save geometry */
    int w, h, x, y;
    gtk_window_get_size(GTK_WINDOW(win), &w, &h);
    gtk_window_get_position(GTK_WINDOW(win), &x, &y);
    surf_geometry_save(w, h, x, y);

    /* Clean up IPC socket */
    if (IPC_SOCKET) g_unlink(IPC_SOCKET);

    gtk_main_quit();
}

/* ─── Close current tab ─────────────────────────────────────────────────────── */

static void on_close_tab(GtkButton *btn, gpointer data) {
    BrowserTab *tab = current_tab();
    if (tab) close_tab(tab);
}

/* ─── Downloads ─────────────────────────────────────────────────────────────── */

static void on_download_finished(WebKitDownload *dl, gpointer data) {
    const char *dest = webkit_download_get_destination(dl);
    if (dest) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "✓ Download complete:\n%s", dest);
        g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);
        gtk_widget_show(dialog);
    }
}

static void on_download_failed(WebKitDownload *dl, GError *error, gpointer data) {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
        "✗ Download failed: %s", error ? error->message : "unknown error");
    g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);
    gtk_widget_show(dialog);
}

static void on_download_started(WebKitWebContext *ctx, WebKitDownload *dl, gpointer data) {
    /* "suggested-filename" is a GObject property, not a getter function */
    char *suggested = NULL;
    g_object_get(G_OBJECT(dl), "suggested-filename", &suggested, NULL);
    if (!suggested || !*suggested) {
        g_free(suggested);
        suggested = g_strdup("download");
    }

    char *downloads_dir = g_build_filename(g_get_home_dir(), "Downloads", NULL);
    g_mkdir_with_parents(downloads_dir, 0755);

    char *dest = g_build_filename(downloads_dir, suggested, NULL);

    /* Avoid clobbering */
    if (g_file_test(dest, G_FILE_TEST_EXISTS)) {
        char *base = g_path_get_basename(dest);
        char *dirname = g_path_get_dirname(dest);
        const char *dot = strrchr(base, '.');
        int i = 1;
        char *newdest = NULL;
        do {
            i++;
            if (dot) {
                char *stem = g_strndup(base, dot - base);
                newdest = g_strdup_printf("%s/%s (%d)%s", dirname, stem, i, dot);
                g_free(stem);
            } else {
                newdest = g_strdup_printf("%s/%s (%d)", dirname, base, i);
            }
        } while (g_file_test(newdest, G_FILE_TEST_EXISTS));
        g_free(dest);
        dest = newdest;
        g_free(base);
        g_free(dirname);
    }

    char *uri = g_strdup_printf("file://%s", dest);
    webkit_download_set_destination(dl, uri);
    g_free(uri);

    g_signal_connect(dl, "finished", G_CALLBACK(on_download_finished), NULL);
    g_signal_connect(dl, "failed", G_CALLBACK(on_download_failed), NULL);

    g_free(dest);
    g_free(downloads_dir);
    g_free(suggested);
}

/* ─── IPC for --new-tab ─────────────────────────────────────────────────────── */

static gboolean on_ipc_data(GIOChannel *src, GIOCondition cond, gpointer data) {
    if (cond & G_IO_IN) {
        int fd = g_io_channel_unix_get_fd(src);
        struct sockaddr_un addr;
        socklen_t addr_len = sizeof(addr);
        int conn = accept(fd, (struct sockaddr*)&addr, &addr_len);
        if (conn >= 0) {
            char buf[4096];
            ssize_t n = read(conn, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                g_strchomp(buf);
                if (*buf) {
                    char *url = g_strdup(buf);
                    g_idle_add((GSourceFunc)idle_new_tab, url);
                }
            }
            close(conn);
        }
    }
    return TRUE; /* keep watching */
}

static void setup_ipc(void) {
    if (!IPC_SOCKET) return;

    /* Remove stale socket */
    g_unlink(IPC_SOCKET);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, IPC_SOCKET, sizeof(addr.sun_path));

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return;
    }
    listen(fd, 5);

    GIOChannel *chan = g_io_channel_unix_new(fd);
    g_io_add_watch(chan, G_IO_IN, on_ipc_data, NULL);
    g_io_channel_unref(chan);
}

gboolean ipc_send_new_tab(const char *url) {
    if (!IPC_SOCKET) return FALSE;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return FALSE;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, IPC_SOCKET, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return FALSE;
    }
    write(fd, url, strlen(url));
    close(fd);
    return TRUE;
}

/* ─── Menu construction ─────────────────────────────────────────────────────── */

static GtkWidget *build_bookmarks_submenu(void) {
    GtkWidget *menu = gtk_menu_new();
    char *json = surf_bookmarks_json();

    if (json && *json) {
        JsonParser *parser = json_parser_new();
        GError *err = NULL;
        if (json_parser_load_from_data(parser, json, -1, &err)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_ARRAY(root)) {
                JsonArray *arr = json_node_get_array(root);
                guint len = json_array_get_length(arr);
                for (guint i = 0; i < len; i++) {
                    JsonObject *obj = json_array_get_object_element(arr, i);
                    const char *url = json_object_get_string_member(obj, "url");
                    const char *title = json_object_get_string_member(obj, "title");
                    if (!title) title = url;
                    char short_title[41];
                    g_strlcpy(short_title, title, sizeof(short_title));
                    GtkWidget *item = gtk_menu_item_new_with_label(short_title);
                    g_signal_connect(item, "activate", G_CALLBACK(on_bookmark_activate),
                                     (gpointer)g_strdup(url));
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
                }
            }
        }
        g_object_unref(parser);
        if (err) g_error_free(err);
    }

    if (!gtk_container_get_children(GTK_CONTAINER(menu))) {
        GtkWidget *item = gtk_menu_item_new_with_label("No bookmarks yet");
        gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    surf_free_str(json);
    return menu;
}

static GtkWidget *build_history_submenu(void) {
    GtkWidget *menu = gtk_menu_new();
    char *json = surf_history_json(20);

    if (json && *json) {
        JsonParser *parser = json_parser_new();
        GError *err = NULL;
        if (json_parser_load_from_data(parser, json, -1, &err)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_ARRAY(root)) {
                JsonArray *arr = json_node_get_array(root);
                guint len = json_array_get_length(arr);
                for (guint i = 0; i < len; i++) {
                    JsonObject *obj = json_array_get_object_element(arr, i);
                    const char *url = json_object_get_string_member(obj, "url");
                    const char *title = json_object_get_string_member(obj, "title");
                    if (!title || !*title) title = url;
                    char short_title[41];
                    g_strlcpy(short_title, title, sizeof(short_title));
                    GtkWidget *item = gtk_menu_item_new_with_label(short_title);
                    g_signal_connect(item, "activate", G_CALLBACK(on_bookmark_activate),
                                     (gpointer)g_strdup(url));
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
                }
            }
        }
        g_object_unref(parser);
        if (err) g_error_free(err);
    }

    if (!gtk_container_get_children(GTK_CONTAINER(menu))) {
        GtkWidget *item = gtk_menu_item_new_with_label("No history yet");
        gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    surf_free_str(json);
    return menu;
}

static void on_menu_clicked(GtkButton *btn, gpointer data) {
    GtkWidget *menu = gtk_menu_new();


    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    /* Bookmarks submenu */
    GtkWidget *bm_item = gtk_menu_item_new_with_label("Bookmarks");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(bm_item), build_bookmarks_submenu());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), bm_item);

    /* History submenu */
    GtkWidget *hist_item = gtk_menu_item_new_with_label("History");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(hist_item), build_history_submenu());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), hist_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    /* New tab */
    GtkWidget *new_item = gtk_menu_item_new_with_label("New Tab");
    g_signal_connect_swapped(new_item, "activate", G_CALLBACK(new_tab), (gpointer)HOMEPAGE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), new_item);

    /* Quit */
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(btn),
                             GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST, NULL);
}

/* ─── Keyboard shortcuts ────────────────────────────────────────────────────── */

static gboolean on_keypress(GtkWidget *win, GdkEventKey *event, gpointer data) {
    guint key = event->keyval;
    GdkModifierType mods = event->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK);

    /* Ctrl+T: new tab */
    if (key == GDK_KEY_t && mods == GDK_CONTROL_MASK) {
        new_tab(HOMEPAGE); return TRUE;
    }
    /* Ctrl+W: close tab */
    if (key == GDK_KEY_w && mods == GDK_CONTROL_MASK) {
        on_close_tab(NULL, NULL); return TRUE;
    }
    /* Ctrl+L: focus URL bar */
    if (key == GDK_KEY_l && mods == GDK_CONTROL_MASK) {
        gtk_widget_grab_focus(app.url_bar); return TRUE;
    }
    /* Ctrl+R: refresh */
    if (key == GDK_KEY_r && mods == GDK_CONTROL_MASK) {
        on_refresh(NULL, NULL); return TRUE;
    }
    /* Ctrl+Q: quit */
    if (key == GDK_KEY_q && mods == GDK_CONTROL_MASK) {
        on_quit(NULL, NULL); return TRUE;
    }
    /* Ctrl+D: bookmark */
    if (key == GDK_KEY_d && mods == GDK_CONTROL_MASK) {
        on_add_bookmark(NULL, NULL); return TRUE;
    }
    /* F11: fullscreen */
    if (key == GDK_KEY_F11 && mods == 0) {
        on_toggle_fullscreen(NULL, NULL); return TRUE;
    }
    /* F12: dev tools */
    if (key == GDK_KEY_F12 && mods == 0) {
        on_toggle_devtools(NULL, NULL); return TRUE;
    }
    /* Alt+Left/Right: back/forward */
    if (key == GDK_KEY_Left && mods == GDK_MOD1_MASK) {
        on_back(NULL, NULL); return TRUE;
    }
    if (key == GDK_KEY_Right && mods == GDK_MOD1_MASK) {
        on_forward(NULL, NULL); return TRUE;
    }
    return FALSE;
}

/* ─── Path setup ────────────────────────────────────────────────────────────── */

static void init_paths(void) {
    char *home = (char*)g_get_home_dir();
    CONFIG_DIR  = g_strdup_printf("%s/.config/acreetion-surf", home);
    DATA_DIR    = g_strdup_printf("%s/.local/share/acreetion-surf", home);
    CACHE_DIR   = g_strdup_printf("%s/.cache/acreetion-surf", home);
    COOKIE_FILE = g_strdup_printf("%s/cookies.txt", CONFIG_DIR);
    IPC_SOCKET  = g_strdup_printf("%s/ipc.sock", CACHE_DIR);

    g_mkdir_with_parents(CONFIG_DIR, 0755);
    g_mkdir_with_parents(DATA_DIR, 0755);
    g_mkdir_with_parents(CACHE_DIR, 0755);
}

/* ─── Main entry (called from Rust) ─────────────────────────────────────────── */

int surf_main(int argc, char **argv) {
    /* Parse args: --new-tab URL, --help, or [URL] */
    const char *start_url = HOMEPAGE;
    gboolean is_new_tab = FALSE;

    if (argc >= 2) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf(APP_TITLE " v2.0 — C + Rust native WebKitGTK browser\n\n"
                   "Usage:\n"
                   "  acreetion-surf [URL]          open browser\n"
                   "  acreetion-surf --new-tab URL  open URL in running instance\n\n"
                   "Keyboard:\n"
                   "  Ctrl+T/W/L/R/Q/D   tab/close/url/refresh/quit/bookmark\n"
                   "  Alt+Left/Right     back/forward\n"
                   "  F11                fullscreen\n"
                   "  F12                dev tools\n\n"
                   "Dark mode: forced via user stylesheet + GTK dark theme.\n"
                   "DRM: WebKitGTK has no Widevine CDM — DRM sites cannot play here.\n");
            return 0;
        }
        if (strcmp(argv[1], "--new-tab") == 0) {
            if (argc < 3) {
                fprintf(stderr, "usage: acreetion-surf --new-tab URL\n");
                return 1;
            }
            start_url = argv[2];
            is_new_tab = TRUE;
        } else {
            start_url = argv[1];
        }
    }

    /* If --new-tab, try IPC first */
    init_paths();
    if (is_new_tab) {
        if (ipc_send_new_tab(start_url))
            return 0;
        /* No running instance — fall through and open a window */
    }

    /* Init GTK */
    gtk_init(&argc, &argv);

    /* Dark GTK theme → propagates prefers-color-scheme:dark to web content */
    GtkSettings *gtk_settings = gtk_settings_get_default();
    g_object_set(gtk_settings, "gtk-application-prefer-dark-theme", TRUE, NULL);

    /* Apply UI CSS */
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, UI_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* Build WebContext with explicit WebsiteDataManager */
    WebKitWebsiteDataManager *manager = webkit_website_data_manager_new(
        "base-data-directory", DATA_DIR,
        "base-cache-directory", CACHE_DIR,
        NULL);

    /* Memory pressure: prevent swap-on-HDD death.
       Set strict first (invariant: conservative < strict < kill). */
    WebKitMemoryPressureSettings *mps = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_kill_threshold(mps, 0.85);
    webkit_memory_pressure_settings_set_strict_threshold(mps, 0.65);
    webkit_memory_pressure_settings_set_conservative_threshold(mps, 0.50);
    webkit_memory_pressure_settings_set_memory_limit(mps, 1536 * 1024 * 1024);
    webkit_memory_pressure_settings_set_poll_interval(mps, 2.0);
    webkit_website_data_manager_set_memory_pressure_settings(mps);

    app.ctx = webkit_web_context_new_with_website_data_manager(manager);
    webkit_web_context_set_cache_model(app.ctx, WEBKIT_CACHE_MODEL_WEB_BROWSER);

    /* Cookies */
    WebKitCookieManager *cm = webkit_web_context_get_cookie_manager(app.ctx);
    webkit_cookie_manager_set_persistent_storage(cm, COOKIE_FILE,
        WEBKIT_COOKIE_PERSISTENT_STORAGE_TEXT);

    /* Downloads */
    g_signal_connect(app.ctx, "download-started", G_CALLBACK(on_download_started), NULL);

    /* ─── Build the window ─── */
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), APP_TITLE);

    /* Restore geometry */
    int w, h, x, y;
    surf_geometry_load(&w, &h, &x, &y);
    gtk_window_set_default_size(GTK_WINDOW(app.window), w, h);
    if (x >= 0 && y >= 0)
        gtk_window_move(GTK_WINDOW(app.window), x, y);

    g_signal_connect(app.window, "destroy", G_CALLBACK(on_window_destroy), NULL);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_keypress), NULL);
    g_signal_connect(app.window, "configure-event", G_CALLBACK(on_configure), NULL);

    /* Main vertical box */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app.window), vbox);

    /* Toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_name(toolbar, "toolbar");
    g_object_set(toolbar, "margin", 4, NULL);

    GtkWidget *btn_back = gtk_button_new_with_label("←");
    GtkWidget *btn_fwd  = gtk_button_new_with_label("→");
    GtkWidget *btn_ref  = gtk_button_new_with_label("⟳");
    GtkWidget *btn_home = gtk_button_new_with_label("⌂");
    GtkWidget *btn_new  = gtk_button_new_with_label("+");

    app.url_bar = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.url_bar), "Search or enter address");
    gtk_widget_set_hexpand(app.url_bar, TRUE);

    GtkWidget *btn_menu = gtk_button_new_with_label("☰");

    gtk_box_pack_start(GTK_BOX(toolbar), btn_back, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_fwd, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_ref, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_home, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_new, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(toolbar), app.url_bar, TRUE, TRUE, 4);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_menu, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    /* Notebook */
    app.notebook = gtk_notebook_new();
    gtk_widget_set_name(app.notebook, "notebook");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(app.notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(app.notebook), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), app.notebook, TRUE, TRUE, 0);

    /* URL-bar autocomplete from history (repeat searches = instant) */
    completion_init();

    /* Signals */
    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back), NULL);
    g_signal_connect(btn_fwd, "clicked", G_CALLBACK(on_forward), NULL);
    g_signal_connect(btn_ref, "clicked", G_CALLBACK(on_refresh), NULL);
    g_signal_connect(btn_home, "clicked", G_CALLBACK(on_bookmark_activate), (gpointer)HOMEPAGE);
    g_signal_connect(btn_new, "clicked", G_CALLBACK(on_bookmark_activate), (gpointer)HOMEPAGE);
    g_signal_connect(app.url_bar, "activate", G_CALLBACK(on_url_activate), NULL);
    g_signal_connect(app.url_bar, "key-press-event", G_CALLBACK(on_url_keypress), NULL);
    g_signal_connect(btn_menu, "clicked", G_CALLBACK(on_menu_clicked), NULL);
    g_signal_connect(app.notebook, "switch-page", G_CALLBACK(on_switch_tab), NULL);

    /* Open first tab */
    app.tabs = NULL;
    app.n_tabs = 0;
    app.tab_cap = 0;

    new_tab(start_url);

    /* Start IPC server */
    setup_ipc();

    /* Show and run */
    gtk_widget_show_all(app.window);

    /* If start_url != HOMEPAGE, the first tab already loaded it in new_tab() */

    gtk_main();
    return 0;
}