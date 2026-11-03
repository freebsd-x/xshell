// ── xshell panel builders: display: brightness, colour, scaling, night light, lock and screen saver, GPU ──
// Definitions of buildXPanel(ctx) functions declared in shell_panels.h (see
// there for ShellContext and the `using namespace` rationale). One of six
// files split by area from the former shell_panels.cc so they compile in
// parallel and each compiler process stays small.
#include "shell_panels.h"
#include <format>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <ctime>
#include <thread>

// Color Filter. An enable switch (SetEnabled) + a 4-way colour-blindness type
// button row (Normal/Deuter./Protan./Tritan. → SetType) + a monochrome switch
// (SetMonochrome) + an intensity slider/label (SetIntensity). update_cf_ui (fed by
// a GetState reply + broadcast + timer) reflects all controls (highlighting the
// active type button). kCfTypeLabels stays a builder-local static. Behaviour
// identical to the former inline block.
void buildColorFilterPanel(ShellContext& ctx) {
    ctx.cf_switch = std::make_shared<SwitchWidget>(false);
    ctx.cf_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        ColorFilterEnableRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.ColorFilter",
            static_cast<uint32_t>(ColorFilterOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });

    static const char* kCfTypeLabels[] = { "Normal", "Deuter.", "Protan.", "Tritan." };
    ctx.cf_type_btns = std::make_shared<std::array<std::shared_ptr<ButtonWidget>, 4>>();
    for (int i = 0; i < 4; ++i) {
        (*ctx.cf_type_btns)[i] = std::make_shared<ButtonWidget>(kCfTypeLabels[i], [i]() {
            if (!g_shell_xbus) return;
            ColorFilterTypeRequest req{};
            req.type_ = static_cast<uint32_t>(i);
            g_shell_xbus->Call("com.freebsdx.ColorFilter",
                static_cast<uint32_t>(ColorFilterOpcode::SetType),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
        (*ctx.cf_type_btns)[i]->setFontSize(10.0f);
        (*ctx.cf_type_btns)[i]->setFixedSize(58, 24);
        (*ctx.cf_type_btns)[i]->setBackgroundColor({0.18f, 0.18f, 0.22f, 1.0f});
    }
    ctx.cf_type_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        (*ctx.cf_type_btns)[0],
        std::make_shared<Spacer>(4, 1),
        (*ctx.cf_type_btns)[1],
        std::make_shared<Spacer>(4, 1),
        (*ctx.cf_type_btns)[2],
        std::make_shared<Spacer>(4, 1),
        (*ctx.cf_type_btns)[3],
    });

    ctx.cf_mono_switch = std::make_shared<SwitchWidget>(false);
    ctx.cf_mono_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        ColorFilterToggleRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.ColorFilter",
            static_cast<uint32_t>(ColorFilterOpcode::SetMonochrome),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });

    ctx.cf_intensity_val    = std::make_shared<TextWidget>("100%", 11.0f);
    ctx.cf_intensity_val->setColor({0.70f, 0.70f, 0.75f, 1.0f});
    ctx.cf_intensity_slider = std::make_shared<SliderWidget>(100.0f, 0.0f, 100.0f);
    ctx.cf_intensity_slider->setOnChanged([cf_intensity_val = ctx.cf_intensity_val](float v) {
        cf_intensity_val->setText(std::format("{:.0f}%", v));
        if (!g_shell_xbus) return;
        ColorFilterIntensityRequest req{};
        req.intensity_ = static_cast<uint32_t>(v);
        g_shell_xbus->Call("com.freebsdx.ColorFilter",
            static_cast<uint32_t>(ColorFilterOpcode::SetIntensity),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.cf_intensity_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        ctx.cf_intensity_slider,
        std::make_shared<Spacer>(6, 1),
        ctx.cf_intensity_val,
    });

    ctx.update_cf_ui = [cf_switch = ctx.cf_switch, cf_type_btns = ctx.cf_type_btns,
                        cf_mono_switch = ctx.cf_mono_switch,
                        cf_intensity_slider = ctx.cf_intensity_slider, cf_intensity_val = ctx.cf_intensity_val]
                       (const ColorFilterState& st) {
        cf_switch->setValue(st.enabled_ != 0);
        uint32_t t = std::min(st.type_, 3u);
        for (int i = 0; i < 4; ++i) {
            (*cf_type_btns)[i]->setBackgroundColor(
                (static_cast<uint32_t>(i) == t)
                    ? Color{0.30f, 0.55f, 0.90f, 1.0f}
                    : Color{0.18f, 0.18f, 0.22f, 1.0f});
        }
        cf_mono_switch->setValue(st.monochrome_ != 0);
        float iv = std::clamp(static_cast<float>(st.intensity_), 0.0f, 100.0f);
        cf_intensity_slider->setValue(iv);
        cf_intensity_val->setText(std::format("{:.0f}%", iv));
    };
}

void buildScreenLockPanel(ShellContext& ctx) {
    ctx.slk_enabled_sw    = std::make_shared<SwitchWidget>(true);
    ctx.slk_require_sw    = std::make_shared<SwitchWidget>(true);
    ctx.slk_sleep_sw      = std::make_shared<SwitchWidget>(true);
    ctx.slk_timeout_cur   = std::make_shared<int>(60);
    ctx.slk_status_lbl    = std::make_shared<TextWidget>("", 10.0f);
    ctx.slk_status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});

    static const std::array<std::pair<int,const char*>, 6> kSlkTimeouts{{
        {0,    "Never"}, {30,   "30 s"}, {60,   "1 min"},
        {300,  "5 min"}, {600, "10 min"}, {1800, "30 min"},
    }};
    ctx.slk_timeout_btns = std::make_shared<std::vector<std::shared_ptr<ButtonWidget>>>();
    for (const auto& [secs, label] : kSlkTimeouts)
        ctx.slk_timeout_btns->push_back(std::make_shared<ButtonWidget>(label));
    for (auto& b : *ctx.slk_timeout_btns) b->setFixedSize(52, 24);

    ctx.slk_hl = [slk_timeout_btns = ctx.slk_timeout_btns, slk_timeout_cur = ctx.slk_timeout_cur]() {
        Color hi  = {0.22f, 0.44f, 0.88f, 0.95f};
        Color dim = {0.22f, 0.22f, 0.26f, 0.90f};
        for (size_t i = 0; i < slk_timeout_btns->size(); ++i)
            (*slk_timeout_btns)[i]->setBackgroundColor(
                kSlkTimeouts[i].first == *slk_timeout_cur ? hi : dim);
    };
    ctx.slk_hl();

    auto send_slk = [slk_enabled_sw = ctx.slk_enabled_sw, slk_require_sw = ctx.slk_require_sw,
                      slk_sleep_sw = ctx.slk_sleep_sw,
                      slk_timeout_cur = ctx.slk_timeout_cur, slk_status_lbl = ctx.slk_status_lbl]() {
        if (!g_shell_xbus) return;
        ScreenLockPolicy p{};
        p.enabled_          = slk_enabled_sw->value()  ? 1u : 0u;
        p.require_password_ = slk_require_sw->value()  ? 1u : 0u;
        p.lock_on_sleep_    = slk_sleep_sw->value()    ? 1u : 0u;
        p.timeout_seconds_  = static_cast<uint32_t>(*slk_timeout_cur);
        g_shell_xbus->Call("com.freebsdx.ScreenLock",
            static_cast<uint32_t>(ScreenLockOpcode::SetPolicy),
            {reinterpret_cast<const uint8_t*>(&p), sizeof(p)},
            [slk_status_lbl](const XBusMessage&) {
                slk_status_lbl->setText("Saved \xe2\x9c\x93");
            });
    };

    for (size_t i = 0; i < ctx.slk_timeout_btns->size(); ++i) {
        (*ctx.slk_timeout_btns)[i]->onClick(
            [i, slk_timeout_cur = ctx.slk_timeout_cur, slk_hl = ctx.slk_hl, send_slk]() {
                *slk_timeout_cur = kSlkTimeouts[i].first;
                slk_hl();
                send_slk();
            });
    }
    ctx.slk_enabled_sw->setOnChanged([send_slk](bool) { send_slk(); });
    ctx.slk_require_sw->setOnChanged([send_slk](bool) { send_slk(); });
    ctx.slk_sleep_sw->setOnChanged([send_slk](bool)   { send_slk(); });

    ctx.slk_lock_btn = std::make_shared<ButtonWidget>("\xf0\x9f\x94\x92 Lock Now",
        []() {
            if (g_shell_xbus)
                g_shell_xbus->Call("com.freebsdx.ScreenLock",
                    static_cast<uint32_t>(ScreenLockOpcode::LockNow), {}, nullptr);
        });
    ctx.slk_lock_btn->setFixedSize(90, 26);
    ctx.slk_lock_btn->setBackgroundColor({0.55f, 0.20f, 0.20f, 0.90f});

    ctx.update_slk_ui = [slk_enabled_sw = ctx.slk_enabled_sw, slk_require_sw = ctx.slk_require_sw,
                          slk_sleep_sw = ctx.slk_sleep_sw,
                          slk_timeout_cur = ctx.slk_timeout_cur, slk_hl = ctx.slk_hl](const ScreenLockPolicy& p) {
        slk_enabled_sw->setValue(p.enabled_ != 0);
        slk_require_sw->setValue(p.require_password_ != 0);
        slk_sleep_sw->setValue(p.lock_on_sleep_ != 0);
        *slk_timeout_cur = static_cast<int>(p.timeout_seconds_);
        slk_hl();
    };
}

void buildAutoLockPanel(ShellContext& ctx) {
    // ON = 10 min idle-before-lock (calls LockOpcode::SetTimeout 600 s)
    // OFF = never lock automatically (calls LockOpcode::SetTimeout 0)
    ctx.qs_autolock_switch = std::make_shared<SwitchWidget>(true);
    ctx.qs_autolock_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        LockTimeoutRequest req{};
        req.seconds_ = on ? 600u : 0u; // 10 min or disabled
        g_shell_xbus->Call("com.freebsdx.Lock",
            static_cast<uint32_t>(LockOpcode::SetTimeout),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    auto qs_autolock_lbl = std::make_shared<TextWidget>("Auto-Lock (10 min)", 12.0f);
    qs_autolock_lbl->setColor({0.75f, 0.75f, 0.80f, 1.0f});
    ctx.qs_autolock_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🔒", 14.0f),
        std::make_shared<Spacer>(8, 1),
        qs_autolock_lbl,
        std::make_shared<Spacer>(1, 1, true),
        ctx.qs_autolock_switch
    });
}

void buildNightShiftPanel(ShellContext& ctx) {
    ctx.qs_ns_switch = std::make_shared<SwitchWidget>(false);
    ctx.qs_ns_val    = std::make_shared<TextWidget>("50%", 11.0f);
    ctx.qs_ns_slider = std::make_shared<SliderWidget>(50.0f, 0.0f, 100.0f);
    ctx.qs_ns_val->setColor({0.72f, 0.72f, 0.72f, 1.0f});

    // Shared helper to send SetConfig
    ctx.send_ns_config = [qs_ns_switch = ctx.qs_ns_switch, qs_ns_slider = ctx.qs_ns_slider]() {
        if (!g_shell_xbus) return;
        NightShiftConfig cfg{};
        cfg.enabled_   = qs_ns_switch->value() ? 1u : 0u;
        cfg.intensity_ = static_cast<uint32_t>(qs_ns_slider->value());
        cfg.auto_mode_ = 0u;
        g_shell_xbus->Call("com.freebsdx.NightShift",
            static_cast<uint32_t>(NightShiftOpcode::SetConfig),
            {reinterpret_cast<const uint8_t*>(&cfg), sizeof(cfg)}, nullptr);
    };
    ctx.qs_ns_switch->setOnChanged([send_ns_config = ctx.send_ns_config](bool) { send_ns_config(); });
    ctx.qs_ns_slider->setOnChanged([qs_ns_val = ctx.qs_ns_val, send_ns_config = ctx.send_ns_config](float v) {
        qs_ns_val->setText(std::format("{}%", static_cast<int>(v)));
        send_ns_config();
    });

    ctx.qs_ns_hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🌙", 15.0f),
        std::make_shared<Spacer>(8, 1),
        std::make_shared<TextWidget>("Night Shift", 12.0f),
        std::make_shared<Spacer>(1, 1, true),
        ctx.qs_ns_switch
    });
    ctx.qs_ns_slider_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🌓", 15.0f),
        std::make_shared<Spacer>(8, 1),
        ctx.qs_ns_slider,
        std::make_shared<Spacer>(8, 1),
        ctx.qs_ns_val
    });
}

void buildScreenSaverPanel(ShellContext& ctx) {
    ctx.ss_on          = std::make_shared<bool>(false);
    ctx.ss_switch      = std::make_shared<SwitchWidget>(false);
    ctx.ss_timeout_cur = std::make_shared<uint32_t>(300u);
    ctx.ss_mode_cur    = std::make_shared<uint32_t>(0u);

    static constexpr uint32_t kSsTimeoutCount = 4u;
    static constexpr const char* kSsTimeoutLabels[] = {"1m", "5m", "10m", "30m"};
    static constexpr uint32_t kSsTimeoutValues[] = {60u, 300u, 600u, 1800u};
    ctx.ss_timeout_btns =
        std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kSsTimeoutCount>>();

    static constexpr uint32_t kSsModeCount = 3u;
    static constexpr const char* kSsModeLabels[] = {"Blank", "Clock", "Matrix"};
    ctx.ss_mode_btns =
        std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kSsModeCount>>();

    auto send_ss_cfg = [ss_on = ctx.ss_on, ss_timeout_cur = ctx.ss_timeout_cur,
                        ss_mode_cur = ctx.ss_mode_cur]() {
        if (!g_shell_xbus) return;
        ScreenSaverConfig cfg{};
        cfg.timeout_secs_ = *ss_on ? *ss_timeout_cur : 0u;
        cfg.mode_         = *ss_mode_cur;
        g_shell_xbus->Call("com.freebsdx.ScreenSaver",
            static_cast<uint32_t>(ScreenSaverOpcode::SetConfig),
            {reinterpret_cast<const uint8_t*>(&cfg), sizeof(cfg)},
            nullptr);
    };

    auto ss_timeout_hl = [ss_timeout_btns = ctx.ss_timeout_btns, ss_timeout_cur = ctx.ss_timeout_cur]() {
        for (uint32_t i = 0; i < kSsTimeoutCount; ++i) {
            bool active = (*ss_timeout_cur == kSsTimeoutValues[i]);
            (*ss_timeout_btns)[i]->setBackgroundColor(
                active ? Color{0.20f, 0.55f, 0.90f, 1.0f}
                       : Color{0.22f, 0.22f, 0.28f, 1.0f});
        }
    };

    auto ss_mode_hl = [ss_mode_btns = ctx.ss_mode_btns, ss_mode_cur = ctx.ss_mode_cur]() {
        for (uint32_t i = 0; i < kSsModeCount; ++i) {
            bool active = (*ss_mode_cur == i);
            (*ss_mode_btns)[i]->setBackgroundColor(
                active ? Color{0.20f, 0.55f, 0.90f, 1.0f}
                       : Color{0.22f, 0.22f, 0.28f, 1.0f});
        }
    };

    for (uint32_t i = 0; i < kSsTimeoutCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kSsTimeoutLabels[i]);
        btn->setFixedSize(36, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.28f, 1.0f});
        btn->onClick([i, ss_timeout_cur = ctx.ss_timeout_cur, ss_timeout_hl, send_ss_cfg]() {
            *ss_timeout_cur = kSsTimeoutValues[i];
            ss_timeout_hl();
            send_ss_cfg();
        });
        (*ctx.ss_timeout_btns)[i] = btn;
    }
    ss_timeout_hl();

    for (uint32_t i = 0; i < kSsModeCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kSsModeLabels[i]);
        btn->setFixedSize(i == 2 ? 58 : 48, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.28f, 1.0f});
        btn->onClick([i, ss_mode_cur = ctx.ss_mode_cur, ss_mode_hl, send_ss_cfg]() {
            *ss_mode_cur = static_cast<uint32_t>(i);
            ss_mode_hl();
            send_ss_cfg();
        });
        (*ctx.ss_mode_btns)[i] = btn;
    }
    ss_mode_hl();

    ctx.ss_switch->setOnChanged([ss_on = ctx.ss_on, send_ss_cfg](bool on) {
        *ss_on = on;
        send_ss_cfg();
    });

    ctx.update_ss_ui = [ss_on = ctx.ss_on, ss_switch = ctx.ss_switch,
                        ss_timeout_cur = ctx.ss_timeout_cur, ss_timeout_hl,
                        ss_mode_cur = ctx.ss_mode_cur, ss_mode_hl](uint32_t timeout_secs, uint32_t mode) {
        *ss_on = (timeout_secs > 0);
        ss_switch->setValue(*ss_on);
        if (timeout_secs > 0) *ss_timeout_cur = timeout_secs;
        ss_timeout_hl();
        *ss_mode_cur = std::min(mode, 2u);
        ss_mode_hl();
    };
}

void buildDockLayoutPanel(ShellContext& ctx) {
    static constexpr uint32_t kDockPosCount = 3u;
    static constexpr const char* kDockPosLabels[] = {"Bottom", "Left", "Right"};
    ctx.dock_pos_cur  = std::make_shared<uint32_t>(0u);
    ctx.dock_pos_btns = std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kDockPosCount>>();
    ctx.dock_autohide = std::make_shared<SwitchWidget>(false);

    auto dock_pos_hl = [dock_pos_btns = ctx.dock_pos_btns, dock_pos_cur = ctx.dock_pos_cur]() {
        for (uint32_t i = 0; i < kDockPosCount; ++i) {
            bool active = (*dock_pos_cur == i);
            (*dock_pos_btns)[i]->setBackgroundColor(
                active ? Color{0.20f, 0.55f, 0.90f, 1.0f}
                       : Color{0.22f, 0.22f, 0.28f, 1.0f});
        }
    };

    for (uint32_t i = 0; i < kDockPosCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kDockPosLabels[i]);
        btn->setFixedSize(58, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.28f, 1.0f});
        btn->onClick([i, dock_pos_cur = ctx.dock_pos_cur, dock_pos_hl]() {
            *dock_pos_cur = i;
            dock_pos_hl();
            if (!g_shell_xbus) return;
            DockSettings req{};
            req.position_ = i;
            g_shell_xbus->Call("com.freebsdx.Dock",
                static_cast<uint32_t>(DockOpcode::SetSettings),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                nullptr);
        });
        (*ctx.dock_pos_btns)[i] = btn;
    }
    dock_pos_hl();

    ctx.update_dock_ui = [dock_pos_cur = ctx.dock_pos_cur, dock_pos_hl,
                          dock_autohide = ctx.dock_autohide](const DockSettings& st) {
        *dock_pos_cur = std::min(st.position_, 2u);
        dock_pos_hl();
        dock_autohide->setValue(st.autohide_ != 0);
    };

    ctx.dock_autohide->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        DockSettings req{};
        req.autohide_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.Dock",
            static_cast<uint32_t>(DockOpcode::SetSettings),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            nullptr);
    });

    // Dev Overlay presets
    constexpr int kDevOverlayCount = 3;
    static const char* kDevOverlayLabels[] = {"Off", "FPS", "Repaint"};
    ctx.dev_overlay_cur  = std::make_shared<uint32_t>(0u); // default Off
    ctx.dev_overlay_btns =
        std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kDevOverlayCount>>();

    auto dev_overlay_hl = [dev_overlay_btns = ctx.dev_overlay_btns, dev_overlay_cur = ctx.dev_overlay_cur]() {
        for (int i = 0; i < kDevOverlayCount; ++i) {
            bool active = (*dev_overlay_cur == static_cast<uint32_t>(i));
            // Off = dim-grey, FPS = blue, Repaint = amber when active
            Color active_col = (i == 0) ? Color{0.30f, 0.30f, 0.34f, 1.0f}
                             : (i == 1) ? Color{0.20f, 0.45f, 0.85f, 1.0f}
                                        : Color{0.75f, 0.50f, 0.10f, 1.0f};
            (*dev_overlay_btns)[i]->setBackgroundColor(
                active ? active_col : Color{0.22f, 0.22f, 0.26f, 1.0f});
        }
    };

    for (int i = 0; i < kDevOverlayCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kDevOverlayLabels[i]);
        btn->setFixedSize(i == 2 ? 60 : 40, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});
        btn->onClick([i, dev_overlay_cur = ctx.dev_overlay_cur, dev_overlay_hl]() {
            *dev_overlay_cur = static_cast<uint32_t>(i);
            dev_overlay_hl();
            if (!g_shell_xbus) return;
            DevOverlayRequest req{};
            req.mode_ = static_cast<uint32_t>(i);
            g_shell_xbus->Call("com.freebsdx.DevOverlay",
                static_cast<uint32_t>(DevOverlayOpcode::SetMode),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
        (*ctx.dev_overlay_btns)[i] = btn;
    }
    dev_overlay_hl();

    ctx.update_dev_overlay_ui = [dev_overlay_cur = ctx.dev_overlay_cur, dev_overlay_hl](const DevOverlayState& st) {
        *dev_overlay_cur = std::min(st.mode_, 2u);
        dev_overlay_hl();
    };

    ctx.dev_overlay_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("\xf0\x9f\x94\xac Dev", 12.0f),
        std::make_shared<Spacer>(1, 1, true),
        (*ctx.dev_overlay_btns)[0],
        std::make_shared<Spacer>(4, 1),
        (*ctx.dev_overlay_btns)[1],
        std::make_shared<Spacer>(4, 1),
        (*ctx.dev_overlay_btns)[2],
    });
}

void buildXWaylandPanel(ShellContext& ctx) {
    ctx.xwl_running  = std::make_shared<bool>(false);
    ctx.xwl_switch   = std::make_shared<SwitchWidget>(false);
    ctx.xwl_info_lbl = std::make_shared<TextWidget>("Stopped", 10.0f);
    ctx.xwl_info_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

    ctx.xwl_switch->setOnChanged([xwl_running = ctx.xwl_running](bool on) {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.XWayland",
            on ? static_cast<uint32_t>(XWaylandOpcode::Enable)
               : static_cast<uint32_t>(XWaylandOpcode::Disable),
            {}, nullptr);
    });

    ctx.update_xwl_ui = [xwl_running = ctx.xwl_running, xwl_switch = ctx.xwl_switch,
                         xwl_info_lbl = ctx.xwl_info_lbl](const XWaylandStatus& st) {
        *xwl_running = (st.running_ != 0);
        xwl_switch->setValue(*xwl_running);
        if (*xwl_running) {
            xwl_info_lbl->setText(std::string("DISPLAY=") + st.display_);
            xwl_info_lbl->setColor({0.35f, 0.80f, 0.50f, 1.0f});
        } else {
            xwl_info_lbl->setText("Stopped");
            xwl_info_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
        }
    };
}

void buildDisplaySettingsPanel(ShellContext& ctx) {
    ctx.disp_col = std::make_shared<FlexColumn>();
    ctx.disp_col->setGap(4);

    auto disp_close_btn = std::make_shared<ButtonWidget>("✕", [](){});
    disp_close_btn->setPadding(4);

    auto disp_hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🖥", 15.0f),
        std::make_shared<Spacer>(8, 1),
        std::make_shared<TextWidget>("Display Settings", 15.0f),
        std::make_shared<Spacer>(1, 1, true),
        disp_close_btn
    });
    disp_hdr_row->setPadding(4);

    auto disp_scroll = std::make_shared<ScrollViewWidget>(ctx.disp_col);
    disp_scroll->setFlexGrow(1.0f);

    auto disp_panel_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        disp_hdr_row,
        std::make_shared<DividerWidget>(0.4f),
        std::make_shared<Spacer>(1, 6),
        disp_scroll
    });
    disp_panel_col->setPadding(14);

    ctx.disp_overlay = std::make_shared<ContainerWidget>(disp_panel_col);
    ctx.disp_overlay->setBackgroundColor({0.10f, 0.11f, 0.18f, 0.97f});
    ctx.disp_overlay->setFixedSize(310, 300);
    ctx.disp_overlay->setVisibility(false);

    *disp_close_btn = ButtonWidget(disp_close_btn->child(), [disp_overlay = ctx.disp_overlay]() {
        disp_overlay->setVisibility(false);
    });

    ctx.disp_btn = std::make_shared<ButtonWidget>("🖥", [disp_overlay = ctx.disp_overlay]() {
        disp_overlay->setVisibility(!disp_overlay->isVisible());
    });
    ctx.disp_btn->setPadding(4);
}

// OSD overlay (volume / brightness pop-up). Only the overlay widgets + the
// show_osd helper are created here; the brightness/volume slider re-wiring that
// fires the OSD stays in main() since it touches other panels' sliders.
void buildOsdOverlayPanel(ShellContext& ctx) {
    auto osd_icon      = std::make_shared<TextWidget>("🔊", 22.0f);
    auto osd_pct       = std::make_shared<TextWidget>("75%", 13.0f);
    osd_pct->setColor({0.80f, 0.80f, 0.80f, 1.0f});
    auto osd_bar_fill  = std::make_shared<ContainerWidget>(nullptr);
    osd_bar_fill->setBackgroundColor({0.90f, 0.90f, 0.90f, 1.0f});
    osd_bar_fill->setFixedSize(90.0f, 6.0f);
    auto osd_bar_rest  = std::make_shared<ContainerWidget>(nullptr);
    osd_bar_rest->setBackgroundColor({0.30f, 0.30f, 0.30f, 1.0f});
    osd_bar_rest->setFixedSize(30.0f, 6.0f);
    auto osd_bar = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        osd_bar_fill, osd_bar_rest});
    auto osd_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        osd_icon, std::make_shared<Spacer>(12, 1),
        osd_bar,  std::make_shared<Spacer>(10, 1),
        osd_pct
    });
    osd_row->setMainAxisAlignment(MainAxisAlignment::Center);
    osd_row->setCrossAxisAlignment(CrossAxisAlignment::Center);
    auto osd_pill = std::make_shared<ContainerWidget>(osd_row);
    osd_pill->setBackgroundColor({0.10f, 0.10f, 0.14f, 0.92f});
    osd_pill->setPadding(18);
    ctx.osd_overlay = std::make_shared<ContainerWidget>(osd_pill);
    ctx.osd_overlay->setFixedSize(10000.0f, 10000.0f);
    ctx.osd_overlay->setVisibility(false);

    // Generation counter — ensures only the latest show_osd timer hides the OSD
    auto osd_gen = std::make_shared<std::atomic<int>>(0);
    ctx.show_osd = std::make_shared<std::function<void(const std::string&, int)>>();
    *ctx.show_osd = [osd_overlay = ctx.osd_overlay, osd_icon, osd_pct, osd_bar_fill, osd_bar_rest, osd_gen]
            (const std::string& icon, int level) {
        osd_icon->setText(icon);
        osd_pct->setText(std::format("{}%", level));
        float fill = static_cast<float>(std::clamp(level, 0, 100)) * 1.2f;
        osd_bar_fill->setFixedSize(fill, 6.0f);
        osd_bar_rest->setFixedSize(120.0f - fill, 6.0f);
        osd_overlay->setVisibility(true);
        int gen = osd_gen->fetch_add(1) + 1;
        std::thread([osd_overlay, osd_gen, gen]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            if (osd_gen->load() == gen)
                osd_overlay->setVisibility(false);
        }).detach();
    };
}

// System resource sparklines section (action center). Three static monospace
// labels (header + CPU + RAM); the CPU/RAM labels are re-textualised by the
// refresh timer. Behaviour is identical to the former inline block.
void buildSparklinesPanel(ShellContext& ctx) {
    ctx.spark_hdr_lbl = std::make_shared<TextWidget>("📊 System Resources", 14.0f);
    ctx.spark_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.spark_cpu_lbl = std::make_shared<TextWidget>("CPU  ▁▁▁▁▁▁▁▁▁▁  0%", 12.0f);
    ctx.spark_cpu_lbl->setFontFamily("monospace");
    ctx.spark_cpu_lbl->setColor({0.45f, 0.85f, 0.45f, 1.0f});
    ctx.spark_ram_lbl = std::make_shared<TextWidget>("RAM  ▁▁▁▁▁▁▁▁▁▁  0%", 12.0f);
    ctx.spark_ram_lbl->setFontFamily("monospace");
    ctx.spark_ram_lbl->setColor({0.45f, 0.70f, 1.0f, 1.0f});
}

// Frame Timing monitor (action center). Builds the label stack + a 16-bar
// frame-time histogram and the update_ft_ui closure that recolours everything
// from a FramePerfState. Behaviour is identical to the former inline block.
void buildFrameTimingPanel(ShellContext& ctx) {
    ctx.ft_hdr_lbl     = std::make_shared<TextWidget>("\xf0\x9f\x93\x8a Frame Timing", 14.0f);
    ctx.ft_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.ft_avg_lbl     = std::make_shared<TextWidget>("avg —ms", 11.0f);
    ctx.ft_avg_lbl->setColor({0.75f, 0.75f, 0.80f, 1.0f});
    ctx.ft_p99_lbl     = std::make_shared<TextWidget>("p99 —ms", 11.0f);
    ctx.ft_p99_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.ft_jank_lbl    = std::make_shared<TextWidget>("jank —", 11.0f);
    ctx.ft_jank_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.ft_gpu_lbl     = std::make_shared<TextWidget>("gpu —µs", 10.0f);
    ctx.ft_gpu_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.ft_vsync_lbl   = std::make_shared<TextWidget>("vsync misses —", 10.0f);
    ctx.ft_vsync_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

    constexpr int kFtBars = 16;
    ctx.ft_bars = std::make_shared<std::array<std::shared_ptr<TextWidget>, kFtBars>>();
    ctx.ft_bar_row = std::make_shared<FlexRow>();
    ctx.ft_bar_row->setGap(2);
    for (int i = 0; i < kFtBars; ++i) {
        auto blk = std::make_shared<TextWidget>("\xe2\x96\x88", 10.0f); // █
        blk->setColor({0.35f, 0.80f, 0.45f, 1.0f}); // green default
        (*ctx.ft_bars)[i] = blk;
        ctx.ft_bar_row->addChild(blk);
    }

    ctx.update_ft_ui = [ft_avg_lbl = ctx.ft_avg_lbl, ft_p99_lbl = ctx.ft_p99_lbl,
                         ft_jank_lbl = ctx.ft_jank_lbl, ft_gpu_lbl = ctx.ft_gpu_lbl,
                         ft_vsync_lbl = ctx.ft_vsync_lbl, ft_bars = ctx.ft_bars](const FramePerfState& st) {
        float avg_ms = static_cast<float>(st.avg_frame_us_) / 1000.0f;
        float p99_ms = static_cast<float>(st.p99_frame_us_) / 1000.0f;

        ft_avg_lbl->setText(std::format("avg {:.1f}ms", avg_ms));
        ft_avg_lbl->setColor(avg_ms > 16.6f
            ? Color{0.85f, 0.35f, 0.35f, 1.0f}
            : avg_ms > 8.0f
                ? Color{0.95f, 0.72f, 0.20f, 1.0f}
                : Color{0.35f, 0.80f, 0.45f, 1.0f});

        ft_p99_lbl->setText(std::format("p99 {:.1f}ms", p99_ms));
        ft_p99_lbl->setColor(p99_ms > 16.6f
            ? Color{0.85f, 0.35f, 0.35f, 1.0f}
            : Color{0.55f, 0.55f, 0.60f, 1.0f});

        ft_jank_lbl->setText(std::format("jank {}", st.jank_count_));
        ft_jank_lbl->setColor(st.jank_count_ > 0
            ? Color{0.95f, 0.72f, 0.20f, 1.0f}
            : Color{0.35f, 0.80f, 0.45f, 1.0f});

        ft_gpu_lbl->setText(std::format("gpu {}µs", st.gpu_submit_us_));
        ft_vsync_lbl->setText(std::format("vsync misses {}", st.vsync_misses_));
        ft_vsync_lbl->setColor(st.vsync_misses_ > 0
            ? Color{0.95f, 0.72f, 0.20f, 1.0f}
            : Color{0.55f, 0.55f, 0.60f, 1.0f});

        constexpr int kFtBars = 16;
        for (int i = 0; i < kFtBars; ++i) {
            uint8_t ms = st.recent_[i];
            Color c = ms > 16 ? Color{0.85f, 0.35f, 0.35f, 1.0f}
                    : ms >  8 ? Color{0.95f, 0.72f, 0.20f, 1.0f}
                               : Color{0.35f, 0.80f, 0.45f, 1.0f};
            (*ft_bars)[i]->setColor(c);
        }
    };
}

// GPU Statistics monitor (action center). A latent panel — the labels are never
// added to a visible card; the update_gpu_ui_b closure keeps them alive and is
// fed from the GetState reply + refresh timer + broadcast. Only the updater
// escapes. Behaviour is identical to the former inline block.
void buildGpuStatsPanel(ShellContext& ctx) {
    auto gpu_hdr_lbl   = std::make_shared<TextWidget>("🖥 GPU Statistics", 14.0f);
    gpu_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto gpu_name_lbl_b  = std::make_shared<TextWidget>("—", 11.0f);
    gpu_name_lbl_b->setColor({0.65f, 0.65f, 0.70f, 1.0f});
    auto gpu_usage_lbl = std::make_shared<TextWidget>("Usage: —", 12.0f);
    auto gpu_vram_lbl_b  = std::make_shared<TextWidget>("VRAM:  —", 12.0f);
    auto gpu_temp_lbl_b  = std::make_shared<TextWidget>("Temp:  —", 12.0f);
    auto gpu_clock_lbl = std::make_shared<TextWidget>("Clock: —", 12.0f);
    ctx.update_gpu_ui_b = [gpu_name_lbl_b, gpu_usage_lbl, gpu_vram_lbl_b,
                          gpu_temp_lbl_b,  gpu_clock_lbl](const GpuStats& st) {
        if (st.name_[0]) gpu_name_lbl_b->setText(std::string(st.name_));
        gpu_usage_lbl->setText(std::format("Usage: {}%", st.usage_pct_));
        gpu_vram_lbl_b->setText(std::format("VRAM:  {} / {} MB",
            st.vram_used_mb_, st.vram_total_mb_));
        if (st.temp_c_ != INT32_MIN)
            gpu_temp_lbl_b->setText(std::format("Temp:  {}°C", st.temp_c_));
        else
            gpu_temp_lbl_b->setText("Temp:  —");
        gpu_clock_lbl->setText(std::format("Clock: {} MHz", st.clock_mhz_));
    };
}

// GPU Monitor (quick-settings card). Five stat labels + the update_gpu_ui
// closure that recolours utilisation green→yellow→red from a GpuStatsDetail.
// Behaviour is identical to the former inline block.
void buildGpuMonitorPanel(ShellContext& ctx) {
    ctx.gpu_name_lbl  = std::make_shared<TextWidget>("GPU", 11.0f);
    ctx.gpu_name_lbl->setColor({0.60f, 0.60f, 0.65f, 1.0f});
    ctx.gpu_util_lbl  = std::make_shared<TextWidget>("–%", 11.0f);
    ctx.gpu_vram_lbl  = std::make_shared<TextWidget>("", 10.0f);
    ctx.gpu_vram_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
    ctx.gpu_temp_lbl  = std::make_shared<TextWidget>("", 10.0f);
    ctx.gpu_temp_lbl->setColor({0.70f, 0.45f, 0.30f, 1.0f});
    ctx.gpu_fan_lbl   = std::make_shared<TextWidget>("", 10.0f);
    ctx.gpu_fan_lbl->setColor({0.40f, 0.55f, 0.70f, 1.0f});

    ctx.update_gpu_ui = [gpu_name_lbl = ctx.gpu_name_lbl, gpu_util_lbl = ctx.gpu_util_lbl,
                          gpu_vram_lbl = ctx.gpu_vram_lbl, gpu_temp_lbl = ctx.gpu_temp_lbl,
                          gpu_fan_lbl = ctx.gpu_fan_lbl](const GpuStatsDetail& st) {
        gpu_name_lbl->setText(std::string(st.name_));
        gpu_util_lbl->setText(std::format("{}%", st.util_pct_));
        // Color util: green→yellow→red
        float u = static_cast<float>(st.util_pct_) / 100.0f;
        gpu_util_lbl->setColor(u < 0.5f
            ? Color{0.30f + u * 0.80f, 0.75f, 0.30f, 1.0f}
            : Color{0.90f, 0.75f - (u - 0.5f) * 1.20f, 0.20f, 1.0f});
        gpu_vram_lbl->setText(std::format("{}/{}MB", st.vram_used_mb_, st.vram_total_mb_));
        gpu_temp_lbl->setText(std::format("{}°C", st.temp_c_));
        if (st.fan_rpm_ > 0)
            gpu_fan_lbl->setText(std::format("{} RPM", st.fan_rpm_));
        else
            gpu_fan_lbl->setText("passive");
    };
}

// Night Light (compositor colour-temperature control). Temp switch + slider +
// value label, plus a schedule sub-panel (switch + from/to time inputs + Apply).
// send_nl_temp / send_nl_schedule stay builder-local helper closures. Behaviour
// is identical to the former inline block.
void buildNightLightPanel(ShellContext& ctx) {
    ctx.nl_switch   = std::make_shared<SwitchWidget>(false);
    ctx.nl_temp_val = std::make_shared<TextWidget>("4000 K", 11.0f);
    ctx.nl_temp_val->setColor({0.72f, 0.72f, 0.72f, 1.0f});
    // Slider: 2700 K (warm amber) → 6500 K (neutral/cool)
    ctx.nl_slider = std::make_shared<SliderWidget>(4000.0f, 2700.0f, 6500.0f);

    auto send_nl_temp = [nl_slider = ctx.nl_slider]() {
        if (!g_shell_xbus) return;
        NightLightTempRequest req{};
        req.temperature_k_ = static_cast<uint32_t>(nl_slider->value());
        g_shell_xbus->Call("com.freebsdx.NightLight",
            static_cast<uint32_t>(NightLightOpcode::SetTemperature),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    };
    ctx.nl_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        NightLightEnableRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.NightLight",
            static_cast<uint32_t>(NightLightOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.nl_slider->setOnChanged([nl_temp_val = ctx.nl_temp_val, send_nl_temp](float v) {
        nl_temp_val->setText(std::format("{} K", static_cast<int>(v)));
        send_nl_temp();
    });

    ctx.nl_hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🌡", 15.0f),
        std::make_shared<Spacer>(8, 1),
        std::make_shared<TextWidget>("Night Light", 12.0f),
        std::make_shared<Spacer>(1, 1, true),
        ctx.nl_switch
    });
    ctx.nl_slider_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🌇", 15.0f),
        std::make_shared<Spacer>(8, 1),
        ctx.nl_slider,
        std::make_shared<Spacer>(8, 1),
        ctx.nl_temp_val
    });

    // Schedule: "From HH:MM  To HH:MM" time inputs + Apply button
    ctx.nl_sched_switch = std::make_shared<SwitchWidget>(false);
    ctx.nl_from_input   = std::make_shared<TextInputWidget>();
    ctx.nl_from_input->setPlaceholder("20:00");
    ctx.nl_from_input->setFixedSize(56, 28);
    ctx.nl_to_input = std::make_shared<TextInputWidget>();
    ctx.nl_to_input->setPlaceholder("07:00");
    ctx.nl_to_input->setFixedSize(56, 28);

    auto send_nl_schedule = [nl_sched_switch = ctx.nl_sched_switch,
                             nl_from_input = ctx.nl_from_input, nl_to_input = ctx.nl_to_input]() {
        if (!g_shell_xbus) return;
        NightLightScheduleRequest req{};
        req.enabled_ = nl_sched_switch->value() ? 1u : 0u;
        auto from = nl_from_input->getText();
        auto to   = nl_to_input->getText();
        std::strncpy(req.from_time_, from.c_str(), sizeof(req.from_time_) - 1);
        std::strncpy(req.to_time_,   to.c_str(),   sizeof(req.to_time_) - 1);
        g_shell_xbus->Call("com.freebsdx.NightLight",
            static_cast<uint32_t>(NightLightOpcode::SetSchedule),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    };
    ctx.nl_sched_switch->setOnChanged([send_nl_schedule](bool) { send_nl_schedule(); });

    ctx.nl_sched_hdr = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("Schedule", 11.0f),
        std::make_shared<Spacer>(1, 1, true),
        ctx.nl_sched_switch
    });
    ctx.nl_sched_times = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("From", 11.0f),
        std::make_shared<Spacer>(6, 1),
        ctx.nl_from_input,
        std::make_shared<Spacer>(12, 1),
        std::make_shared<TextWidget>("To", 11.0f),
        std::make_shared<Spacer>(6, 1),
        ctx.nl_to_input,
        std::make_shared<Spacer>(1, 1, true),
        std::make_shared<ButtonWidget>("Apply", [send_nl_schedule]() { send_nl_schedule(); })
    });

    ctx.update_nl_ui = [nl_switch = ctx.nl_switch, nl_slider = ctx.nl_slider,
                         nl_temp_val = ctx.nl_temp_val, nl_sched_switch = ctx.nl_sched_switch,
                         nl_from_input = ctx.nl_from_input, nl_to_input = ctx.nl_to_input]
                        (const NightLightState& st) {
        nl_switch->setValue(st.enabled_ != 0);
        nl_slider->setValue(static_cast<float>(st.temperature_k_));
        nl_temp_val->setText(std::format("{} K", st.temperature_k_));
        nl_sched_switch->setValue(st.schedule_enabled_ != 0);
        if (st.from_time_[0]) nl_from_input->setText(std::string(st.from_time_));
        if (st.to_time_[0])   nl_to_input->setText(std::string(st.to_time_));
    };
}

// Color Profile presets (quick-settings card). A 3-way cycle selector
// (sRGB/P3/Adobe) — shared index + button row + color_prof_hl highlight helper
// (stays builder-local) + update_color_prof_ui. Behaviour is identical to the
// former inline block.
void buildColorProfilePanel(ShellContext& ctx) {
    constexpr int kColorProfileCount = 3;
    static const char* kColorProfileLabels[] = {"sRGB", "P3", "Adobe"};
    ctx.color_prof_cur  = std::make_shared<uint32_t>(0u); // default sRGB
    ctx.color_prof_btns =
        std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kColorProfileCount>>();

    auto color_prof_hl = [color_prof_btns = ctx.color_prof_btns, color_prof_cur = ctx.color_prof_cur]() {
        for (int i = 0; i < kColorProfileCount; ++i) {
            bool active = (*color_prof_cur == static_cast<uint32_t>(i));
            (*color_prof_btns)[i]->setBackgroundColor(
                active ? Color{0.20f, 0.45f, 0.85f, 1.0f}
                       : Color{0.22f, 0.22f, 0.26f, 1.0f});
        }
    };

    for (int i = 0; i < kColorProfileCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kColorProfileLabels[i]);
        btn->setFixedSize(46, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});
        btn->onClick([i, color_prof_cur = ctx.color_prof_cur, color_prof_hl]() {
            *color_prof_cur = static_cast<uint32_t>(i);
            color_prof_hl();
            if (!g_shell_xbus) return;
            ColorProfileRequest req{};
            req.profile_ = static_cast<uint32_t>(i);
            g_shell_xbus->Call("com.freebsdx.ColorProfile",
                static_cast<uint32_t>(ColorProfileOpcode::SetProfile),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
        (*ctx.color_prof_btns)[i] = btn;
    }
    color_prof_hl();

    ctx.update_color_prof_ui = [color_prof_cur = ctx.color_prof_cur, color_prof_hl](const ColorProfileState& st) {
        *color_prof_cur = std::min(st.profile_, 2u);
        color_prof_hl();
    };

    ctx.color_prof_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("\xf0\x9f\x8e\xa8 Color", 12.0f),
        std::make_shared<Spacer>(1, 1, true),
        (*ctx.color_prof_btns)[0],
        std::make_shared<Spacer>(4, 1),
        (*ctx.color_prof_btns)[1],
        std::make_shared<Spacer>(4, 1),
        (*ctx.color_prof_btns)[2],
    });
}

// Display Scaling presets (quick-settings card). A 4-way cycle selector
// (1×/1.5×/2×/3×) keyed off kScaleValues — shared index + button row +
// disp_scale_hl highlight helper (stays builder-local) + update_disp_scale_ui.
// Behaviour is identical to the former inline block.
void buildDisplayScalePanel(ShellContext& ctx) {
    constexpr int kScaleCount = 4;
    static const char* kScaleLabels[]  = {"1×", "1.5×", "2×", "3×"};
    static const uint32_t kScaleValues[] = {100u, 150u, 200u, 300u};
    ctx.disp_scale_cur  = std::make_shared<uint32_t>(100u); // default 1×
    ctx.disp_scale_btns =
        std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kScaleCount>>();

    auto disp_scale_hl = [disp_scale_btns = ctx.disp_scale_btns, disp_scale_cur = ctx.disp_scale_cur]() {
        for (int i = 0; i < kScaleCount; ++i) {
            bool active = (*disp_scale_cur == kScaleValues[i]);
            (*disp_scale_btns)[i]->setBackgroundColor(
                active ? Color{0.20f, 0.45f, 0.85f, 1.0f}
                       : Color{0.22f, 0.22f, 0.26f, 1.0f});
        }
    };

    for (int i = 0; i < kScaleCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kScaleLabels[i]);
        btn->setFixedSize(i == 1 ? 40 : 34, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});
        btn->onClick([i, disp_scale_cur = ctx.disp_scale_cur, disp_scale_hl]() {
            *disp_scale_cur = kScaleValues[i];
            disp_scale_hl();
            if (!g_shell_xbus) return;
            DisplayScalePctRequest req{};
            req.scale_pct_ = kScaleValues[i];
            g_shell_xbus->Call("com.freebsdx.DisplayScale",
                static_cast<uint32_t>(DisplayScaleOpcode::SetScale),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
        (*ctx.disp_scale_btns)[i] = btn;
    }
    disp_scale_hl();

    ctx.update_disp_scale_ui = [disp_scale_cur = ctx.disp_scale_cur, disp_scale_hl](const DisplayScaleState& st) {
        *disp_scale_cur = st.scale_pct_;
        disp_scale_hl();
    };

    ctx.disp_scale_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("\xf0\x9f\x94\x8d Scale", 12.0f),
        std::make_shared<Spacer>(1, 1, true),
        (*ctx.disp_scale_btns)[0],
        std::make_shared<Spacer>(3, 1),
        (*ctx.disp_scale_btns)[1],
        std::make_shared<Spacer>(3, 1),
        (*ctx.disp_scale_btns)[2],
        std::make_shared<Spacer>(3, 1),
        (*ctx.disp_scale_btns)[3],
    });
}

// Display Refresh Rate presets (quick-settings card). A 4-way selector
// (60/120/144/240 Hz) that greys out unsupported rates via a supported_mask_;
// shared cur + mask + button row + rrate_hl highlight helper (stays
// builder-local) + update_rrate_ui. Behaviour is identical to the former inline
// block.
void buildRefreshRatePanel(ShellContext& ctx) {
    static const uint32_t kRrateHz[]   = {60, 120, 144, 240};
    static const char*    kRrateLabels[] = {"60", "120", "144", "240"};
    static const uint32_t kRrateMasks[] = {0x1, 0x2, 0x4, 0x8};
    constexpr int kRrateCount = 4;

    ctx.rrate_cur  = std::make_shared<uint32_t>(60);
    ctx.rrate_mask = std::make_shared<uint32_t>(0xF); // assume all supported until told otherwise
    ctx.rrate_btns = std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kRrateCount>>();
    for (int i = 0; i < kRrateCount; ++i)
        (*ctx.rrate_btns)[i] = std::make_shared<ButtonWidget>(kRrateLabels[i]);

    auto rrate_hl = [rrate_btns = ctx.rrate_btns, rrate_cur = ctx.rrate_cur, rrate_mask = ctx.rrate_mask]() {
        for (int i = 0; i < kRrateCount; ++i) {
            bool supported = ((*rrate_mask) & kRrateMasks[i]) != 0;
            bool active    = (*rrate_cur == kRrateHz[i]);
            (*rrate_btns)[i]->setBackgroundColor(
                active    ? Color{0.20f, 0.45f, 0.85f, 1.0f} :
                supported ? Color{0.22f, 0.22f, 0.26f, 1.0f} :
                            Color{0.14f, 0.14f, 0.16f, 0.6f});
        }
    };

    for (int i = 0; i < kRrateCount; ++i) {
        (*ctx.rrate_btns)[i]->setFontSize(11.0f);
        (*ctx.rrate_btns)[i]->setFixedSize(44, 24);
        uint32_t hz = kRrateHz[i];
        uint32_t mask = kRrateMasks[i];
        (*ctx.rrate_btns)[i]->onClick([hz, mask, rrate_cur = ctx.rrate_cur, rrate_mask = ctx.rrate_mask, rrate_hl]() {
            if (!((*rrate_mask) & mask)) return; // unsupported — ignore
            *rrate_cur = hz;
            rrate_hl();
            if (!g_shell_xbus) return;
            RefreshRateRequest req{};
            req.hz_ = hz;
            g_shell_xbus->Call("com.freebsdx.RefreshRate",
                static_cast<uint32_t>(RefreshRateOpcode::SetRate),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
    }
    rrate_hl();

    ctx.update_rrate_ui = [rrate_cur = ctx.rrate_cur, rrate_mask = ctx.rrate_mask, rrate_hl]
                            (const RefreshRateState& st) {
        *rrate_cur  = st.current_hz_;
        *rrate_mask = st.supported_mask_;
        rrate_hl();
    };

    ctx.rrate_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("Hz", 11.0f),
        std::make_shared<Spacer>(8, 1),
        (*ctx.rrate_btns)[0], std::make_shared<Spacer>(4, 1),
        (*ctx.rrate_btns)[1], std::make_shared<Spacer>(4, 1),
        (*ctx.rrate_btns)[2], std::make_shared<Spacer>(4, 1),
        (*ctx.rrate_btns)[3],
    });
}

// Text Size presets (quick-settings card). A 3-way cycle selector (A-/A/A+) —
// shared index + button array + text_size_hl highlight helper (stays
// builder-local) + update_text_size_ui. The text_size_row is assembled far
// from here (kept a main() local); this builder only creates the widgets +
// updater. Behaviour is identical to the former inline block.
void buildTextSizePanel(ShellContext& ctx) {
    constexpr int kTextSizeCount = 3;
    static const char* kTextSizeLabels[] = {"A-", "A", "A+"};
    ctx.text_size_cur  = std::make_shared<uint32_t>(1u); // default Normal
    ctx.text_size_btns = std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kTextSizeCount>>();

    auto text_size_hl = [text_size_btns = ctx.text_size_btns, text_size_cur = ctx.text_size_cur]() {
        for (int i = 0; i < kTextSizeCount; ++i) {
            bool active = (*text_size_cur == static_cast<uint32_t>(i));
            (*text_size_btns)[i]->setBackgroundColor(
                active ? Color{0.20f, 0.45f, 0.85f, 1.0f}
                       : Color{0.22f, 0.22f, 0.26f, 1.0f});
        }
    };

    for (int i = 0; i < kTextSizeCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kTextSizeLabels[i]);
        btn->setFixedSize(36, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});
        btn->onClick([i, text_size_cur = ctx.text_size_cur, text_size_hl]() {
            *text_size_cur = static_cast<uint32_t>(i);
            text_size_hl();
            if (!g_shell_xbus) return;
            TextSizeRequest req{};
            req.size_ = static_cast<uint32_t>(i);
            g_shell_xbus->Call("com.freebsdx.TextSize",
                static_cast<uint32_t>(TextSizeOpcode::SetSize),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
        (*ctx.text_size_btns)[i] = btn;
    }
    text_size_hl();

    ctx.update_text_size_ui = [text_size_cur = ctx.text_size_cur, text_size_hl](const TextSizeState& st) {
        *text_size_cur = std::min(st.size_, 2u);
        text_size_hl();
    };
}

// Auto-Brightness toggle (quick-settings card). A switch + ambient-lux sensor
// label; update_auto_br_ui mirrors an AutoBrightnessState into them. Behaviour
// is identical to the former inline block.
void buildAutoBrightnessPanel(ShellContext& ctx) {
    ctx.auto_br_switch  = std::make_shared<SwitchWidget>(false);
    ctx.auto_br_lux_lbl = std::make_shared<TextWidget>("Sensor: —", 11.0f);
    ctx.auto_br_lux_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

    ctx.update_auto_br_ui = [auto_br_switch = ctx.auto_br_switch, auto_br_lux_lbl = ctx.auto_br_lux_lbl]
                              (const AutoBrightnessState& st) {
        auto_br_switch->setValue(st.enabled_ != 0);
        if (st.lux_ == UINT32_MAX)
            auto_br_lux_lbl->setText("Sensor: unavailable");
        else
            auto_br_lux_lbl->setText(std::format("Sensor: {} lux", st.lux_));
    };

    ctx.auto_br_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        AutoBrightnessRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.AutoBrightness",
            static_cast<uint32_t>(AutoBrightnessOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });

    ctx.auto_br_hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("💡 Auto-Brightness", 12.0f),
        std::make_shared<Spacer>(1, 1, true),
        ctx.auto_br_switch,
    });
}

// Screen Timeout quick-selector (quick-settings card). A 4-preset selector
// (30s/2m/10m/Never) keyed off a builder-local kStoutPresets table; shared
// index + button row + stout_hl highlight helper (stays builder-local) +
// update_stout_ui that snaps to the nearest preset. Behaviour is identical to
// the former inline block.
void buildScreenTimeoutPanel(ShellContext& ctx) {
    struct StoutPreset { uint32_t secs; const char* label; };
    static const StoutPreset kStoutPresets[] = {
        {30,  "30s"}, {120, "2m"}, {600, "10m"}, {0, "Never"}
    };
    constexpr int kStoutCount = 4;
    ctx.stout_btns = std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kStoutCount>>();
    ctx.stout_cur  = std::make_shared<uint32_t>(600); // default 10 min

    auto stout_hl = [stout_btns = ctx.stout_btns, stout_cur = ctx.stout_cur]() {
        Color dim{0.22f, 0.22f, 0.26f, 1.0f};
        Color hi {0.25f, 0.45f, 0.72f, 1.0f};
        for (int j = 0; j < kStoutCount; ++j)
            (*stout_btns)[j]->setBackgroundColor(
                kStoutPresets[j].secs == *stout_cur ? hi : dim);
    };

    for (int j = 0; j < kStoutCount; ++j) {
        (*ctx.stout_btns)[j] = std::make_shared<ButtonWidget>(kStoutPresets[j].label);
        (*ctx.stout_btns)[j]->setFontSize(11.0f);
        (*ctx.stout_btns)[j]->setFixedSize(48, 24);
        uint32_t secs = kStoutPresets[j].secs;
        (*ctx.stout_btns)[j]->onClick([secs, stout_cur = ctx.stout_cur, stout_hl]() {
            if (!g_shell_xbus) return;
            *stout_cur = secs;
            stout_hl();
            ScreenTimeoutRequest req{};
            req.timeout_secs_ = secs;
            g_shell_xbus->Call("com.freebsdx.ScreenTimeout",
                static_cast<uint32_t>(ScreenTimeoutOpcode::SetTimeout),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
    }
    stout_hl();

    ctx.stout_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("⏱", 13.0f),
        std::make_shared<Spacer>(6, 1),
        (*ctx.stout_btns)[0], std::make_shared<Spacer>(4, 1),
        (*ctx.stout_btns)[1], std::make_shared<Spacer>(4, 1),
        (*ctx.stout_btns)[2], std::make_shared<Spacer>(4, 1),
        (*ctx.stout_btns)[3],
    });

    ctx.update_stout_ui = [stout_cur = ctx.stout_cur, stout_hl](const ScreenTimeoutState& st) {
        // Snap to nearest preset (or keep current if no match)
        for (int j = 0; j < kStoutCount; ++j) {
            if (kStoutPresets[j].secs == st.timeout_secs_) {
                *stout_cur = st.timeout_secs_;
                break;
            }
        }
        stout_hl();
    };
}

// Screen Rotation Lock (quick-settings card). Lock switch + orientation label;
// update_rot_ui mirrors a RotationLockState (orientation index → label via a
// builder-local kOrientLabels table). Behaviour is identical to the former
// inline block.
void buildRotationLockPanel(ShellContext& ctx) {
    static const char* kOrientLabels[] = { "Landscape", "Portrait", "Landscape ↕", "Portrait ↕" };
    ctx.rot_lock_switch  = std::make_shared<SwitchWidget>(false);
    ctx.rot_orient_lbl   = std::make_shared<TextWidget>("Landscape", 11.0f);
    ctx.rot_orient_lbl->setColor({0.65f, 0.65f, 0.70f, 1.0f});
    ctx.rot_lock_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        RotationLockRequest req{};
        req.locked_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.RotationLock",
            static_cast<uint32_t>(RotationLockOpcode::SetLocked),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.update_rot_ui = [rot_lock_switch = ctx.rot_lock_switch, rot_orient_lbl = ctx.rot_orient_lbl](const RotationLockState& st) {
        rot_lock_switch->setValue(st.locked_ != 0);
        uint32_t idx = st.orientation_ < 4 ? st.orientation_ : 0;
        rot_orient_lbl->setText(kOrientLabels[idx]);
    };
}

void buildIdleLockPanel(ShellContext& ctx) {
    struct IdleStep { const char* label; uint32_t secs; };
    static constexpr IdleStep kIdleSteps[] = {
        {"Never", 0}, {"1 min", 60}, {"5 min", 300}, {"10 min", 600}, {"30 min", 1800}
    };
    auto idle_step = std::make_shared<int>(2); // index 2 = 5 min (matches g_idle_lock_secs default)
    auto idle_lock_lbl = std::make_shared<TextWidget>("Auto-Lock: 5 min", 12.0f);
    idle_lock_lbl->setColor({0.75f, 0.75f, 0.80f, 1.0f});
    auto idle_lock_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("⏱", 14.0f),
        std::make_shared<Spacer>(8, 1),
        idle_lock_lbl,
        std::make_shared<Spacer>(1, 1, true),
        std::make_shared<TextWidget>("tap to change", 11.0f),
    });
    idle_lock_row->setPadding(2);

    ctx.idle_lock_btn = std::make_shared<ButtonWidget>(idle_lock_row,
        [idle_step, idle_lock_lbl]() {
            *idle_step = (*idle_step + 1) % 5;
            g_idle_lock_secs.store(kIdleSteps[*idle_step].secs);
            g_idle_ticks.store(0); // reset counter on threshold change
            idle_lock_lbl->setText(
                std::string("Auto-Lock: ") + kIdleSteps[*idle_step].label);
        });
}

// Night Shift Schedule UI. Same hour-stepper shape as the DND schedule: a
// "Schedule Night Shift" switch + From/To hour steppers (‹ / › around a two-digit
// hour label). The steppers mutate the file-scope g_ns_sched_start/g_ns_sched_end
// ints (by reference) and persist via saveNsSched; the switch toggles g_ns_sched_on.
// The make_ns_hour_dec/inc helpers and the four stepper buttons stay builder-local
// (held by ns_sched_times); switch/from-lbl/to-lbl/hdr/times escape. Behaviour
// identical to the former inline block.
void buildNightShiftSchedulePanel(ShellContext& ctx) {
    ctx.ns_sched_switch = std::make_shared<SwitchWidget>(false);
    ctx.ns_from_lbl = std::make_shared<TextWidget>("20", 14.0f);
    ctx.ns_to_lbl   = std::make_shared<TextWidget>("07", 14.0f);
    ctx.ns_from_lbl->setColor({0.90f, 0.90f, 0.90f, 1.0f});
    ctx.ns_to_lbl->setColor({0.90f, 0.90f, 0.90f, 1.0f});

    auto make_ns_hour_dec = [](int& hr, std::shared_ptr<TextWidget> lbl) {
        return std::make_shared<ButtonWidget>("‹", [&hr, lbl]() {
            hr = (hr + 23) % 24;
            char buf[4]; std::snprintf(buf, sizeof(buf), "%02d", hr);
            lbl->setText(buf); saveNsSched();
        });
    };
    auto make_ns_hour_inc = [](int& hr, std::shared_ptr<TextWidget> lbl) {
        return std::make_shared<ButtonWidget>("›", [&hr, lbl]() {
            hr = (hr + 1) % 24;
            char buf[4]; std::snprintf(buf, sizeof(buf), "%02d", hr);
            lbl->setText(buf); saveNsSched();
        });
    };

    auto ns_from_dec = make_ns_hour_dec(g_ns_sched_start, ctx.ns_from_lbl);
    auto ns_from_inc = make_ns_hour_inc(g_ns_sched_start, ctx.ns_from_lbl);
    auto ns_to_dec   = make_ns_hour_dec(g_ns_sched_end,   ctx.ns_to_lbl);
    auto ns_to_inc   = make_ns_hour_inc(g_ns_sched_end,   ctx.ns_to_lbl);

    ctx.ns_sched_switch->setOnChanged([](bool on) {
        g_ns_sched_on = on; saveNsSched();
    });

    ctx.ns_sched_hdr = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🌙", 14.0f),
        std::make_shared<Spacer>(8, 1),
        std::make_shared<TextWidget>("Schedule Night Shift", 12.0f),
        std::make_shared<Spacer>(1, 1, true),
        ctx.ns_sched_switch
    });
    ctx.ns_sched_times = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<Spacer>(22, 1),
        std::make_shared<TextWidget>("From", 11.0f),
        std::make_shared<Spacer>(6, 1),
        ns_from_dec, std::make_shared<Spacer>(4, 1),
        ctx.ns_from_lbl, std::make_shared<Spacer>(4, 1),
        ns_from_inc,
        std::make_shared<Spacer>(10, 1),
        std::make_shared<TextWidget>("To", 11.0f),
        std::make_shared<Spacer>(6, 1),
        ns_to_dec, std::make_shared<Spacer>(4, 1),
        ctx.ns_to_lbl, std::make_shared<Spacer>(4, 1),
        ns_to_inc,
    });
}

// Per-workspace wallpaper strip. A header + a row of 4 workspace thumbnail buttons
// (WS 1..4), the current one highlighted (ws_wp_update_hl, keyed off the file-scope
// g_current_ws). Each button invokes ws_wp_open_fn(i) — a shared_ptr<function>
// whose body is wired later in OnConnect (once the file-chooser overlay exists), so
// the builder just creates the empty holder; the per-button lambdas capture it.
// Behaviour identical to the former inline block.
void buildWorkspaceWallpaperPanel(ShellContext& ctx) {
    // ws_wp_open_fn is wired in OnConnect once fc_overlay, rebuild_fc, etc. are ready.
    ctx.ws_wp_open_fn = std::make_shared<std::function<void(int)>>();

    // 4 workspace thumbnail buttons; current workspace highlighted
    ctx.ws_wp_btns = std::make_shared<std::array<std::shared_ptr<ButtonWidget>, 4>>();
    ctx.ws_wp_update_hl = std::make_shared<std::function<void()>>();
    *ctx.ws_wp_update_hl = [ws_wp_btns = ctx.ws_wp_btns]() {
        for (int i = 0; i < 4; ++i) {
            bool active = (i == g_current_ws);
            (*ws_wp_btns)[i]->setBackgroundColor(active
                ? Color{0.22f, 0.44f, 0.88f, 0.95f}
                : Color{0.14f, 0.16f, 0.24f, 1.0f});
        }
    };

    ctx.ws_wp_hdr_lbl = std::make_shared<TextWidget>("🖼 Workspace Wallpapers", 13.0f);
    ctx.ws_wp_row     = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{});
    for (int i = 0; i < 4; ++i) {
        int wi = i;
        auto lbl = std::make_shared<TextWidget>("WS " + std::to_string(i + 1), 11.0f);
        auto btn = std::make_shared<ButtonWidget>(lbl,
            [wi, ws_wp_open_fn = ctx.ws_wp_open_fn]() {
                if (g_shell_xbus && *ws_wp_open_fn) (*ws_wp_open_fn)(wi);
            });
        btn->setFixedSize(68, 36);
        btn->setBackgroundColor({0.14f, 0.16f, 0.24f, 1.0f});
        (*ctx.ws_wp_btns)[i] = btn;
        ctx.ws_wp_row->addChild(btn);
        if (i < 3) ctx.ws_wp_row->addChild(std::make_shared<Spacer>(4, 1));
    }
    (*ctx.ws_wp_update_hl)();
}

// Color-theme presets (cross-coupled). A row of 5 preset label-buttons (Ocean/
// Midnight/Dusk/Sakura/Day); apply_tp highlights the active one, persists
// g_theme_preset via saveThemePreset, posts a ThemeSetRequest, and mirrors the
// dark flag into the sibling qs_dark_switch. qs_dark_switch is a main()-local
// passed by const-ref. The kPresets table stays a builder-local static; the
// per-button lambdas capture the escaping apply_tp std::function. Behaviour
// identical to the former inline block.
void buildColorThemePanel(ShellContext& ctx,
                                 const std::shared_ptr<SwitchWidget>& qs_dark_switch) {
    struct ThemePreset { const char* emoji; const char* name; uint32_t dark; uint32_t accent; };
    static constexpr ThemePreset kPresets[5] = {
        {"🌊", "Ocean",    1u, 0u},  // dark + blue
        {"🌙", "Midnight", 1u, 1u},  // dark + purple
        {"🌆", "Dusk",     1u, 2u},  // dark + orange
        {"🌸", "Sakura",   0u, 1u},  // light + purple
        {"☀️",  "Day",      0u, 0u},  // light + blue
    };

    ctx.tp_labels = std::make_shared<std::array<std::shared_ptr<TextWidget>, 5>>();
    ctx.tp_idx    = std::make_shared<int>(-1);

    ctx.apply_tp = [tp_labels = ctx.tp_labels, tp_idx = ctx.tp_idx, qs_dark_switch](int idx) {
        // Highlight active preset, dim others
        for (int i = 0; i < 5; ++i) {
            auto& lbl = (*tp_labels)[i];
            if (!lbl) continue;
            lbl->setColor(i == idx
                ? Color{1.0f, 0.85f, 0.3f, 1.0f}
                : Color{0.70f, 0.70f, 0.75f, 1.0f});
        }
        *tp_idx = idx;
        g_theme_preset = idx;
        saveThemePreset();
        if (!g_shell_xbus || idx < 0) return;
        ThemeSetRequest req{};
        req.dark_mode_  = kPresets[idx].dark;
        req.accent_idx_ = kPresets[idx].accent;
        g_shell_xbus->Call("com.freebsdx.Theme",
            static_cast<uint32_t>(ThemeOpcode::SetTheme),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        g_accent_idx = kPresets[idx].accent;
        qs_dark_switch->setValue(kPresets[idx].dark != 0);
    };

    std::vector<std::shared_ptr<Widget>> tp_btn_widgets;
    for (int i = 0; i < 5; ++i) {
        auto lbl = std::make_shared<TextWidget>(
            std::string(kPresets[i].emoji) + " " + kPresets[i].name, 11.0f);
        lbl->setColor({0.70f, 0.70f, 0.75f, 1.0f});
        (*ctx.tp_labels)[i] = lbl;
        auto btn = std::make_shared<ButtonWidget>(lbl, [i, apply_tp = ctx.apply_tp](){ apply_tp(i); });
        btn->setPadding(4);
        tp_btn_widgets.push_back(btn);
        if (i < 4) tp_btn_widgets.push_back(std::make_shared<Spacer>(4, 1));
    }
    ctx.theme_presets_row = std::make_shared<FlexRow>(tp_btn_widgets);
}
