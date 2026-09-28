#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <openvr.h>
#include <superblt_flat.h>

extern "C"
{
    __declspec(dllexport) const char* MODULE_LICENCE_DECLARATION =
        "This module is licenced under the GNU GPL version 2 or later, or another compatible licence";

    __declspec(dllexport) const char* MODULE_SOURCE_CODE_LOCATION = nullptr;
    __declspec(dllexport) const char* MODULE_SOURCE_CODE_REVISION = nullptr;
}

namespace {

    std::mutex g_mutex;

    bool g_active = false;
    bool g_ready = false;
    bool g_submitted = false;
    bool g_we_initialized_openvr = false;

    // SteamVR can sometimes emit KeyboardClosed around the same time as
    // a real confirmation. Do not immediately treat Closed as Cancel.
    bool g_close_pending = false;
    bool g_have_event_input = false;

    std::chrono::steady_clock::time_point g_close_deadline;

    constexpr auto kCloseGrace =
        std::chrono::milliseconds(300);

    std::string g_event_buffer;
    size_t g_cursor_bytes = 0;
    std::string g_result;
    std::string g_error;


    void log_line(const std::string& s) {
        PD2HOOK_LOG_LUA(
            ("[PD2 VR Chat Buffer native v0.9-debug] " + s).c_str()
        );
    }


    bool ensure_openvr() {
        if (vr::VRSystem() && vr::VROverlay()) {
            return true;
        }

        vr::EVRInitError err = vr::VRInitError_None;

        vr::VR_Init(
            &err,
            vr::VRApplication_Overlay
        );

        if (
            err != vr::VRInitError_None ||
            !vr::VRSystem() ||
            !vr::VROverlay()
            ) {
            g_error =
                std::string("OpenVR init failed: ") +
                vr::VR_GetVRInitErrorAsEnglishDescription(err);

            return false;
        }

        g_we_initialized_openvr = true;

        return true;
    }


    static size_t prev_utf8_boundary(
        const std::string& s,
        size_t pos
    ) {
        if (pos == 0 || s.empty()) {
            return 0;
        }

        size_t i =
            std::min(pos, s.size()) - 1;

        while (
            i > 0 &&
            (
                static_cast<unsigned char>(s[i])
                & 0xC0
                ) == 0x80
            ) {
            --i;
        }

        return i;
    }


    static size_t next_utf8_boundary(
        const std::string& s,
        size_t pos
    ) {
        if (pos >= s.size()) {
            return s.size();
        }

        size_t i = pos + 1;

        while (
            i < s.size() &&
            (
                static_cast<unsigned char>(s[i])
                & 0xC0
                ) == 0x80
            ) {
            ++i;
        }

        return i;
    }


    static size_t utf8_char_count_to(
        const std::string& s,
        size_t byte_pos
    ) {
        byte_pos =
            std::min(byte_pos, s.size());

        size_t count = 0;

        for (
            size_t i = 0;
            i < byte_pos;
            ++i
            ) {
            unsigned char c =
                static_cast<unsigned char>(
                    s[i]
                    );

            if ((c & 0xC0) != 0x80) {
                ++count;
            }
        }

        return count;
    }


    static bool is_left_arrow(
        const std::string& s
    ) {
        return s == "\x1b[D";
    }


    static bool is_right_arrow(
        const std::string& s
    ) {
        return s == "\x1b[C";
    }


    static bool is_line_break(
        const std::string& s
    ) {
        return
            s == "\n" ||
            s == "\r" ||
            s == "\r\n";
    }


    std::string read_keyboard_text() {
        if (!vr::VROverlay()) {
            return {};
        }

        uint32_t needed =
            vr::VROverlay()->GetKeyboardText(
                nullptr,
                0
            );

        if (needed == 0) {
            return {};
        }

        std::vector<char> buf(
            static_cast<size_t>(needed) + 2,
            0
        );

        uint32_t written =
            vr::VROverlay()->GetKeyboardText(
                buf.data(),
                static_cast<uint32_t>(buf.size())
            );

        if (written == 0) {
            return {};
        }

        return std::string(buf.data());
    }


    void append_keyboard_event(
        const vr::VREvent_t& event
    ) {
        const char* p =
            event.data.keyboard.cNewInput;

        std::string fragment;

        if (p) {
            size_t n =
                strnlen(
                    p,
                    sizeof(
                        event.data.keyboard.cNewInput
                        )
                );

            fragment.assign(p, n);
        }

        std::string overlay_now =
            read_keyboard_text();

        log_line(
            "KeyboardCharInput cNewInput=[" +
            fragment +
            "] GetKeyboardText=[" +
            overlay_now +
            "] current_buffer=[" +
            g_event_buffer +
            "] cursor=" +
            std::to_string(
                utf8_char_count_to(
                    g_event_buffer,
                    g_cursor_bytes
                )
            )
        );

        g_have_event_input = true;

        if (g_close_pending) {
            g_close_pending = false;

            log_line(
                "new keyboard input "
                "cancelled pending close"
            );
        }

        const std::string& token =
            !fragment.empty()
            ? fragment
            : overlay_now;


        //------------------------------------------------------------
            // Backspace
        //------------------------------------------------------------

            if (token == "\b") {
                if (g_cursor_bytes > 0) {

                    size_t prev =
                        prev_utf8_boundary(
                            g_event_buffer,
                            g_cursor_bytes
                        );

                    g_event_buffer.erase(
                        prev,
                        g_cursor_bytes - prev
                    );

                    g_cursor_bytes =
                        prev;
                }

                log_line(
                    "Backspace -> buffer=[" +
                    g_event_buffer +
                    "] cursor=" +
                    std::to_string(
                        utf8_char_count_to(
                            g_event_buffer,
                            g_cursor_bytes
                        )
                    )
                );

                return;
            }


        //------------------------------------------------------------
            // Left arrow
        //------------------------------------------------------------

            if (is_left_arrow(token)) {

                g_cursor_bytes =
                    prev_utf8_boundary(
                        g_event_buffer,
                        g_cursor_bytes
                    );

                log_line(
                    "Left -> cursor=" +
                    std::to_string(
                        utf8_char_count_to(
                            g_event_buffer,
                            g_cursor_bytes
                        )
                    )
                );

                return;
            }


        //------------------------------------------------------------
            // Right arrow
        //------------------------------------------------------------

            if (is_right_arrow(token)) {

                g_cursor_bytes =
                    next_utf8_boundary(
                        g_event_buffer,
                        g_cursor_bytes
                    );

                log_line(
                    "Right -> cursor=" +
                    std::to_string(
                        utf8_char_count_to(
                            g_event_buffer,
                            g_cursor_bytes
                        )
                    )
                );

                return;
            }


        //------------------------------------------------------------
            // Enter / newline garbage
        //------------------------------------------------------------

            if (is_line_break(token)) {
                log_line(
                    "ignored line-break "
                    "KeyboardCharInput"
                );

                return;
            }


       // ------------------------------------------------------------
            // Normal character insertion
       // ------------------------------------------------------------

            if (!token.empty()) {

                g_event_buffer.insert(
                    g_cursor_bytes,
                    token
                );

                g_cursor_bytes +=
                    token.size();

                log_line(
                    "inserted token -> buffer=[" +
                    g_event_buffer +
                    "] cursor=" +
                    std::to_string(
                        utf8_char_count_to(
                            g_event_buffer,
                            g_cursor_bytes
                        )
                    )
                );
            }
    }


    void finish(bool submitted) {
        std::string overlay_text =
            read_keyboard_text();
        if (
            overlay_text == "\b" ||
            is_line_break(overlay_text) ||
            is_left_arrow(overlay_text) ||
            is_right_arrow(overlay_text)
            ) {
            overlay_text.clear();
        }

        log_line(
            "finish buffers: event_buffer=[" +
            g_event_buffer +
            "] overlay_text=[" +
            overlay_text +
            "]"
        );

        // Modern SteamVR's legacy GetKeyboardText path may contain
        // only the latest character. Prefer our accumulated event
        // buffer whenever it clearly contains more data.
        if (g_have_event_input) {
            g_result =
                g_event_buffer;

            log_line(
                "finish chose artificial event buffer"
            );
        }
        else if (!overlay_text.empty()) {
            g_result =
                overlay_text;

            log_line(
                "finish chose overlay text"
            );
        }
        else {
            g_result.clear();

            log_line(
                "finish used empty fallback"
            );
        }

        g_close_pending = false;

        g_submitted = submitted;
        g_ready = true;
        g_active = false;

        log_line(
            std::string("finish submitted=") +
            (submitted ? "true" : "false") +
            " result=[" +
            g_result +
            "]"
        );
    }


    void log_keyboard_event_details(
        const char* name,
        const vr::VREvent_t& event
    ) {
        const char* p =
            event.data.keyboard.cNewInput;

        std::string c_new_input;

        if (p) {
            size_t n =
                strnlen(
                    p,
                    sizeof(
                        event.data.keyboard.cNewInput
                        )
                );

            c_new_input.assign(p, n);
        }

        std::string overlay_text =
            read_keyboard_text();

        log_line(
            std::string(name) +
            " type=" +
            std::to_string(event.eventType) +
            " device=" +
            std::to_string(
                event.trackedDeviceIndex
            ) +
            " age=" +
            std::to_string(
                event.eventAgeSeconds
            ) +
            " userValue=" +
            std::to_string(
                static_cast<unsigned long long>(
                    event.data.keyboard.uUserValue
                    )
            ) +
            " cNewInput=[" +
            c_new_input +
            "] overlay_text=[" +
            overlay_text +
            "] event_buffer=[" +
            g_event_buffer +
            "]"
        );
    }


    int lua_start(lua_State* L) {
        const char* description =
            luaL_optstring(
                L,
                1,
                "PAYDAY 2 Chat"
            );

        int max_chars =
            static_cast<int>(
                luaL_optinteger(
                    L,
                    2,
                    60
                )
                );

        const char* existing =
            luaL_optstring(
                L,
                3,
                ""
            );

        std::lock_guard<std::mutex> lock(
            g_mutex
        );

        if (g_active) {
            lua_pushboolean(L, 1);
            lua_pushstring(
                L,
                "already active"
            );

            return 2;
        }

        g_ready = false;
        g_submitted = false;
        g_close_pending = false;
        g_have_event_input = false;

        g_event_buffer =
            existing
            ? existing
            : "";

        g_cursor_bytes =
            g_event_buffer.size();

        g_result.clear();
        g_error.clear();

        if (!ensure_openvr()) {
            lua_pushboolean(L, 0);
            lua_pushstring(
                L,
                g_error.c_str()
            );

            return 2;
        }

        vr::EVROverlayError err =
            vr::VROverlay()->ShowKeyboard(
                vr::k_EGamepadTextInputModeNormal,
                vr::k_EGamepadTextInputLineModeSingleLine,
                0,
                description,
                static_cast<uint32_t>(
                    std::max(
                        1,
                        max_chars
                    )
                    ),
                existing,
                0
            );

        if (
            err !=
            vr::VROverlayError_None
            ) {
            g_error =
                std::string(
                    "ShowKeyboard failed: "
                ) +
                vr::VROverlay()
                ->GetOverlayErrorNameFromEnum(
                    err
                );

            lua_pushboolean(L, 0);
            lua_pushstring(
                L,
                g_error.c_str()
            );

            return 2;
        }

        g_active = true;

        log_line(
            "SteamVR keyboard opened"
        );

        lua_pushboolean(L, 1);
        lua_pushnil(L);

        return 2;
    }


    int lua_poll(lua_State* L) {
        std::lock_guard<std::mutex> lock(
            g_mutex
        );

        if (!g_ready) {
            lua_pushstring(
                L,
                "pending"
            );

            lua_pushnil(L);

            return 2;
        }

        lua_pushstring(
            L,
            g_submitted
            ? "done"
            : "cancelled"
        );

        lua_pushlstring(
            L,
            g_result.data(),
            g_result.size()
        );

        g_ready = false;

        g_result.clear();
        g_event_buffer.clear();

        g_cursor_bytes = 0;
        g_have_event_input = false;

        return 2;
    }

    int lua_peek(lua_State* L) {
        std::lock_guard<std::mutex> lock(
            g_mutex
        );

        lua_pushlstring(
            L,
            g_event_buffer.data(),
            g_event_buffer.size()
        );

        lua_pushinteger(
            L,
            static_cast<lua_Integer>(
                utf8_char_count_to(
                    g_event_buffer,
                    g_cursor_bytes
                )
                )
        );

        return 2;
    }

    int lua_cancel(lua_State* L) {
        std::lock_guard<std::mutex> lock(
            g_mutex
        );

        if (
            g_active &&
            vr::VROverlay()
            ) {
            vr::VROverlay()
                ->HideKeyboard();
        }

        g_active = false;
        g_ready = false;
        g_submitted = false;
        g_close_pending = false;
        g_have_event_input = false;

        g_event_buffer.clear();
        g_result.clear();
        g_error.clear();

        g_cursor_bytes = 0;

        return 0;
    }

} // namespace


void Plugin_Init() {
    log_line(
        "plugin initialized"
    );
}


void Plugin_Update() {
    std::lock_guard<std::mutex> lock(
        g_mutex
    );

    if (
        !g_active ||
        !vr::VRSystem()
        ) {
        return;
    }

    vr::VREvent_t event{};

    while (
        vr::VRSystem()->PollNextEvent(
            &event,
            sizeof(event)
        )
        ) {
        switch (event.eventType) {

        case vr::VREvent_KeyboardCharInput:
            append_keyboard_event(
                event
            );
            break;


        case vr::VREvent_KeyboardDone:
            log_keyboard_event_details(
                "KeyboardDone",
                event
            );

            // A real Done always wins over a pending Closed.
            g_close_pending = false;

            finish(true);

            return;


        case vr::VREvent_KeyboardClosed:
            log_keyboard_event_details(
                "KeyboardClosed",
                event
            );

            // Do NOT finish(false) immediately.
            // Some SteamVR builds may produce Closed immediately
            // before/around the actual Done event.
            g_close_pending = true;

            g_close_deadline =
                std::chrono::steady_clock::now() +
                kCloseGrace;

            log_line(
                "KeyboardClosed deferred for 300 ms"
            );

            break;


        case vr::VREvent_KeyboardClosed_Global:
            log_keyboard_event_details(
                "KeyboardClosed_Global",
                event
            );

            g_close_pending = true;

            g_close_deadline =
                std::chrono::steady_clock::now() +
                kCloseGrace;

            log_line(
                "KeyboardClosed_Global deferred for 300 ms"
            );

            break;


        default:
            break;
        }
    }

    // Only after the event queue has been drained do we decide
    // whether a Closed was a real cancellation.
    if (
        g_active &&
        g_close_pending &&
        std::chrono::steady_clock::now() >=
        g_close_deadline
        ) {
        log_line(
            "pending KeyboardClosed expired -> cancel"
        );

        finish(false);
    }
}


void Plugin_Setup_Lua(lua_State* L) {
    (void)L;
}


int Plugin_PushLua(lua_State* L) {
    lua_newtable(L);

    lua_pushcfunction(
        L,
        lua_start
    );

    lua_setfield(
        L,
        -2,
        "start"
    );

    lua_pushcfunction(
        L,
        lua_poll
    );

    lua_setfield(
        L,
        -2,
        "poll"
    );

    lua_pushcfunction(
        L,
        lua_peek
    );

    lua_setfield(
        L,
        -2,
        "peek"
    );

    lua_pushcfunction(
        L,
        lua_cancel
    );

    lua_setfield(
        L,
        -2,
        "cancel"
    );

    return 1;
}