//-----------------------------------------------------------------------------
// The GTK-based implementation of platform-dependent GUI functionality.
//
// Copyright 2018 whitequark
//-----------------------------------------------------------------------------
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <json-c/json_object.h>
#include <json-c/json_util.h>
#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/checkmenuitem.h>
#include <gtkmm/entry.h>
#include <gtkmm/filechooserdialog.h>
#include <gtkmm/fixed.h>
#include <gtkmm/glarea.h>
#include <gtkmm/main.h>
#include <gtkmm/menu.h>
#include <gtkmm/menubar.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/scrollbar.h>
#include <gtkmm/separatormenuitem.h>
#include <gtkmm/window.h>

#include "config.h"

#if defined(HAVE_SPACEWARE)
#   include <spnav.h>
#   include <gdk/gdkx.h>
#endif

#include "solvespace.h"

namespace SolveSpace {
namespace Platform {

//-----------------------------------------------------------------------------
// Utility functions
//-----------------------------------------------------------------------------

static std::string PrepareMnemonics(std::string label) {
    std::replace(label.begin(), label.end(), '&', '_');
    return label;
}

static std::string PrepareTitle(std::string title) {
    return title + " — SolveSpace";
}

//-----------------------------------------------------------------------------
// Fatal errors
//-----------------------------------------------------------------------------

void FatalError(std::string message) {
    fprintf(stderr, "%s", message.c_str());
    abort();
}

//-----------------------------------------------------------------------------
// Settings
//-----------------------------------------------------------------------------

class SettingsImplGtk : public Settings {
public:
    // Why aren't we using GSettings? Two reasons. It doesn't allow to easily see whether
    // the setting had the default value, and it requires to install a schema globally.
    Path         _path;
    json_object *_json = NULL;

    static Path GetConfigPath() {
        Path configHome;
        if(getenv("XDG_CONFIG_HOME")) {
            configHome = Path::From(getenv("XDG_CONFIG_HOME"));
        } else if(getenv("HOME")) {
            configHome = Path::From(getenv("HOME")).Join(".config");
        } else {
            dbp("neither XDG_CONFIG_HOME nor HOME are set");
            return Path::From("");
        }
        if(!configHome.IsEmpty()) {
            configHome = configHome.Join("solvespace");
        }

        const char *configHomeC = configHome.raw.c_str();
        struct stat st;
        if(stat(configHomeC, &st)) {
            if(errno == ENOENT) {
                if(mkdir(configHomeC, 0777)) {
                    dbp("cannot mkdir %s: %s", configHomeC, strerror(errno));
                    return Path::From("");
                }
            } else {
                dbp("cannot stat %s: %s", configHomeC, strerror(errno));
                return Path::From("");
            }
        } else if(!S_ISDIR(st.st_mode)) {
            dbp("%s is not a directory", configHomeC);
            return Path::From("");
        }

        return configHome.Join("settings.json");
    }

    SettingsImplGtk() {
        _path = GetConfigPath();
        if(_path.IsEmpty()) {
            dbp("settings will not be saved");
        } else {
            _json = json_object_from_file(_path.raw.c_str());
            if(!_json && errno != ENOENT) {
                dbp("cannot load settings: %s", strerror(errno));
            }
        }

        if(_json == NULL) {
            _json = json_object_new_object();
        }
    }

    ~SettingsImplGtk() {
        if(!_path.IsEmpty()) {
            // json-c <0.12 has the first argument non-const
            if(json_object_to_file_ext((char *)_path.raw.c_str(), _json,
                                       JSON_C_TO_STRING_PRETTY)) {
                dbp("cannot save settings: %s", strerror(errno));
            }
        }

        json_object_put(_json);
    }

    void FreezeInt(const std::string &key, uint32_t value) override {
        struct json_object *jsonValue = json_object_new_int(value);
        json_object_object_add(_json, key.c_str(), jsonValue);
    }

    uint32_t ThawInt(const std::string &key, uint32_t defaultValue) override {
        struct json_object *jsonValue;
        if(json_object_object_get_ex(_json, key.c_str(), &jsonValue)) {
            return json_object_get_int(jsonValue);
        }
        return defaultValue;
    }

    void FreezeBool(const std::string &key, bool value) override {
        struct json_object *jsonValue = json_object_new_boolean(value);
        json_object_object_add(_json, key.c_str(), jsonValue);
    }

    bool ThawBool(const std::string &key, bool defaultValue) override {
        struct json_object *jsonValue;
        if(json_object_object_get_ex(_json, key.c_str(), &jsonValue)) {
            return json_object_get_boolean(jsonValue);
        }
        return defaultValue;
    }

    void FreezeFloat(const std::string &key, double value) override {
        struct json_object *jsonValue = json_object_new_double(value);
        json_object_object_add(_json, key.c_str(), jsonValue);
    }

    double ThawFloat(const std::string &key, double defaultValue) override {
        struct json_object *jsonValue;
        if(json_object_object_get_ex(_json, key.c_str(), &jsonValue)) {
            return json_object_get_double(jsonValue);
        }
        return defaultValue;
    }

    void FreezeString(const std::string &key, const std::string &value) override {
        struct json_object *jsonValue = json_object_new_string(value.c_str());
        json_object_object_add(_json, key.c_str(), jsonValue);
    }

    std::string ThawString(const std::string &key,
                           const std::string &defaultValue = "") override {
        struct json_object *jsonValue;
        if(json_object_object_get_ex(_json, key.c_str(), &jsonValue)) {
            return json_object_get_string(jsonValue);
        }
        return defaultValue;
    }
};

SettingsRef GetSettings() {
    static std::shared_ptr<SettingsImplGtk> settings;
    if(!settings) {
        settings = std::make_shared<SettingsImplGtk>();
    }
    return settings;
}

//-----------------------------------------------------------------------------
// Timers
//-----------------------------------------------------------------------------

class TimerImplGtk : public Timer {
public:
    sigc::connection    _connection;

    void WindUp(unsigned milliseconds) override {
        if(!_connection.empty()) {
            _connection.disconnect();
        }

        auto handler = [this]() {
            if(this->onTimeout) {
                this->onTimeout();
            }
            return false;
        };
        _connection = Glib::signal_timeout().connect(handler, milliseconds);
    }
};

TimerRef CreateTimer() {
    return std::unique_ptr<TimerImplGtk>(new TimerImplGtk);
}

//-----------------------------------------------------------------------------
// GTK menu extensions
//-----------------------------------------------------------------------------

class GtkMenuItem : public Gtk::CheckMenuItem {
    Platform::MenuItem *_receiver;
    bool                _has_indicator;
    bool                _synthetic_event;

public:
    GtkMenuItem(Platform::MenuItem *receiver) :
        _receiver(receiver), _has_indicator(false), _synthetic_event(false) {
    }

    void set_accel_key(const Gtk::AccelKey &accel_key) {
        Gtk::CheckMenuItem::set_accel_key(accel_key);
    }

    bool has_indicator() const {
        return _has_indicator;
    }

    void set_has_indicator(bool has_indicator) {
        _has_indicator = has_indicator;
    }

    void set_active(bool active) {
        if(Gtk::CheckMenuItem::get_active() == active)
            return;

        _synthetic_event = true;
        Gtk::CheckMenuItem::set_active(active);
        _synthetic_event = false;
    }

protected:
    void on_activate() override {
        Gtk::CheckMenuItem::on_activate();

        if(!_synthetic_event && _receiver->onTrigger) {
            _receiver->onTrigger();
        }
    }

    void draw_indicator_vfunc(const Cairo::RefPtr<Cairo::Context> &cr) override {
        if(_has_indicator) {
            Gtk::CheckMenuItem::draw_indicator_vfunc(cr);
        }
    }
};

//-----------------------------------------------------------------------------
// Menus
//-----------------------------------------------------------------------------

class MenuItemImplGtk : public MenuItem {
public:
    GtkMenuItem gtkMenuItem;

    MenuItemImplGtk() : gtkMenuItem(this) {}

    void SetAccelerator(KeyboardEvent accel) override {
        guint accelKey;
        if(accel.key == KeyboardEvent::Key::CHARACTER) {
            if(accel.chr == '\t') {
                accelKey = GDK_KEY_Tab;
            } else if(accel.chr == '\x1b') {
                accelKey = GDK_KEY_Escape;
            } else if(accel.chr == '\x7f') {
                accelKey = GDK_KEY_Delete;
            } else {
                accelKey = gdk_unicode_to_keyval(accel.chr);
            }
        } else if(accel.key == KeyboardEvent::Key::FUNCTION) {
            accelKey = GDK_KEY_F1 + accel.num - 1;
        }

        Gdk::ModifierType accelMods = {};
        if(accel.shiftDown) {
            accelMods |= Gdk::SHIFT_MASK;
        }
        if(accel.controlDown) {
            accelMods |= Gdk::CONTROL_MASK;
        }

        gtkMenuItem.set_accel_key(Gtk::AccelKey(accelKey, accelMods));
    }

    void SetIndicator(Indicator type) override {
        switch(type) {
            case Indicator::NONE:
                gtkMenuItem.set_has_indicator(false);
                break;

            case Indicator::CHECK_MARK:
                gtkMenuItem.set_has_indicator(true);
                gtkMenuItem.set_draw_as_radio(false);
                break;

            case Indicator::RADIO_MARK:
                gtkMenuItem.set_has_indicator(true);
                gtkMenuItem.set_draw_as_radio(true);
                break;
        }
    }

    void SetActive(bool active) override {
        ssassert(gtkMenuItem.has_indicator(),
                 "Cannot change state of a menu item without indicator");
        gtkMenuItem.set_active(active);
    }

    void SetEnabled(bool enabled) override {
        gtkMenuItem.set_sensitive(enabled);
    }
};

class MenuImplGtk : public Menu {
public:
    Gtk::Menu   gtkMenu;
    std::vector<std::shared_ptr<MenuItemImplGtk>>   menuItems;
    std::vector<std::shared_ptr<MenuImplGtk>>       subMenus;

    MenuItemRef AddItem(const std::string &label,
                        std::function<void()> onTrigger = NULL) override {
        auto menuItem = std::make_shared<MenuItemImplGtk>();
        menuItems.push_back(menuItem);

        menuItem->gtkMenuItem.set_label(PrepareMnemonics(label));
        menuItem->gtkMenuItem.set_use_underline(true);
        menuItem->gtkMenuItem.show();
        menuItem->onTrigger = onTrigger;
        gtkMenu.append(menuItem->gtkMenuItem);

        return menuItem;
    }

    MenuRef AddSubMenu(const std::string &label) override {
        auto menuItem = std::make_shared<MenuItemImplGtk>();
        menuItems.push_back(menuItem);

        auto subMenu = std::make_shared<MenuImplGtk>();
        subMenus.push_back(subMenu);

        menuItem->gtkMenuItem.set_label(PrepareMnemonics(label));
        menuItem->gtkMenuItem.set_use_underline(true);
        menuItem->gtkMenuItem.set_submenu(subMenu->gtkMenu);
        menuItem->gtkMenuItem.show_all();
        gtkMenu.append(menuItem->gtkMenuItem);

        return subMenu;
    }

    void AddSeparator() override {
        Gtk::SeparatorMenuItem *gtkMenuItem = Gtk::manage(new Gtk::SeparatorMenuItem());
        gtkMenuItem->show();
        gtkMenu.append(*Gtk::manage(gtkMenuItem));
    }

    void PopUp() override {
        Glib::RefPtr<Glib::MainLoop> loop = Glib::MainLoop::create();
        auto signal = gtkMenu.signal_deactivate().connect([&]() { loop->quit(); });

        gtkMenu.show_all();
        gtkMenu.popup(0, GDK_CURRENT_TIME);
        loop->run();
        signal.disconnect();
    }

    void Clear() override {
        gtkMenu.foreach([&](Gtk::Widget &w) { gtkMenu.remove(w); });
        menuItems.clear();
        subMenus.clear();
    }
};

MenuRef CreateMenu() {
    return std::make_shared<MenuImplGtk>();
}

class MenuBarImplGtk : public MenuBar {
public:
    Gtk::MenuBar    gtkMenuBar;
    std::vector<std::shared_ptr<MenuImplGtk>>       subMenus;

    MenuRef AddSubMenu(const std::string &label) override {
        auto subMenu = std::make_shared<MenuImplGtk>();
        subMenus.push_back(subMenu);

        Gtk::MenuItem *gtkMenuItem = Gtk::manage(new Gtk::MenuItem);
        gtkMenuItem->set_label(PrepareMnemonics(label));
        gtkMenuItem->set_use_underline(true);
        gtkMenuItem->set_submenu(subMenu->gtkMenu);
        gtkMenuItem->show_all();
        gtkMenuBar.append(*gtkMenuItem);

        return subMenu;
    }

    void Clear() override {
        gtkMenuBar.foreach([&](Gtk::Widget &w) { gtkMenuBar.remove(w); });
        subMenus.clear();
    }
};

MenuBarRef GetOrCreateMainMenu(bool *unique) {
    *unique = false;
    return std::make_shared<MenuBarImplGtk>();
}

//-----------------------------------------------------------------------------
// GTK GL and window extensions
//-----------------------------------------------------------------------------

class GtkGLWidget : public Gtk::GLArea {
    Window *_receiver;

public:
    GtkGLWidget(Platform::Window *receiver) : _receiver(receiver) {
        set_has_depth_buffer(true);
        set_can_focus(true);
        set_events(Gdk::POINTER_MOTION_MASK |
                   Gdk::BUTTON_PRESS_MASK |
                   Gdk::BUTTON_RELEASE_MASK |
                   Gdk::BUTTON_MOTION_MASK |
                   Gdk::SCROLL_MASK |
                   Gdk::LEAVE_NOTIFY_MASK |
                   Gdk::KEY_PRESS_MASK |
                   Gdk::KEY_RELEASE_MASK);
    }

protected:
    // Work around a bug fixed in GTKMM 3.22:
    // https://mail.gnome.org/archives/gtkmm-list/2016-April/msg00020.html
    Glib::RefPtr<Gdk::GLContext> on_create_context() override {
        return get_window()->create_gl_context();
    }

    bool on_render(const Glib::RefPtr<Gdk::GLContext> &context) override {
        if(_receiver->onRender) {
            _receiver->onRender();
        }
        return true;
    }

    bool process_pointer_event(MouseEvent::Type type, double x, double y,
                               guint state, guint button = 0, int scroll_delta = 0) {
        MouseEvent event = {};
        event.type = type;
        event.x = x;
        event.y = y;
        if(button == 1 || (state & GDK_BUTTON1_MASK) != 0) {
            event.button = MouseEvent::Button::LEFT;
        } else if(button == 2 || (state & GDK_BUTTON2_MASK) != 0) {
            event.button = MouseEvent::Button::MIDDLE;
        } else if(button == 3 || (state & GDK_BUTTON3_MASK) != 0) {
            event.button = MouseEvent::Button::RIGHT;
        }
        if((state & GDK_SHIFT_MASK) != 0) {
            event.shiftDown = true;
        }
        if((state & GDK_CONTROL_MASK) != 0) {
            event.controlDown = true;
        }
        if(scroll_delta != 0) {
            event.scrollDelta = scroll_delta;
        }

        if(_receiver->onMouseEvent) {
            return _receiver->onMouseEvent(event);
        }

        return false;
    }

    bool on_motion_notify_event(GdkEventMotion *gdk_event) override {
        if(process_pointer_event(MouseEvent::Type::MOTION,
                                 gdk_event->x, gdk_event->y, gdk_event->state))
            return true;

        return Gtk::GLArea::on_motion_notify_event(gdk_event);
    }

    bool on_button_press_event(GdkEventButton *gdk_event) override {
        MouseEvent::Type type;
        if(gdk_event->type == GDK_BUTTON_PRESS) {
            type = MouseEvent::Type::PRESS;
        } else if(gdk_event->type == GDK_2BUTTON_PRESS) {
            type = MouseEvent::Type::DBL_PRESS;
        } else {
            return Gtk::GLArea::on_button_press_event(gdk_event);
        }

        if(process_pointer_event(type, gdk_event->x, gdk_event->y,
                                 gdk_event->state, gdk_event->button))
            return true;

        return Gtk::GLArea::on_button_press_event(gdk_event);
    }

    bool on_button_release_event(GdkEventButton *gdk_event) override {
        if(process_pointer_event(MouseEvent::Type::RELEASE,
                                 gdk_event->x, gdk_event->y,
                                 gdk_event->state, gdk_event->button))
            return true;

        return Gtk::GLArea::on_button_release_event(gdk_event);
    }

    bool on_scroll_event(GdkEventScroll *gdk_event) override {
        int delta;
        if(gdk_event->delta_y < 0 || gdk_event->direction == GDK_SCROLL_UP) {
            delta = 1;
        } else if(gdk_event->delta_y > 0 || gdk_event->direction == GDK_SCROLL_DOWN) {
            delta = -1;
        } else {
            return false;
        }

        if(process_pointer_event(MouseEvent::Type::SCROLL_VERT,
                                 gdk_event->x, gdk_event->y,
                                 gdk_event->state, 0, delta))
            return true;

        return Gtk::GLArea::on_scroll_event(gdk_event);
    }

    bool on_leave_notify_event(GdkEventCrossing *gdk_event) override {
        if(process_pointer_event(MouseEvent::Type::LEAVE,
                                 gdk_event->x, gdk_event->y, gdk_event->state))
            return true;

        return Gtk::GLArea::on_leave_notify_event(gdk_event);
    }

    bool process_key_event(KeyboardEvent::Type type, GdkEventKey *gdk_event) {
        KeyboardEvent event = {};
        event.type = type;

        if(gdk_event->state & ~(GDK_SHIFT_MASK|GDK_CONTROL_MASK)) {
            return false;
        }

        event.shiftDown   = (gdk_event->state & GDK_SHIFT_MASK)   != 0;
        event.controlDown = (gdk_event->state & GDK_CONTROL_MASK) != 0;

        char32_t chr = gdk_keyval_to_unicode(gdk_keyval_to_lower(gdk_event->keyval));
        if(chr != 0) {
            event.key = KeyboardEvent::Key::CHARACTER;
            event.chr = chr;
        } else if(gdk_event->keyval >= GDK_KEY_F1 &&
                  gdk_event->keyval <= GDK_KEY_F12) {
            event.key = KeyboardEvent::Key::FUNCTION;
            event.num = gdk_event->keyval - GDK_KEY_F1 + 1;
        } else {
            return false;
        }

        if(SS.GW.KeyboardEvent(event)) {
            return true;
        }

        return false;
    }

    bool on_key_press_event(GdkEventKey *gdk_event) override {
        if(process_key_event(KeyboardEvent::Type::PRESS, gdk_event))
            return true;

        return Gtk::GLArea::on_key_press_event(gdk_event);
    }

    bool on_key_release_event(GdkEventKey *gdk_event) override {
        if(process_key_event(KeyboardEvent::Type::RELEASE, gdk_event))
            return true;

        return Gtk::GLArea::on_key_release_event(gdk_event);
    }
};

class GtkEditorOverlay : public Gtk::Fixed {
    Window      *_receiver;
    GtkGLWidget _gl_widget;
    Gtk::Entry  _entry;

public:
    GtkEditorOverlay(Platform::Window *receiver) : _receiver(receiver), _gl_widget(receiver) {
        add(_gl_widget);

        _entry.set_no_show_all(true);
        _entry.set_has_frame(false);
        add(_entry);

        _entry.signal_activate().
            connect(sigc::mem_fun(this, &GtkEditorOverlay::on_activate));
    }

    bool is_editing() const {
        return _entry.is_visible();
    }

    void start_editing(int x, int y, int font_height, int min_width, bool is_monospace,
                       const std::string &val) {
        Pango::FontDescription font_desc;
        font_desc.set_family(is_monospace ? "monospace" : "normal");
        font_desc.set_absolute_size(font_height * Pango::SCALE);
        _entry.override_font(font_desc);

        // The y coordinate denotes baseline.
        Pango::FontMetrics font_metrics = get_pango_context()->get_metrics(font_desc);
        y -= font_metrics.get_ascent() / Pango::SCALE;

        Glib::RefPtr<Pango::Layout> layout = Pango::Layout::create(get_pango_context());
        layout->set_font_description(font_desc);
        // Add one extra char width to avoid scrolling.
        layout->set_text(val + " ");
        int width = layout->get_logical_extents().get_width();

        Gtk::Border margin  = _entry.get_style_context()->get_margin();
        Gtk::Border border  = _entry.get_style_context()->get_border();
        Gtk::Border padding = _entry.get_style_context()->get_padding();
        move(_entry,
             x - margin.get_left() - border.get_left() - padding.get_left(),
             y - margin.get_top()  - border.get_top()  - padding.get_top());

        int fitWidth = width / Pango::SCALE + padding.get_left() + padding.get_right();
        _entry.set_size_request(max(fitWidth, min_width), -1);
        queue_resize();

        _entry.set_text(val);

        if(!_entry.is_visible()) {
            _entry.show();
            _entry.grab_focus();

            // We grab the input for ourselves and not the entry to still have
            // the pointer events go through the underlay.
            add_modal_grab();
        }
    }

    void stop_editing() {
        if(_entry.is_visible()) {
            remove_modal_grab();
            _entry.hide();
            _gl_widget.grab_focus();
        }
    }

    GtkGLWidget &get_gl_widget() {
        return _gl_widget;
    }

protected:
    bool on_key_press_event(GdkEventKey *gdk_event) override {
        if(is_editing()) {
            if(gdk_event->keyval == GDK_KEY_Escape) {
                stop_editing();
            } else {
                _entry.event((GdkEvent *)gdk_event);
            }
            return true;
        }

        return Gtk::Fixed::on_key_press_event(gdk_event);
    }

    bool on_key_release_event(GdkEventKey *gdk_event) override {
        if(is_editing()) {
            _entry.event((GdkEvent *)gdk_event);
            return true;
        }

        return Gtk::Fixed::on_key_release_event(gdk_event);
    }

    void on_size_allocate(Gtk::Allocation& allocation) override {
        Gtk::Fixed::on_size_allocate(allocation);

        _gl_widget.size_allocate(allocation);

        int width, height, min_height, natural_height;
        _entry.get_size_request(width, height);
        _entry.get_preferred_height(min_height, natural_height);

        Gtk::Allocation entry_rect = _entry.get_allocation();
        entry_rect.set_width(width);
        entry_rect.set_height(natural_height);
        _entry.size_allocate(entry_rect);
    }

    void on_activate() {
        if(_receiver->onEditingDone) {
            _receiver->onEditingDone(_entry.get_text());
        }
    }
};

class GtkWindow : public Gtk::Window {
    Platform::Window   *_receiver;
    Gtk::VBox           _vbox;
    Gtk::MenuBar       *_menu_bar;
    Gtk::HBox           _hbox;
    GtkEditorOverlay    _editor_overlay;
    Gtk::VScrollbar     _scrollbar;
    bool                _is_fullscreen;

public:
    GtkWindow(Platform::Window *receiver) :
            _receiver(receiver), _menu_bar(NULL), _editor_overlay(receiver) {
        _hbox.pack_start(_editor_overlay, /*expand=*/true, /*fill=*/true);
        _hbox.pack_end(_scrollbar, /*expand=*/false, /*fill=*/false);
        _vbox.pack_end(_hbox, /*expand=*/true, /*fill=*/true);
        add(_vbox);

        _vbox.show();
        _hbox.show();
        _editor_overlay.show();
        get_gl_widget().show();

        _scrollbar.get_adjustment()->signal_value_changed().
            connect(sigc::mem_fun(this, &GtkWindow::on_scrollbar_value_changed));
    }

    bool is_full_screen() const {
        return _is_fullscreen;
    }

    Gtk::MenuBar *get_menu_bar() const {
        return _menu_bar;
    }

    void set_menu_bar(Gtk::MenuBar *menu_bar) {
        if(_menu_bar) {
            _vbox.remove(*_menu_bar);
        }
        _menu_bar = menu_bar;
        if(_menu_bar) {
            _menu_bar->show_all();
            _vbox.pack_start(*_menu_bar, /*expand=*/false, /*fill=*/false);
        }
    }

    GtkEditorOverlay &get_editor_overlay() {
        return _editor_overlay;
    }

    GtkGLWidget &get_gl_widget() {
        return _editor_overlay.get_gl_widget();
    }

    Gtk::VScrollbar &get_scrollbar() {
        return _scrollbar;
    }

protected:
    bool on_delete_event(GdkEventAny* gdk_event) {
        if(_receiver->onClose) {
            _receiver->onClose();
            return true;
        }

        return false;
    }

    bool on_window_state_event(GdkEventWindowState *gdk_event) override {
        _is_fullscreen = gdk_event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN;
        if(_receiver->onFullScreen) {
            _receiver->onFullScreen(_is_fullscreen);
        }

        return Gtk::Window::on_window_state_event(gdk_event);
    }

    void on_scrollbar_value_changed() {
        if(_receiver->onScrollbarAdjusted) {
            _receiver->onScrollbarAdjusted(_scrollbar.get_adjustment()->get_value());
        }
    }
};

//-----------------------------------------------------------------------------
// Windows
//-----------------------------------------------------------------------------

class WindowImplGtk : public Window {
public:
    GtkWindow       gtkWindow;
    MenuBarRef      menuBar;

    WindowImplGtk(Window::Kind kind) : gtkWindow(this) {
        switch(kind) {
            case Kind::TOPLEVEL:
                break;

            case Kind::TOOL:
                gtkWindow.set_type_hint(Gdk::WINDOW_TYPE_HINT_UTILITY);
                gtkWindow.set_skip_taskbar_hint(true);
                gtkWindow.set_skip_pager_hint(true);
                break;
        }

        auto icon = LoadPng("freedesktop/solvespace-48x48.png");
        auto gdkIcon =
            Gdk::Pixbuf::create_from_data(&icon->data[0], Gdk::COLORSPACE_RGB,
                                          icon->format == Pixmap::Format::RGBA, 8,
                                          icon->width, icon->height, icon->stride);
        gtkWindow.set_icon(gdkIcon->copy());
    }

    double GetPixelDensity() override {
        return gtkWindow.get_screen()->get_resolution();
    }

    int GetDevicePixelRatio() override {
        return gtkWindow.get_scale_factor();
    }

    bool IsVisible() override {
        return gtkWindow.is_visible();
    }

    void SetVisible(bool visible) override {
        if(visible) {
            gtkWindow.show();
        } else {
            gtkWindow.hide();
        }
    }

    void Focus() override {
        gtkWindow.present();
    }

    bool IsFullScreen() override {
        return gtkWindow.is_full_screen();
    }

    void SetFullScreen(bool fullScreen) override {
        if(fullScreen) {
            gtkWindow.fullscreen();
        } else {
            gtkWindow.unfullscreen();
        }
    }

    void SetTitle(const std::string &title) override {
        gtkWindow.set_title(PrepareTitle(title));
    }

    void SetMenuBar(MenuBarRef newMenuBar) override {
        if(newMenuBar) {
            Gtk::MenuBar *gtkMenuBar = &((MenuBarImplGtk*)&*newMenuBar)->gtkMenuBar;
            gtkWindow.set_menu_bar(gtkMenuBar);
        } else {
            gtkWindow.set_menu_bar(NULL);
        }
        menuBar = newMenuBar;
    }

    void GetContentSize(double *width, double *height) override {
        *width  = gtkWindow.get_gl_widget().get_allocated_width();
        *height = gtkWindow.get_gl_widget().get_allocated_height();
    }

    void SetMinContentSize(double width, double height) override {
        gtkWindow.get_gl_widget().set_size_request(width, height);
    }

    void FreezePosition(SettingsRef settings, const std::string &key) override {
        if(!gtkWindow.is_visible()) return;

        int left, top, width, height;
        gtkWindow.get_position(left, top);
        gtkWindow.get_size(width, height);
        bool isMaximized = gtkWindow.is_maximized();

        settings->FreezeInt(key + "_Left",       left);
        settings->FreezeInt(key + "_Top",        top);
        settings->FreezeInt(key + "_Width",      width);
        settings->FreezeInt(key + "_Height",     height);
        settings->FreezeBool(key + "_Maximized", isMaximized);
    }

    void ThawPosition(SettingsRef settings, const std::string &key) override {
        int left, top, width, height;
        gtkWindow.get_position(left, top);
        gtkWindow.get_size(width, height);

        left   = settings->ThawInt(key + "_Left",   left);
        top    = settings->ThawInt(key + "_Top",    top);
        width  = settings->ThawInt(key + "_Width",  width);
        height = settings->ThawInt(key + "_Height", height);

        gtkWindow.move(left, top);
        gtkWindow.resize(width, height);

        if(settings->ThawBool(key + "_Maximized", false)) {
            gtkWindow.maximize();
        }
    }

    void SetCursor(Cursor cursor) override {
        Gdk::CursorType gdkCursorType;
        switch(cursor) {
            case Cursor::POINTER: gdkCursorType = Gdk::ARROW; break;
            case Cursor::HAND:    gdkCursorType = Gdk::HAND1; break;
        }

        auto gdkWindow = gtkWindow.get_gl_widget().get_window();
        if(gdkWindow) {
            gdkWindow->set_cursor(Gdk::Cursor::create(gdkCursorType));
        }
    }

    void SetTooltip(const std::string &text) override {
        if(text.empty()) {
            gtkWindow.get_gl_widget().set_has_tooltip(false);
        } else {
            gtkWindow.get_gl_widget().set_tooltip_text(text);
        }
    }

    bool IsEditorVisible() override {
        return gtkWindow.get_editor_overlay().is_editing();
    }

    void ShowEditor(double x, double y, double fontHeight, double minWidth,
                    bool isMonospace, const std::string &text) override {
        gtkWindow.get_editor_overlay().start_editing(
            x, y, fontHeight, minWidth, isMonospace, text);
    }

    void HideEditor() override {
        gtkWindow.get_editor_overlay().stop_editing();
    }

    void SetScrollbarVisible(bool visible) override {
        if(visible) {
            gtkWindow.get_scrollbar().show();
        } else {
            gtkWindow.get_scrollbar().hide();
        }
    }

    void ConfigureScrollbar(double min, double max, double pageSize) override {
        auto adjustment = gtkWindow.get_scrollbar().get_adjustment();
        adjustment->configure(adjustment->get_value(), min, max, 1, 4, pageSize);
    }

    double GetScrollbarPosition() override {
        return gtkWindow.get_scrollbar().get_adjustment()->get_value();
    }

    void SetScrollbarPosition(double pos) override {
        return gtkWindow.get_scrollbar().get_adjustment()->set_value(pos);
    }

    void Invalidate() override {
        gtkWindow.get_gl_widget().queue_render();
    }

    void Redraw() override {
        Invalidate();
        Gtk::Main::iteration(/*blocking=*/false);
    }

    void *NativePtr() override {
        return &gtkWindow;
    }
};

WindowRef CreateWindow(Window::Kind kind, WindowRef parentWindow) {
    auto window = std::make_shared<WindowImplGtk>(kind);
    if(parentWindow) {
        window->gtkWindow.set_transient_for(
            std::static_pointer_cast<WindowImplGtk>(parentWindow)->gtkWindow);
    }
    return window;
}

//-----------------------------------------------------------------------------
// 3DConnexion support
//-----------------------------------------------------------------------------

void Open3DConnexion() {}
void Close3DConnexion() {}

#if defined(HAVE_SPACEWARE) && defined(GDK_WINDOWING_X11)
static GdkFilterReturn GdkSpnavFilter(GdkXEvent *gdkXEvent, GdkEvent *gdkEvent, gpointer data) {
    XEvent *xEvent = (XEvent *)gdkXEvent;
    WindowImplGtk *window = (WindowImplGtk *)data;

    spnav_event spnavEvent;
    if(!spnav_x11_event(xEvent, &spnavEvent)) {
        return GDK_FILTER_CONTINUE;
    }

    switch(spnavEvent.type) {
        case SPNAV_EVENT_MOTION: {
            SixDofEvent event = {};
            event.type = SixDofEvent::Type::MOTION;
            event.translationX = (double)spnavEvent.motion.x;
            event.translationY = (double)spnavEvent.motion.y;
            event.translationZ = (double)spnavEvent.motion.z  * -1.0;
            event.rotationX    = (double)spnavEvent.motion.rx *  0.001;
            event.rotationY    = (double)spnavEvent.motion.ry *  0.001;
            event.rotationZ    = (double)spnavEvent.motion.rz * -0.001;
            event.shiftDown    = xEvent->xmotion.state & ShiftMask;
            event.controlDown  = xEvent->xmotion.state & ControlMask;
            if(window->onSixDofEvent) {
                window->onSixDofEvent(event);
            }
            break;
        }

        case SPNAV_EVENT_BUTTON:
            SixDofEvent event = {};
            if(spnavEvent.button.press) {
                event.type = SixDofEvent::Type::PRESS;
            } else {
                event.type = SixDofEvent::Type::RELEASE;
            }
            switch(spnavEvent.button.bnum) {
                case 0:  event.button = SixDofEvent::Button::FIT; break;
                default: return GDK_FILTER_REMOVE;
            }
            event.shiftDown   = xEvent->xmotion.state & ShiftMask;
            event.controlDown = xEvent->xmotion.state & ControlMask;
            if(window->onSixDofEvent) {
                window->onSixDofEvent(event);
            }
            break;
    }

    return GDK_FILTER_REMOVE;
}

void Request3DConnexionEventsForWindow(WindowRef window) {
    std::shared_ptr<WindowImplGtk> windowImpl =
        std::static_pointer_cast<WindowImplGtk>(window);

    Glib::RefPtr<Gdk::Window> gdkWindow = windowImpl->gtkWindow.get_window();
    if(GDK_IS_X11_DISPLAY(gdkWindow->get_display()->gobj())) {
        gdkWindow->add_filter(GdkSpnavFilter, windowImpl.get());
        spnav_x11_open(gdk_x11_get_default_xdisplay(),
                       gdk_x11_window_get_xid(gdkWindow->gobj()));
    }
}
#else
void Request3DConnexionEventsForWindow(WindowRef window) {}
#endif

//-----------------------------------------------------------------------------
// Message dialogs
//-----------------------------------------------------------------------------

class MessageDialogImplGtk : public MessageDialog {
public:
    Gtk::Image         gtkImage;
    Gtk::MessageDialog gtkDialog;

    MessageDialogImplGtk(Gtk::Window &parent)
        : gtkDialog(parent, "", /*use_markup=*/false, Gtk::MESSAGE_INFO,
                    Gtk::BUTTONS_NONE, /*modal=*/true)
    {
         SetTitle("Message");
    }

    void SetType(Type type) override {
        switch(type) {
            case Type::INFORMATION:
                gtkImage.set_from_icon_name("dialog-information", Gtk::ICON_SIZE_DIALOG);
                break;

            case Type::QUESTION:
                gtkImage.set_from_icon_name("dialog-question", Gtk::ICON_SIZE_DIALOG);
                break;

            case Type::WARNING:
                gtkImage.set_from_icon_name("dialog-warning", Gtk::ICON_SIZE_DIALOG);
                break;

            case Type::ERROR:
                gtkImage.set_from_icon_name("dialog-error", Gtk::ICON_SIZE_DIALOG);
                break;
        }
        gtkDialog.set_image(gtkImage);
    }

    void SetTitle(std::string title) override {
        gtkDialog.set_title(PrepareTitle(title));
    }

    void SetMessage(std::string message) override {
        gtkDialog.set_message(message);
    }

    void SetDescription(std::string description) override {
        gtkDialog.set_secondary_text(description);
    }

    void AddButton(std::string name, Response response, bool isDefault) override {
        int responseId;
        switch(response) {
            case Response::NONE:   ssassert(false, "Invalid response");
            case Response::OK:     responseId = Gtk::RESPONSE_OK;     break;
            case Response::YES:    responseId = Gtk::RESPONSE_YES;    break;
            case Response::NO:     responseId = Gtk::RESPONSE_NO;     break;
            case Response::CANCEL: responseId = Gtk::RESPONSE_CANCEL; break;
        }
        gtkDialog.add_button(PrepareMnemonics(name), responseId);
        if(isDefault) {
            gtkDialog.set_default_response(responseId);
        }
    }

    Response RunModal() override {
        switch(gtkDialog.run()) {
            case Gtk::RESPONSE_OK:     return Response::OK;     break;
            case Gtk::RESPONSE_YES:    return Response::YES;    break;
            case Gtk::RESPONSE_NO:     return Response::NO;     break;
            case Gtk::RESPONSE_CANCEL: return Response::CANCEL; break;

            case Gtk::RESPONSE_NONE:
            case Gtk::RESPONSE_CLOSE:
            case Gtk::RESPONSE_DELETE_EVENT:
                return Response::NONE;
                break;

            default: ssassert(false, "Unexpected response");
        }
    }
};

MessageDialogRef CreateMessageDialog(WindowRef parentWindow) {
    return std::make_shared<MessageDialogImplGtk>(
                std::static_pointer_cast<WindowImplGtk>(parentWindow)->gtkWindow);
}

//-----------------------------------------------------------------------------
// File dialogs
//-----------------------------------------------------------------------------

class FileDialogImplGtk : public FileDialog {
public:
    Gtk::FileChooserDialog      gtkDialog;
    std::vector<std::string>    extensions;

    FileDialogImplGtk(Gtk::FileChooserDialog &&dialog)
        : gtkDialog(std::move(dialog))
    {
        gtkDialog.property_filter().signal_changed().
            connect(sigc::mem_fun(this, &FileDialogImplGtk::FilterChanged));
    }

    void SetTitle(std::string title) override {
        gtkDialog.set_title(PrepareTitle(title));
    }

    void SetCurrentName(std::string name) override {
        gtkDialog.set_current_name(name);
    }

    Platform::Path GetFilename() override {
        return Path::From(gtkDialog.get_filename());
    }

    void SetFilename(Platform::Path path) override {
        gtkDialog.set_filename(path.raw);
    }

    void AddFilter(std::string name, std::vector<std::string> extensions) override {
        Glib::RefPtr<Gtk::FileFilter> gtkFilter = Gtk::FileFilter::create();
        Glib::ustring desc;
        for(auto extension : extensions) {
            Glib::ustring pattern = "*";
            if(!extension.empty()) {
                pattern = "*." + extension;
                gtkFilter->add_pattern(pattern);
                gtkFilter->add_pattern(Glib::ustring(pattern).uppercase());
            }
            if(!desc.empty()) {
                desc += ", ";
            }
            desc += pattern;
        }
        gtkFilter->set_name(name + " (" + desc + ")");

        this->extensions.push_back(extensions.front());
        gtkDialog.add_filter(gtkFilter);
    }

    std::string GetExtension() {
        auto filters = gtkDialog.list_filters();
        size_t filterIndex =
            std::find(filters.begin(), filters.end(), gtkDialog.get_filter()) -
            filters.begin();
        if(filterIndex < extensions.size()) {
            return extensions[filterIndex];
        } else {
            return extensions.front();
        }
    }

    void SetExtension(std::string extension) {
        auto filters = gtkDialog.list_filters();
        size_t extensionIndex =
            std::find(extensions.begin(), extensions.end(), extension) -
            extensions.begin();
        if(extensionIndex < filters.size()) {
            gtkDialog.set_filter(filters[extensionIndex]);
        } else {
            gtkDialog.set_filter(filters.front());
        }
    }

    void FilterChanged() {
        std::string extension = GetExtension();
        if(extension == "") return;

        Platform::Path path = GetFilename();
        SetCurrentName(path.WithExtension(extension).FileName());
    }

    void FreezeChoices(SettingsRef settings, const std::string &key) override {
        settings->FreezeString("Dialog_" + key + "_Folder",
                               gtkDialog.get_current_folder());
        settings->FreezeString("Dialog_" + key + "_Filter", GetExtension());
    }

    void ThawChoices(SettingsRef settings, const std::string &key) override {
        gtkDialog.set_current_folder(settings->ThawString("Dialog_" + key + "_Folder"));
        SetExtension(settings->ThawString("Dialog_" + key + "_Filter"));
    }

    bool RunModal() override {
        if(gtkDialog.get_action() == Gtk::FILE_CHOOSER_ACTION_SAVE &&
                Path::From(gtkDialog.get_current_name()).FileStem().empty()) {
            gtkDialog.set_current_name(std::string(_("untitled")) + "." + GetExtension());
        }

        if(gtkDialog.run() == Gtk::RESPONSE_OK) {
            return true;
        } else {
            return false;
        }
    }
};

FileDialogRef CreateOpenFileDialog(WindowRef parentWindow) {
    Gtk::Window &gtkParent = std::static_pointer_cast<WindowImplGtk>(parentWindow)->gtkWindow;
    Gtk::FileChooserDialog gtkDialog(gtkParent, C_("title", "Open File"),
                                     Gtk::FILE_CHOOSER_ACTION_OPEN);
    gtkDialog.add_button(C_("button", "_Cancel"), Gtk::RESPONSE_CANCEL);
    gtkDialog.add_button(C_("button", "_Open"), Gtk::RESPONSE_OK);
    gtkDialog.set_default_response(Gtk::RESPONSE_OK);
    return std::make_shared<FileDialogImplGtk>(std::move(gtkDialog));

}

FileDialogRef CreateSaveFileDialog(WindowRef parentWindow) {
    Gtk::Window &gtkParent = std::static_pointer_cast<WindowImplGtk>(parentWindow)->gtkWindow;
    Gtk::FileChooserDialog gtkDialog(gtkParent, C_("title", "Save File"),
                                     Gtk::FILE_CHOOSER_ACTION_SAVE);
    gtkDialog.set_do_overwrite_confirmation(true);
    gtkDialog.add_button(C_("button", "_Cancel"), Gtk::RESPONSE_CANCEL);
    gtkDialog.add_button(C_("button", "_Save"), Gtk::RESPONSE_OK);
    gtkDialog.set_default_response(Gtk::RESPONSE_OK);
    return std::make_shared<FileDialogImplGtk>(std::move(gtkDialog));
}

//-----------------------------------------------------------------------------
// Application-wide APIs
//-----------------------------------------------------------------------------

void Exit() {
    Gtk::Main::quit();
}

}
}