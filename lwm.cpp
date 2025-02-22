/*******************************************************************************
 * LWM Minimal No-Decoration Floating WM using XCB (Refactored)
 *
 * Features:
 *  - Alt+Mouse Left/Right for window move/resize (with edge snapping).
 *  - Alt+F toggles fullscreen.
 *  - Alt+E closes focused window (sends WM_DELETE_WINDOW if available).
 *  - Alt+Q shows an exit confirmation dialog.
 *  - Alt+R shows a "Runner" prompt (with a larger font).
 *  - Alt+Tab cycles through windows.
 *  - Alt+I shows a help dialog with key bindings.
 *  - Alt+M minimizes a window.
 *  - Alt+N restores all minimized windows.
 *  - Focus follows mouse.
 *
 * Uncomment #define FOCUS_FOLLOWS_MOUSE for sloppy focus.
 ******************************************************************************/

#include <xcb/xcb.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_ewmh.h>
#include <X11/keysym.h>  // for XK_ constants
#include <signal.h>

#include <fstream>
#include <map>
#include <vector>
#include <queue>
#include <set>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <iostream>
#include <optional>
#include <memory>

// Uncomment to enable debug logs:
//#define DEBUG_LOGS

// Uncomment if you want “focus follows mouse” (sloppy focus):
#define FOCUS_FOLLOWS_MOUSE

/*******************************************************************************
 * CONSTANTS (colors, fonts, snapping threshold)
 ******************************************************************************/
#define BACKGROUND_COLOR 0x2E3440   // Dark background for dialogs
#define FOREGROUND_COLOR 0xFFFFFF   // White text
#define HELP_BG_COLOR    0x000000   // Black background for help dialog

// Font names for dialogs:
#define DEFAULT_FONT "9x15"
#define RUNNER_FONT  "10x20"         // Bigger font for Runner dialog

// Runner dialog dimensions
static constexpr uint16_t RUNNER_WIDTH  = 300;
static constexpr uint16_t RUNNER_HEIGHT = 50;

// Snapping threshold (in pixels)
static constexpr int SNAP_THRESHOLD = 10;

/*******************************************************************************
 * RAII wrappers for XCB replies
 ******************************************************************************/
template<typename T>
struct XCBReplyDeleter {
    void operator()(T* ptr) const { free(ptr); }
};

template<typename T>
using UniqueXCBReply = std::unique_ptr<T, XCBReplyDeleter<T>>;

/*******************************************************************************
 * Logger Class
 *
 * Encapsulates logging into a dedicated thread.
 ******************************************************************************/
class Logger {
public:
    Logger(const std::string &logFilePath) : m_logFilePath(logFilePath), m_active(true) {
        m_loggerThread = std::thread(&Logger::logWorker, this);
    }

    ~Logger() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_active = false;
        }
        m_cv.notify_all();
        if (m_loggerThread.joinable())
            m_loggerThread.join();
    }

    void log(const std::string &msg) {
#ifndef DEBUG_LOGS
        (void)msg; // Mark msg as unused when debug logs are disabled.
#else
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(msg);
        m_cv.notify_one();
#endif
    }

private:
    void logWorker() {
        std::ofstream logFile(m_logFilePath, std::ios::out | std::ios::app);
        if (!logFile.is_open()) return;
        while (true) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return !m_queue.empty() || !m_active; });
            while (!m_queue.empty()) {
                logFile << m_queue.front() << std::endl;
                m_queue.pop();
            }
            if (!m_active && m_queue.empty())
                break;
        }
        logFile.close();
    }

    std::string m_logFilePath;
    std::queue<std::string> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_active;
    std::thread m_loggerThread;
};

/*******************************************************************************
 * WindowManager (WM) class
 ******************************************************************************/
class WM {
public:
    WM(Logger &logger) : m_logger(logger) {}
    ~WM() = default;

    bool initialize();
    void runEventLoop();
    void cleanup();

    void focusWindow(xcb_window_t w);
    void focusNextWindow();

private:
    // Internal helpers
    bool setupEWMH();
    void setupAtoms();
    void setupCursor();
    void selectInputOnRoot();
    void grabKeysAndButtons();
    void setupSupportingWMCheck();
    void resetFocus(); // reset input focus to a valid window

    // Helper to draw text in a window (used for dialogs)
    void drawText(xcb_window_t win, const char* fontName, const char* text,
                  int x, int y, uint32_t fgColor, uint32_t bgColor);

    // Event handlers
    void handleKeyPress(xcb_key_press_event_t *ev);
    void handleButtonPress(xcb_button_press_event_t *ev);
    void handleMotionNotify(xcb_motion_notify_event_t *ev);
    void handleButtonRelease(xcb_button_release_event_t *ev);
    void handleMapRequest(xcb_map_request_event_t *mr);
    void handleDestroyNotify(xcb_destroy_notify_event_t *dn);
    void handleUnmapNotify(xcb_unmap_notify_event_t *un);
    void handleConfigureRequest(xcb_configure_request_event_t *cr);
    void handleExpose(xcb_expose_event_t *ev);
    void handleClientMessage(xcb_client_message_event_t *cm);
#ifdef FOCUS_FOLLOWS_MOUSE
    void handleEnterNotify(xcb_enter_notify_event_t *ev);
#endif

    // Fullscreen toggle
    void toggleFullscreen(xcb_window_t w);

    // Runner, Exit, and Help dialogs
    void createPopUpWindow(const char* title,
                           xcb_window_t &winVar,
                           uint16_t width, uint16_t height,
                           bool &activeFlag);
    void destroyPopUpWindow(xcb_window_t &winVar, bool &activeFlag);

    void createExitConfirmationDialog();
    void destroyExitConfirmationDialog();
    void handleExitConfirmationKeypress(xcb_keysym_t ks);

    void createRunnerDialog();
    void destroyRunnerDialog();
    void redrawRunnerDialog();
    void handleRunnerInput(xcb_keysym_t ks);
    void executeCommand(const std::string &cmd);

    // Help dialog functions
    void createHelpDialog();
    void destroyHelpDialog();

    // Move/Resize structures
    struct MoveStart {
        xcb_window_t window = XCB_NONE;
        int start_x = 0;
        int start_y = 0;
        int orig_x  = 0;
        int orig_y  = 0;
    } moveStart;

    struct ResizeStart {
        xcb_window_t window = XCB_NONE;
        int start_x = 0;
        int start_y = 0;
        uint16_t start_width  = 0;
        uint16_t start_height = 0;
    } resizeStart;

private:
    xcb_connection_t       *m_conn   = nullptr;
    xcb_screen_t           *m_screen = nullptr;
    xcb_ewmh_connection_t   m_ewmh;
    xcb_cursor_t            m_cursor = XCB_CURSOR_NONE;
    xcb_key_symbols_t      *m_keysyms= nullptr;

    int m_screenWidth  = 0;
    int m_screenHeight = 0;

    std::vector<xcb_window_t> m_windowList;
    size_t m_currentWindowIndex = 0;

    // Runner dialog state
    bool         m_isRunnerActive          = false;
    xcb_window_t m_runnerWindow            = XCB_NONE;
    std::string  m_runnerInput;

    // Exit confirmation dialog state
    bool         m_isExitConfirmationActive = false;
    xcb_window_t m_exitConfirmationWindow   = XCB_NONE;

    // Help dialog state
    bool         m_isHelpActive = false;
    xcb_window_t m_helpWindow   = XCB_NONE;

    // For storing window geometries
    struct WindowGeometry {
        int      x;
        int      y;
        uint16_t width;
        uint16_t height;
    };
    std::map<xcb_window_t, WindowGeometry> m_geometryCache;
    std::map<xcb_window_t, WindowGeometry> m_originalGeometry;

    // New features: minimized windows.
    std::vector<xcb_window_t> m_minimizedWindows;

    // Atoms
    xcb_atom_t WM_PROTOCOLS;
    xcb_atom_t WM_DELETE_WINDOW;
    xcb_atom_t NET_WM_STATE;
    xcb_atom_t NET_WM_STATE_FULLSCREEN;
    xcb_atom_t _NET_SUPPORTING_WM_CHECK;

private:
    xcb_keysym_t getKeysym(xcb_keycode_t code, uint16_t state);
    void invalidateGeometryCache(xcb_window_t w);
    WindowGeometry getWindowGeometry(xcb_window_t w);
    std::optional<xcb_screen_t*> setupScreen(int scrNum);
    Logger &m_logger;
};

/*******************************************************************************
 * WM IMPLEMENTATION
 ******************************************************************************/
std::optional<xcb_screen_t*> WM::setupScreen(int scrNum) {
    const xcb_setup_t *setup = xcb_get_setup(m_conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    while (scrNum-- > 0) {
        xcb_screen_next(&it);
    }
    if(it.data) return it.data;
    return std::nullopt;
}

bool WM::initialize()
{
    int scrNum;
    m_conn = xcb_connect(nullptr, &scrNum);
    if (!m_conn || xcb_connection_has_error(m_conn)) {
        m_logger.log("Failed to connect to X server.");
        return false;
    }

    auto screenOpt = setupScreen(scrNum);
    if (!screenOpt) {
        m_logger.log("No valid screen found.");
        return false;
    }
    m_screen = screenOpt.value();

    m_screenWidth  = m_screen->width_in_pixels;
    m_screenHeight = m_screen->height_in_pixels;

    if (!setupEWMH()) {
        m_logger.log("Failed to initialize EWMH.");
        return false;
    }
    setupAtoms();
    setupCursor();
    selectInputOnRoot();

    m_keysyms = xcb_key_symbols_alloc(m_conn);
    if (!m_keysyms) {
        m_logger.log("Failed to allocate keysyms.");
        return false;
    }

    grabKeysAndButtons();
    setupSupportingWMCheck();

    xcb_flush(m_conn);
    return true;
}

bool WM::setupEWMH()
{
    std::memset(&m_ewmh, 0, sizeof(m_ewmh));
    xcb_intern_atom_cookie_t *cookies = xcb_ewmh_init_atoms(m_conn, &m_ewmh);
    if (!xcb_ewmh_init_atoms_replies(&m_ewmh, cookies, nullptr)) {
        return false;
    }
    return true;
}

void WM::setupAtoms()
{
    auto get_atom = [this](const char* name) -> xcb_atom_t {
        xcb_intern_atom_cookie_t c = xcb_intern_atom(m_conn, 0, std::strlen(name), name);
        UniqueXCBReply<xcb_intern_atom_reply_t> r(xcb_intern_atom_reply(m_conn, c, nullptr));
        if (!r) return XCB_NONE;
        return r->atom;
    };

    WM_PROTOCOLS             = get_atom("WM_PROTOCOLS");
    WM_DELETE_WINDOW         = get_atom("WM_DELETE_WINDOW");
    NET_WM_STATE             = get_atom("_NET_WM_STATE");
    NET_WM_STATE_FULLSCREEN  = get_atom("_NET_WM_STATE_FULLSCREEN");
    _NET_SUPPORTING_WM_CHECK = get_atom("_NET_SUPPORTING_WM_CHECK");

    xcb_atom_t NET_SUPPORTED = get_atom("_NET_SUPPORTED");
    std::vector<xcb_atom_t> supported = {
        NET_WM_STATE,
        NET_WM_STATE_FULLSCREEN
    };
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_screen->root,
                        NET_SUPPORTED, XCB_ATOM_ATOM, 32,
                        supported.size(), supported.data());
}

void WM::setupCursor()
{
    xcb_cursor_context_t *ctx = nullptr;
    if (xcb_cursor_context_new(m_conn, m_screen, &ctx) < 0) {
        m_logger.log("Failed to create cursor context.");
        return;
    }
    m_cursor = xcb_cursor_load_cursor(ctx, "left_ptr");
    xcb_cursor_context_free(ctx);

    if (m_cursor != XCB_CURSOR_NONE) {
        xcb_change_window_attributes(m_conn, m_screen->root, XCB_CW_CURSOR, &m_cursor);
    }
    xcb_flush(m_conn);
}

void WM::selectInputOnRoot()
{
    uint32_t mask = XCB_CW_EVENT_MASK;
    uint32_t val  = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
                  | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
                  | XCB_EVENT_MASK_PROPERTY_CHANGE
                  | XCB_EVENT_MASK_BUTTON_PRESS
                  | XCB_EVENT_MASK_BUTTON_RELEASE
                  | XCB_EVENT_MASK_POINTER_MOTION
#ifdef FOCUS_FOLLOWS_MOUSE
                  | XCB_EVENT_MASK_ENTER_WINDOW
#endif
                  | XCB_EVENT_MASK_EXPOSURE;

    xcb_void_cookie_t ck = xcb_change_window_attributes_checked(m_conn, m_screen->root, mask, &val);
    xcb_generic_error_t* err = xcb_request_check(m_conn, ck);
    if (err) {
        m_logger.log("Another WM is probably running; cannot redirect the root window.");
        free(err);
    }
}

void WM::grabKeysAndButtons()
{
    const uint16_t MOD_MASK = XCB_MOD_MASK_1; // "Alt"

    xcb_keysym_t keysToGrab[] = {
        XK_f, XK_e, XK_q, XK_r, XK_Tab, XK_i, XK_m, XK_n
    };

    const uint16_t modifiers[] = {
        MOD_MASK,
        static_cast<uint16_t>(MOD_MASK | XCB_MOD_MASK_LOCK),
        static_cast<uint16_t>(MOD_MASK | XCB_MOD_MASK_2),
        static_cast<uint16_t>(MOD_MASK | XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2)
    };

    for (auto mod : modifiers) {
        for (auto ks : keysToGrab) {
            xcb_keycode_t *kc = xcb_key_symbols_get_keycode(m_keysyms, ks);
            if (!kc) continue;
            for (int i = 0; kc[i] != XCB_NO_SYMBOL; i++) {
                xcb_grab_key(m_conn, 1, m_screen->root,
                             mod, kc[i],
                             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
            }
            free(kc);
        }
        // Grab buttons for move/resize.
        xcb_grab_button(m_conn, 1, m_screen->root,
            XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
            XCB_NONE, XCB_NONE,
            1, mod);

        xcb_grab_button(m_conn, 1, m_screen->root,
            XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
            XCB_NONE, XCB_NONE,
            3, mod);
    }
    xcb_flush(m_conn);
}

void WM::setupSupportingWMCheck()
{
    xcb_window_t wmCheckWin = xcb_generate_id(m_conn);

    xcb_create_window(m_conn, m_screen->root_depth, wmCheckWin, m_screen->root,
                      -100, -100, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      m_screen->root_visual, 0, nullptr);

    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, wmCheckWin,
                        _NET_SUPPORTING_WM_CHECK, XCB_ATOM_WINDOW, 32, 1, &wmCheckWin);
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_screen->root,
                        _NET_SUPPORTING_WM_CHECK, XCB_ATOM_WINDOW, 32, 1, &wmCheckWin);

    const char* wmName = "EnhancedMinimalWM";
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, wmCheckWin,
                        m_ewmh._NET_WM_NAME, XCB_ATOM_STRING, 8,
                        std::strlen(wmName), wmName);

    xcb_map_window(m_conn, wmCheckWin);
    xcb_flush(m_conn);
}

void WM::runEventLoop()
{
    xcb_flush(m_conn);
    while (true) {
        xcb_generic_event_t *ev = xcb_wait_for_event(m_conn);
        if (!ev) break; // error or connection closed
        uint8_t rt = ev->response_type & ~0x80;

        switch (rt) {
            case XCB_KEY_PRESS:
                handleKeyPress(reinterpret_cast<xcb_key_press_event_t*>(ev));
                break;
            case XCB_BUTTON_PRESS:
                handleButtonPress(reinterpret_cast<xcb_button_press_event_t*>(ev));
                break;
            case XCB_MOTION_NOTIFY:
                handleMotionNotify(reinterpret_cast<xcb_motion_notify_event_t*>(ev));
                break;
            case XCB_BUTTON_RELEASE:
                handleButtonRelease(reinterpret_cast<xcb_button_release_event_t*>(ev));
                break;
            case XCB_MAP_REQUEST:
                handleMapRequest(reinterpret_cast<xcb_map_request_event_t*>(ev));
                break;
            case XCB_DESTROY_NOTIFY:
                handleDestroyNotify(reinterpret_cast<xcb_destroy_notify_event_t*>(ev));
                break;
            case XCB_UNMAP_NOTIFY:
                handleUnmapNotify(reinterpret_cast<xcb_unmap_notify_event_t*>(ev));
                break;
            case XCB_CONFIGURE_REQUEST:
                handleConfigureRequest(reinterpret_cast<xcb_configure_request_event_t*>(ev));
                break;
            case XCB_EXPOSE:
                handleExpose(reinterpret_cast<xcb_expose_event_t*>(ev));
                break;
            case XCB_CLIENT_MESSAGE:
                handleClientMessage(reinterpret_cast<xcb_client_message_event_t*>(ev));
                break;
#ifdef FOCUS_FOLLOWS_MOUSE
            case XCB_ENTER_NOTIFY:
                handleEnterNotify(reinterpret_cast<xcb_enter_notify_event_t*>(ev));
                break;
#endif
            default:
                m_logger.log("Unhandled event type: " + std::to_string(rt));
                break;
        }
        free(ev);
    }
}

void WM::cleanup()
{
    for (auto w : m_windowList) {
        xcb_destroy_window(m_conn, w);
    }
    xcb_flush(m_conn);

    if (m_cursor != XCB_CURSOR_NONE) {
        xcb_free_cursor(m_conn, m_cursor);
    }
    xcb_ewmh_connection_wipe(&m_ewmh);

    if (m_keysyms) {
        xcb_key_symbols_free(m_keysyms);
        m_keysyms = nullptr;
    }
    if (m_conn) {
        xcb_disconnect(m_conn);
        m_conn = nullptr;
    }
}

void WM::focusWindow(xcb_window_t w)
{
    if (!w || w == XCB_NONE) return;
    uint32_t vals[] = { XCB_STACK_MODE_ABOVE };
    xcb_configure_window(m_conn, w, XCB_CONFIG_WINDOW_STACK_MODE, vals);
    xcb_map_window(m_conn, w);
    xcb_set_input_focus(m_conn, XCB_INPUT_FOCUS_POINTER_ROOT, w, XCB_CURRENT_TIME);
    xcb_flush(m_conn);
}

void WM::focusNextWindow()
{
    if (m_windowList.empty()) return;
    size_t sz = m_windowList.size();
    for (size_t i = 0; i < sz; i++) {
        m_currentWindowIndex = (m_currentWindowIndex + 1) % sz;
        xcb_window_t w = m_windowList[m_currentWindowIndex];

        xcb_get_window_attributes_cookie_t c = xcb_get_window_attributes(m_conn, w);
        UniqueXCBReply<xcb_get_window_attributes_reply_t> ar(
            xcb_get_window_attributes_reply(m_conn, c, nullptr)
        );
        if (ar && ar->map_state == XCB_MAP_STATE_VIEWABLE) {
            focusWindow(w);
            return;
        }
    }
}

void WM::handleButtonPress(xcb_button_press_event_t *ev)
{
    // Ignore button presses on pop-up dialogs.
    if ((m_isExitConfirmationActive && ev->event == m_exitConfirmationWindow) ||
        (m_isRunnerActive && ev->event == m_runnerWindow) ||
        (m_isHelpActive && ev->event == m_helpWindow))
        return;

    bool altPressed = (ev->state & XCB_MOD_MASK_1);
    if (!altPressed) return;
    if (ev->child == XCB_NONE) return;
    xcb_window_t w = ev->child;
    auto geom = getWindowGeometry(w);
    if (ev->detail == 1) { // left button => move
        moveStart.window  = w;
        moveStart.start_x = ev->root_x;
        moveStart.start_y = ev->root_y;
        moveStart.orig_x  = geom.x;
        moveStart.orig_y  = geom.y;
    } else if (ev->detail == 3) { // right button => resize
        resizeStart.window       = w;
        resizeStart.start_x      = ev->root_x;
        resizeStart.start_y      = ev->root_y;
        resizeStart.start_width  = geom.width;
        resizeStart.start_height = geom.height;
    }
}

void WM::handleMotionNotify(xcb_motion_notify_event_t *ev)
{
    if (moveStart.window != XCB_NONE) {
        int dx = ev->root_x - moveStart.start_x;
        int dy = ev->root_y - moveStart.start_y;
        int newX = moveStart.orig_x + dx;
        int newY = moveStart.orig_y + dy;
        auto winGeom = getWindowGeometry(moveStart.window);
        if (std::abs(newX) < SNAP_THRESHOLD) newX = 0;
        if (std::abs(newY) < SNAP_THRESHOLD) newY = 0;
        if (std::abs((newX + winGeom.width) - m_screenWidth) < SNAP_THRESHOLD)
            newX = m_screenWidth - winGeom.width;
        if (std::abs((newY + winGeom.height) - m_screenHeight) < SNAP_THRESHOLD)
            newY = m_screenHeight - winGeom.height;
        uint32_t vals[2] = { static_cast<uint32_t>(newX), static_cast<uint32_t>(newY) };
        xcb_configure_window(m_conn, moveStart.window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
        invalidateGeometryCache(moveStart.window);
        xcb_flush(m_conn);
    } else if (resizeStart.window != XCB_NONE) {
        int dx = ev->root_x - resizeStart.start_x;
        int dy = ev->root_y - resizeStart.start_y;
        uint16_t nw = std::max((int)resizeStart.start_width + dx, 50);
        uint16_t nh = std::max((int)resizeStart.start_height + dy, 50);
        uint32_t vals[2] = { nw, nh };
        xcb_configure_window(m_conn, resizeStart.window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
        invalidateGeometryCache(resizeStart.window);
        xcb_flush(m_conn);
    }
}

void WM::handleButtonRelease(xcb_button_release_event_t *ev)
{
    (void)ev;
    moveStart = {};
    resizeStart = {};
}

void WM::toggleFullscreen(xcb_window_t w)
{
    if (!w) return;
    bool isFS = false;
    {
        UniqueXCBReply<xcb_get_property_reply_t> prop(
            xcb_get_property_reply(m_conn,
            xcb_get_property(m_conn, 0, w, NET_WM_STATE, XCB_ATOM_ATOM, 0, 1024),
            nullptr)
        );
        if (prop) {
            xcb_atom_t* states = static_cast<xcb_atom_t*>(xcb_get_property_value(prop.get()));
            int len = xcb_get_property_value_length(prop.get()) / sizeof(xcb_atom_t);
            for (int i = 0; i < len; i++) {
                if (states[i] == NET_WM_STATE_FULLSCREEN) {
                    isFS = true;
                    break;
                }
            }
        }
    }
    if (!isFS) {
        auto g = getWindowGeometry(w);
        m_originalGeometry[w] = g;
        xcb_atom_t add[] = { NET_WM_STATE_FULLSCREEN };
        xcb_ewmh_set_wm_state(&m_ewmh, w, 1, add);
        uint32_t vals[4] = { 0, 0, static_cast<uint32_t>(m_screenWidth), static_cast<uint32_t>(m_screenHeight) };
        xcb_configure_window(m_conn, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
    } else {
        auto it = m_originalGeometry.find(w);
        if (it != m_originalGeometry.end()) {
            auto &g = it->second;
            uint32_t vals[4] = { static_cast<uint32_t>(g.x), static_cast<uint32_t>(g.y), g.width, g.height };
            xcb_configure_window(m_conn, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
            m_originalGeometry.erase(w);
        }
        xcb_atom_t rm[] = { NET_WM_STATE_FULLSCREEN };
        xcb_ewmh_set_wm_state(&m_ewmh, w, 0, rm);
    }
    invalidateGeometryCache(w);
    xcb_flush(m_conn);
}

void WM::createPopUpWindow(const char* title, xcb_window_t &winVar,
                           uint16_t width, uint16_t height, bool &activeFlag)
{
    if (activeFlag) return;
    activeFlag = true;

    winVar = xcb_generate_id(m_conn);
    int x = (m_screenWidth - width) / 2;
    int y = (m_screenHeight - height) / 2;

    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t vals[2] = {
        BACKGROUND_COLOR,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS |
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE
    };

    xcb_create_window(m_conn, m_screen->root_depth,
                      winVar, m_screen->root,
                      x, y, width, height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      m_screen->root_visual,
                      mask, vals);

    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE,
                        winVar, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8,
                        std::strlen(title), title);

    xcb_map_window(m_conn, winVar);
    m_windowList.push_back(winVar);
    m_currentWindowIndex = m_windowList.size() - 1;
    focusWindow(winVar);
    xcb_flush(m_conn);
}

void WM::destroyPopUpWindow(xcb_window_t &winVar, bool &activeFlag)
{
    if (!activeFlag || winVar == XCB_NONE) return;
    xcb_unmap_window(m_conn, winVar);
    xcb_destroy_window(m_conn, winVar);

    auto it = std::find(m_windowList.begin(), m_windowList.end(), winVar);
    if (it != m_windowList.end()) {
        m_windowList.erase(it);
        if (m_currentWindowIndex >= m_windowList.size())
            m_currentWindowIndex = 0;
    }
    winVar   = XCB_NONE;
    activeFlag = false;
    xcb_flush(m_conn);
    resetFocus();
}

void WM::resetFocus()
{
    if (!m_windowList.empty()) {
        focusWindow(m_windowList.back());
    } else {
        xcb_set_input_focus(m_conn, XCB_INPUT_FOCUS_POINTER_ROOT, m_screen->root, XCB_CURRENT_TIME);
        xcb_flush(m_conn);
    }
}

void WM::createExitConfirmationDialog()
{
    createPopUpWindow("Confirm Exit", m_exitConfirmationWindow, 300, 100, m_isExitConfirmationActive);
}
void WM::destroyExitConfirmationDialog()
{
    destroyPopUpWindow(m_exitConfirmationWindow, m_isExitConfirmationActive);
}
void WM::handleExitConfirmationKeypress(xcb_keysym_t ks)
{
    if (ks == XK_y || ks == XK_Y)
        std::exit(0);
    else if (ks == XK_n || ks == XK_Escape)
        destroyExitConfirmationDialog();
}

void WM::createRunnerDialog()
{
    createPopUpWindow("Run Program", m_runnerWindow, RUNNER_WIDTH, RUNNER_HEIGHT, m_isRunnerActive);
    m_runnerInput.clear();
}
void WM::destroyRunnerDialog()
{
    destroyPopUpWindow(m_runnerWindow, m_isRunnerActive);
    m_runnerInput.clear();
}

void WM::createHelpDialog()
{
    createPopUpWindow("Key Bindings", m_helpWindow, 400, 240, m_isHelpActive);
}
void WM::destroyHelpDialog()
{
    destroyPopUpWindow(m_helpWindow, m_isHelpActive);
}

void WM::drawText(xcb_window_t win, const char* fontName, const char* text,
                  int x, int y, uint32_t fgColor, uint32_t bgColor)
{
    xcb_gcontext_t gc = xcb_generate_id(m_conn);
    xcb_font_t font = xcb_generate_id(m_conn);
    xcb_open_font(m_conn, font, std::strlen(fontName), fontName);
    uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT;
    uint32_t vals[3] = { fgColor, bgColor, font };
    xcb_create_gc(m_conn, gc, win, mask, vals);
    xcb_image_text_8(m_conn, std::strlen(text), win, gc, x, y, text);
    xcb_close_font(m_conn, font);
    xcb_free_gc(m_conn, gc);
    xcb_flush(m_conn);
}

void WM::handleExpose(xcb_expose_event_t *ev)
{
    xcb_window_t w = ev->window;
    if (m_isRunnerActive && w == m_runnerWindow) {
        xcb_gcontext_t bgGc = xcb_generate_id(m_conn);
        uint32_t bgMask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
        uint32_t bgVals[2] = { BACKGROUND_COLOR, BACKGROUND_COLOR };
        xcb_create_gc(m_conn, bgGc, w, bgMask, bgVals);
        xcb_rectangle_t rect = {0, 0, ev->width, ev->height};
        xcb_poly_fill_rectangle(m_conn, w, bgGc, 1, &rect);
        xcb_free_gc(m_conn, bgGc);
        int textY = RUNNER_HEIGHT / 2 + 10;
        drawText(w, RUNNER_FONT, m_runnerInput.c_str(), 10, textY, FOREGROUND_COLOR, BACKGROUND_COLOR);
    }
    else if (m_isExitConfirmationActive && w == m_exitConfirmationWindow) {
        xcb_gcontext_t bgGc = xcb_generate_id(m_conn);
        uint32_t bgMask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
        uint32_t bgVals[2] = { BACKGROUND_COLOR, BACKGROUND_COLOR };
        xcb_create_gc(m_conn, bgGc, w, bgMask, bgVals);
        xcb_rectangle_t rect = {0, 0, ev->width, ev->height};
        xcb_poly_fill_rectangle(m_conn, w, bgGc, 1, &rect);
        xcb_free_gc(m_conn, bgGc);
        const char *msg = "Exit WM? (Y/N or ESC)";
        drawText(w, DEFAULT_FONT, msg, 10, ev->height/2, FOREGROUND_COLOR, BACKGROUND_COLOR);
    }
    else if (m_isHelpActive && w == m_helpWindow) {
        xcb_gcontext_t bgGc = xcb_generate_id(m_conn);
        uint32_t bgMask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
        uint32_t bgVals[2] = { HELP_BG_COLOR, HELP_BG_COLOR };
        xcb_create_gc(m_conn, bgGc, w, bgMask, bgVals);
        xcb_rectangle_t rect = {0, 0, ev->width, ev->height};
        xcb_poly_fill_rectangle(m_conn, w, bgGc, 1, &rect);
        xcb_free_gc(m_conn, bgGc);
        const char* lines[] = {
            "Alt+F          => Toggle fullscreen",
            "Alt+E          => Close focused window",
            "Alt+Q          => Exit confirmation dialog",
            "Alt+R          => Runner prompt",
            "Alt+Tab        => Focus next window",
            "Alt+I          => Help dialog",
            "Alt+M          => Minimize window",
            "Alt+N          => Restore all minimized"
        };
        int lineCount = sizeof(lines) / sizeof(lines[0]);
        int y = 20;
        for (int i = 0; i < lineCount; i++) {
            drawText(w, DEFAULT_FONT, lines[i], 10, y, FOREGROUND_COLOR, HELP_BG_COLOR);
            y += 20;
        }
    }
}

#ifdef FOCUS_FOLLOWS_MOUSE
void WM::handleEnterNotify(xcb_enter_notify_event_t *ev)
{
    if (ev->event != XCB_NONE) {
        auto it = std::find(m_windowList.begin(), m_windowList.end(), ev->event);
        if (it != m_windowList.end()) {
            focusWindow(*it);
        }
    }
}
#endif

xcb_keysym_t WM::getKeysym(xcb_keycode_t code, uint16_t state)
{
    state &= ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2);
    int col = (state & XCB_MOD_MASK_SHIFT) ? 1 : 0;
    return xcb_key_symbols_get_keysym(m_keysyms, code, col);
}

void WM::invalidateGeometryCache(xcb_window_t w)
{
    m_geometryCache.erase(w);
}

WM::WindowGeometry WM::getWindowGeometry(xcb_window_t w)
{
    auto it = m_geometryCache.find(w);
    if (it != m_geometryCache.end()) {
        return it->second;
    }
    UniqueXCBReply<xcb_get_geometry_reply_t> geom(
        xcb_get_geometry_reply(m_conn, xcb_get_geometry(m_conn, w), nullptr)
    );
    if (!geom) {
        return {0, 0, 100, 100};
    }
    WindowGeometry wg = { geom->x, geom->y, geom->width, geom->height };
    m_geometryCache[w] = wg;
    return wg;
}

void WM::handleKeyPress(xcb_key_press_event_t *ev)
{
    if (!ev) return;
    xcb_keysym_t ks = getKeysym(ev->detail, ev->state);

    if (m_isExitConfirmationActive) {
        handleExitConfirmationKeypress(ks);
        return;
    }
    if (m_isRunnerActive) {
        handleRunnerInput(ks);
        return;
    }
    if (m_isHelpActive) {
        if (ks == XK_Escape) {
            destroyHelpDialog();
        }
        return;
    }

    bool altPressed = (ev->state & XCB_MOD_MASK_1);

    auto getFocusedWindow = [this]() -> xcb_window_t {
        xcb_get_input_focus_cookie_t ck = xcb_get_input_focus(m_conn);
        UniqueXCBReply<xcb_get_input_focus_reply_t> rp(xcb_get_input_focus_reply(m_conn, ck, nullptr));
        if (!rp) return XCB_NONE;
        return rp->focus;
    };
    xcb_window_t foc = getFocusedWindow();

    if (!altPressed) return;

    switch (ks) {
        case XK_f:
            toggleFullscreen(foc);
            break;
        case XK_e:
        {
            bool hasWMDelete = false;
            xcb_icccm_get_wm_protocols_reply_t pr;
            if (xcb_icccm_get_wm_protocols_reply(m_conn,
                    xcb_icccm_get_wm_protocols(m_conn, foc, WM_PROTOCOLS),
                    &pr, nullptr))
            {
                for (uint32_t i = 0; i < pr.atoms_len; i++) {
                    if (pr.atoms[i] == WM_DELETE_WINDOW) {
                        hasWMDelete = true;
                        break;
                    }
                }
                if (hasWMDelete) {
                    xcb_client_message_event_t cme = {};
                    cme.response_type = XCB_CLIENT_MESSAGE;
                    cme.window = foc;
                    cme.type = WM_PROTOCOLS;
                    cme.format = 32;
                    cme.data.data32[0] = WM_DELETE_WINDOW;
                    cme.data.data32[1] = XCB_CURRENT_TIME;
                    xcb_send_event(m_conn, false, foc, 0, reinterpret_cast<char*>(&cme));
                    xcb_flush(m_conn);
                }
                xcb_icccm_get_wm_protocols_reply_wipe(&pr);
            }
            if (!hasWMDelete) {
                xcb_destroy_window(m_conn, foc);
                xcb_flush(m_conn);
            }
            break;
        }
        case XK_q:
            createExitConfirmationDialog();
            break;
        case XK_r:
            createRunnerDialog();
            break;
        case XK_i:
            createHelpDialog();
            break;
        case XK_Tab:
            focusNextWindow();
            break;
        case XK_m:
            if (foc != XCB_NONE &&
                foc != m_runnerWindow &&
                foc != m_exitConfirmationWindow &&
                foc != m_helpWindow)
            {
                auto it = std::find(m_windowList.begin(), m_windowList.end(), foc);
                if (it != m_windowList.end()) {
                    m_windowList.erase(it);
                }
                m_minimizedWindows.push_back(foc);
                xcb_unmap_window(m_conn, foc);
                resetFocus();
            }
            break;
        case XK_n:
            for (xcb_window_t w : m_minimizedWindows) {
                xcb_map_window(m_conn, w);
                if (std::find(m_windowList.begin(), m_windowList.end(), w) == m_windowList.end()) {
                    m_windowList.push_back(w);
                }
            }
            m_minimizedWindows.clear();
            if (!m_windowList.empty()) {
                focusWindow(m_windowList.back());
            }
            break;
        default:
            break;
    }
}

void WM::handleMapRequest(xcb_map_request_event_t *mr)
{
    UniqueXCBReply<xcb_get_window_attributes_reply_t> attr(
        xcb_get_window_attributes_reply(m_conn, xcb_get_window_attributes(m_conn, mr->window), nullptr)
    );
    if (attr) {
        if (attr->override_redirect) {
            xcb_map_window(m_conn, mr->window);
            return;
        }
    }
    xcb_map_window(m_conn, mr->window);
    {
        uint32_t vals[] = { XCB_STACK_MODE_ABOVE };
        xcb_configure_window(m_conn, mr->window, XCB_CONFIG_WINDOW_STACK_MODE, vals);
    }
    focusWindow(mr->window);

    if (std::find(m_windowList.begin(), m_windowList.end(), mr->window) == m_windowList.end()) {
        m_windowList.push_back(mr->window);
        m_currentWindowIndex = m_windowList.size() - 1;
    }

#ifdef FOCUS_FOLLOWS_MOUSE
    {
        uint32_t enter_mask = XCB_EVENT_MASK_ENTER_WINDOW;
        xcb_change_window_attributes(m_conn, mr->window, XCB_CW_EVENT_MASK, &enter_mask);
    }
#endif

    auto g = getWindowGeometry(mr->window);
    xcb_configure_notify_event_t ce = {};
    ce.response_type = XCB_CONFIGURE_NOTIFY;
    ce.event = mr->window;
    ce.window = mr->window;
    ce.x = g.x;
    ce.y = g.y;
    ce.width = g.width;
    ce.height = g.height;
    ce.border_width = 0;
    ce.override_redirect = false;
    xcb_send_event(m_conn, false, mr->window, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<char*>(&ce));
    xcb_flush(m_conn);
}

void WM::handleDestroyNotify(xcb_destroy_notify_event_t *dn)
{
    xcb_window_t w = dn->window;
    auto it = std::find(m_windowList.begin(), m_windowList.end(), w);
    if (it != m_windowList.end()) {
        m_windowList.erase(it);
        if (m_currentWindowIndex >= m_windowList.size())
            m_currentWindowIndex = 0;
    }
    invalidateGeometryCache(w);
}

void WM::handleUnmapNotify(xcb_unmap_notify_event_t *un)
{
    (void)un;
}

void WM::handleConfigureRequest(xcb_configure_request_event_t *cr)
{
    uint16_t mask = cr->value_mask;
    uint32_t vals[7];
    int i = 0;
    if (mask & XCB_CONFIG_WINDOW_X)            vals[i++] = cr->x;
    if (mask & XCB_CONFIG_WINDOW_Y)            vals[i++] = cr->y;
    if (mask & XCB_CONFIG_WINDOW_WIDTH)        vals[i++] = cr->width;
    if (mask & XCB_CONFIG_WINDOW_HEIGHT)       vals[i++] = cr->height;
    if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) vals[i++] = cr->border_width;
    if (mask & XCB_CONFIG_WINDOW_SIBLING)      vals[i++] = cr->sibling;
    if (mask & XCB_CONFIG_WINDOW_STACK_MODE)   vals[i++] = cr->stack_mode;
    xcb_configure_window(m_conn, cr->window, mask, vals);
    invalidateGeometryCache(cr->window);
    xcb_flush(m_conn);
}

void WM::handleClientMessage(xcb_client_message_event_t *cm)
{
    if (cm->type == WM_PROTOCOLS && cm->data.data32[0] == WM_DELETE_WINDOW) {
        xcb_destroy_window(m_conn, cm->window);
        xcb_flush(m_conn);
    } else if (cm->type == m_ewmh._NET_ACTIVE_WINDOW) {
        xcb_window_t w = cm->data.data32[1];
        if (w) focusWindow(w);
    }
}

void WM::handleRunnerInput(xcb_keysym_t ks)
{
    if (ks == XK_Escape) {
        destroyRunnerDialog();
    } else if (ks == XK_Return) {
        executeCommand(m_runnerInput);
        destroyRunnerDialog();
    } else if (ks == XK_BackSpace) {
        if (!m_runnerInput.empty()) {
            m_runnerInput.pop_back();
            redrawRunnerDialog();
        }
    } else {
        if (ks >= 32 && ks <= 126) {
            m_runnerInput.push_back(static_cast<char>(ks));
            redrawRunnerDialog();
        }
    }
}

void WM::executeCommand(const std::string &cmd)
{
    if (cmd.empty()) return;
    pid_t pid = fork();
    if (pid < 0) {
        m_logger.log("Failed to fork for command: " + cmd);
        return;
    }
    if (pid == 0) {
        if (setsid() == -1) _exit(1);
        for (int fd = 0; fd < (int)sysconf(_SC_OPEN_MAX); fd++) {
            close(fd);
        }
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(1);
    } else {
        m_logger.log("Launched command: " + cmd + " [PID=" + std::to_string(pid) + "]");
    }
}

void WM::redrawRunnerDialog()
{
    if (!m_isRunnerActive || m_runnerWindow == XCB_NONE)
        return;
    xcb_expose_event_t ev = {};
    ev.response_type = XCB_EXPOSE;
    ev.window = m_runnerWindow;
    ev.width = RUNNER_WIDTH;
    ev.height = RUNNER_HEIGHT;
    xcb_send_event(m_conn, false, m_runnerWindow, XCB_EVENT_MASK_EXPOSURE,
                   reinterpret_cast<char*>(&ev));
    xcb_flush(m_conn);
}

/*******************************************************************************
 * Main Function
 ******************************************************************************/
int main()
{
    signal(SIGCHLD, SIG_IGN);

    const char* home = getenv("HOME");
    std::string logPath = home ? std::string(home) + "/lwm.log" : "lwm.log";
    Logger logger(logPath);
    logger.log("Starting LWM Minimal WM with new features...");

    WM wm(logger);
    if (!wm.initialize()) {
        logger.log("LWM initialization failed.");
        return 1;
    }

    wm.runEventLoop();
    wm.cleanup();

    return 0;
}
