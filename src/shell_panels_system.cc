// ── xshell panel builders: system administration: services, jobs, logs, packages, jails, VMs, security ──
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

// Cron Jobs (paginated Fetcher list). Header + result column + Refresh button;
// rebuild_cron pages com.freebsdx.CronJobs ListJobs (per-job schedule/command
// rows with an enable switch + Run-now button). The add_entry row-builder stays
// a local closure. Behaviour is identical to the former inline block.
void buildCronJobsPanel(ShellContext& ctx) {
    ctx.cron_hdr_lbl = std::make_shared<TextWidget>("⏰ Cron Jobs", 14.0f);
    ctx.cron_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});

    ctx.cron_col = std::make_shared<FlexColumn>();

    ctx.rebuild_cron = std::make_shared<std::function<void()>>();
    *ctx.rebuild_cron = [cron_col = ctx.cron_col,
                     rcron_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_cron)]() {
        if (!g_shell_xbus) return;
        uint32_t page = 0;
        g_shell_xbus->Call("com.freebsdx.CronJobs",
            static_cast<uint32_t>(CronOpcode::ListJobs),
            {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
            [cron_col, rcron_weak](const XBusMessage& r) {
                CronPage pg{};
                std::memcpy(&pg, r.payload_, sizeof(pg));
                cron_col->clearChildren();
                if (!pg.count_) {
                    auto empty = std::make_shared<TextWidget>("No cron jobs", 11.0f);
                    empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                    cron_col->addChild(empty);
                    return;
                }
                auto add_entry = [cron_col](const CronEntry& e) {
                    std::string sched(e.schedule_);
                    std::string cmd(e.command_);
                    uint32_t jid = e.job_id_;
                    bool enabled = e.enabled_ != 0;

                    auto sched_lbl = std::make_shared<TextWidget>(sched, 10.0f);
                    sched_lbl->setColor({0.55f, 0.80f, 0.55f, 1.0f});
                    auto cmd_lbl = std::make_shared<TextWidget>(cmd, 11.0f);
                    cmd_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                    auto en_sw = std::make_shared<SwitchWidget>(enabled);
                    en_sw->setOnChanged([jid](bool on) {
                        if (!g_shell_xbus) return;
                        CronJobToggleRequest req{};
                        req.job_id_  = jid;
                        req.enabled_ = on ? 1u : 0u;
                        g_shell_xbus->Call("com.freebsdx.CronJobs",
                            static_cast<uint32_t>(CronOpcode::SetJobEnabled),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                            nullptr);
                    });

                    auto run_btn = std::make_shared<ButtonWidget>("▶ Run",
                        [jid]() {
                            if (!g_shell_xbus) return;
                            CronRunRequest req{};
                            req.job_id_ = jid;
                            g_shell_xbus->Call("com.freebsdx.CronJobs",
                                static_cast<uint32_t>(CronOpcode::RunNow),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                nullptr);
                        });
                    run_btn->setFontSize(10.0f);
                    run_btn->setFixedSize(54, 22);
                    run_btn->setBackgroundColor({0.14f, 0.35f, 0.14f, 1.0f});

                    auto ctrl_row = std::make_shared<FlexRow>(
                        std::vector<std::shared_ptr<Widget>>{
                            en_sw,
                            std::make_shared<Spacer>(8, 1),
                            run_btn,
                        });

                    auto entry_col = std::make_shared<FlexColumn>(
                        std::vector<std::shared_ptr<Widget>>{
                            std::make_shared<FlexRow>(
                                std::vector<std::shared_ptr<Widget>>{
                                    std::make_shared<FlexColumn>(
                                        std::vector<std::shared_ptr<Widget>>{
                                            sched_lbl, cmd_lbl }),
                                    std::make_shared<Spacer>(1, 1, true),
                                    ctrl_row,
                                }),
                        });
                    entry_col->setBackgroundColor({0.10f, 0.12f, 0.18f, 1.0f});
                    entry_col->setPadding(6);
                    cron_col->addChild(entry_col);
                };
                for (uint32_t i = 0; i < pg.count_; ++i)
                    add_entry(pg.entries_[i]);
                if (pg.has_more_) {
                    for (uint32_t np = 1; np < (pg.total_ + 2) / 3; ++np) {
                        uint32_t p2 = np;
                        if (!g_shell_xbus) break;
                        g_shell_xbus->Call("com.freebsdx.CronJobs",
                            static_cast<uint32_t>(CronOpcode::ListJobs),
                            {reinterpret_cast<const uint8_t*>(&p2), sizeof(p2)},
                            [cron_col, add_entry](const XBusMessage& r2) {
                                CronPage pg2{};
                                std::memcpy(&pg2, r2.payload_, sizeof(pg2));
                                for (uint32_t j = 0; j < pg2.count_; ++j)
                                    add_entry(pg2.entries_[j]);
                            });
                    }
                }
            });
    };

    ctx.cron_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh", [rebuild_cron = ctx.rebuild_cron]() {
        if (rebuild_cron && *rebuild_cron) (*rebuild_cron)();
    });
    ctx.cron_refresh_btn->setFixedSize(84, 26);
}

// Startup Items (paginated Fetcher list). Header + result column + Refresh button;
// rebuild_startup pages com.freebsdx.StartupItems ListItems (per-item enable
// switch + Remove button, greyed for builtin entries; toggle/remove replies
// re-trigger the rebuild). The per-row builder lives inside the fetch closure.
// Behaviour is identical to the former inline block.
void buildStartupItemsPanel(ShellContext& ctx) {
    ctx.startup_hdr_lbl = std::make_shared<TextWidget>("🚀 Startup Items", 14.0f);
    ctx.startup_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.startup_col = std::make_shared<FlexColumn>();

    ctx.rebuild_startup = std::make_shared<std::function<void()>>();
    *ctx.rebuild_startup = [startup_col = ctx.startup_col,
                        rs_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_startup)]() {
        if (!g_shell_xbus) return;
        startup_col->clearChildren();
        auto fetch = std::make_shared<std::function<void(uint32_t)>>();
        *fetch = [startup_col, fetch_weak = std::weak_ptr<std::function<void(uint32_t)>>(fetch),
                  rs_weak](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg = page;
            g_shell_xbus->Call("com.freebsdx.StartupItems",
                static_cast<uint32_t>(StartupOpcode::ListItems),
                {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
                [startup_col, fetch_weak, rs_weak](const XBusMessage& r) {
                    StartupPage sp{};
                    std::memcpy(&sp, r.payload_, sizeof(sp));
                    for (uint32_t i = 0; i < sp.count_; ++i) {
                        const auto& e = sp.entries_[i];
                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(e.name_), 12.0f);
                        auto exec_lbl = std::make_shared<TextWidget>(
                            std::string(e.exec_), 10.0f);
                        exec_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
                        auto en_sw = std::make_shared<SwitchWidget>(e.enabled_ != 0);
                        uint32_t item_id = e.item_id_;
                        en_sw->setOnChanged([item_id, rs_weak](bool on) {
                            if (!g_shell_xbus) return;
                            StartupToggleRequest req{};
                            req.item_id_ = item_id;
                            req.enabled_ = on ? 1u : 0u;
                            g_shell_xbus->Call("com.freebsdx.StartupItems",
                                static_cast<uint32_t>(StartupOpcode::SetEnabled),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                [rs_weak](const XBusMessage&) {
                                    if (auto rs = rs_weak.lock())
                                        if (*rs) (*rs)();
                                });
                        });
                        // Remove button — greyed for builtin entries
                        bool builtin = e.builtin_ != 0;
                        auto rm_btn = std::make_shared<ButtonWidget>("⊗");
                        rm_btn->setFontSize(11.0f);
                        rm_btn->setFixedSize(28, 22);
                        rm_btn->setBackgroundColor(builtin
                            ? Color{0.25f, 0.25f, 0.28f, 1.0f}
                            : Color{0.55f, 0.18f, 0.18f, 1.0f});
                        if (!builtin) {
                            // Deletes the .desktop file: the first click only
                            // arms the button ("Confirm?"), the second removes.
                            auto armed = std::make_shared<bool>(false);
                            std::weak_ptr<ButtonWidget> rm_weak = rm_btn;
                            rm_btn->onClick([item_id, rs_weak, armed, rm_weak]() {
                                if (!g_shell_xbus) return;
                                if (!*armed) {
                                    *armed = true;
                                    if (auto b = rm_weak.lock()) {
                                        b->setText("Confirm?");
                                        b->setFixedSize(60, 22);
                                    }
                                    return;
                                }
                                StartupRemoveRequest req{};
                                req.item_id_ = item_id;
                                g_shell_xbus->Call("com.freebsdx.StartupItems",
                                    static_cast<uint32_t>(StartupOpcode::RemoveItem),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    [rs_weak](const XBusMessage&) {
                                        if (auto rs = rs_weak.lock())
                                            if (*rs) (*rs)();
                                    });
                            });
                        }
                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl,
                                std::make_shared<Spacer>(1, 1, true),
                                exec_lbl,
                                std::make_shared<Spacer>(6, 1),
                                en_sw,
                                std::make_shared<Spacer>(4, 1),
                                rm_btn,
                            });
                        startup_col->addChild(row);
                        startup_col->addChild(std::make_shared<Spacer>(1, 4));
                    }
                    if (sp.has_more_) {
                        if (auto f = fetch_weak.lock())
                            if (*f) (*f)(sp.page_ + 1);
                    }
                });
        };
        if (*fetch) (*fetch)(0);
    };

    ctx.startup_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh",
        [rebuild_startup = ctx.rebuild_startup]() {
            if (rebuild_startup && *rebuild_startup) (*rebuild_startup)();
        });
    ctx.startup_refresh_btn->setFixedSize(84, 26);
}

// System Logs (paginated Fetcher list). Header + result column + a client-side
// level filter (0=All/1=Error/2=Warn) driven by a 3-button row (with a highlight
// helper) + Refresh button; rebuild_syslog pages com.freebsdx.SysLog GetEntries
// (per-entry level-badge/source header row + message, filtered client-side). The
// filter state, the 3 filter buttons, and the highlight helper stay builder-local
// (held alive by the escaping syslog_filter_row). Behaviour identical to the
// former inline block.
void buildSysLogPanel(ShellContext& ctx) {
    ctx.syslog_hdr_lbl   = std::make_shared<TextWidget>("📋 System Logs", 14.0f);
    ctx.syslog_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.syslog_col       = std::make_shared<FlexColumn>();

    // Client-side level filter: 0=All, 1=Error, 2=Warn
    auto syslog_filter    = std::make_shared<uint32_t>(0);

    // Filter button row
    auto syslog_btn_all   = std::make_shared<ButtonWidget>("⚪ All");
    auto syslog_btn_err   = std::make_shared<ButtonWidget>("🔴 Error");
    auto syslog_btn_warn  = std::make_shared<ButtonWidget>("🟡 Warn");
    for (auto* b : {syslog_btn_all.get(), syslog_btn_err.get(), syslog_btn_warn.get()})
        b->setFontSize(11.0f);
    syslog_btn_all->setFixedSize(62, 24);
    syslog_btn_err->setFixedSize(70, 24);
    syslog_btn_warn->setFixedSize(70, 24);

    ctx.rebuild_syslog = std::make_shared<std::function<void()>>();

    // Highlight helper: brighten the active filter button
    auto syslog_hl = [syslog_btn_all, syslog_btn_err, syslog_btn_warn, syslog_filter]() {
        Color dim{0.22f, 0.22f, 0.26f, 1.0f};
        Color hi {0.30f, 0.45f, 0.70f, 1.0f};
        syslog_btn_all ->setBackgroundColor(*syslog_filter == 0 ? hi : dim);
        syslog_btn_err ->setBackgroundColor(*syslog_filter == 1 ? hi : dim);
        syslog_btn_warn->setBackgroundColor(*syslog_filter == 2 ? hi : dim);
    };
    syslog_hl(); // set initial highlight

    *ctx.rebuild_syslog = [syslog_col = ctx.syslog_col, syslog_filter,
                       rs_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_syslog)]() {
        if (!g_shell_xbus) return;
        syslog_col->clearChildren();
        auto fetch = std::make_shared<std::function<void(uint32_t)>>();
        *fetch = [syslog_col, syslog_filter,
                  fetch_weak = std::weak_ptr<std::function<void(uint32_t)>>(fetch)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg = page;
            g_shell_xbus->Call("com.freebsdx.SysLog",
                static_cast<uint32_t>(SyslogOpcode::GetEntries),
                {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
                [syslog_col, syslog_filter, fetch_weak](const XBusMessage& r) {
                    SyslogPage sp{};
                    std::memcpy(&sp, r.payload_, sizeof(sp));
                    for (uint32_t i = 0; i < sp.count_; ++i) {
                        const auto& e = sp.entries_[i];
                        // Client-side filter: skip entries that don't match
                        std::string_view lvl{e.level_};
                        bool is_err  = (lvl == "ERROR");
                        bool is_warn = (lvl == "WARN");
                        if (*syslog_filter == 1 && !is_err)  continue;
                        if (*syslog_filter == 2 && !is_warn) continue;

                        // Level badge colour
                        Color lvl_color = is_err
                            ? Color{0.90f, 0.35f, 0.35f, 1.0f}
                            : (is_warn ? Color{0.90f, 0.75f, 0.20f, 1.0f}
                                       : Color{0.55f, 0.55f, 0.60f, 1.0f});
                        auto lvl_lbl = std::make_shared<TextWidget>(
                            std::string(e.level_), 10.0f);
                        lvl_lbl->setColor(lvl_color);
                        auto src_lbl = std::make_shared<TextWidget>(
                            std::string(e.source_), 10.0f);
                        src_lbl->setColor({0.50f, 0.65f, 0.90f, 1.0f});
                        auto msg_lbl = std::make_shared<TextWidget>(
                            std::string(e.message_), 11.0f);
                        auto hdr_row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                lvl_lbl,
                                std::make_shared<Spacer>(6, 1),
                                src_lbl,
                            });
                        syslog_col->addChild(hdr_row);
                        syslog_col->addChild(msg_lbl);
                        syslog_col->addChild(std::make_shared<Spacer>(1, 6));
                    }
                    if (sp.has_more_) {
                        if (auto f = fetch_weak.lock())
                            if (*f) (*f)(sp.page_ + 1);
                    }
                });
        };
        if (*fetch) (*fetch)(0);
    };

    syslog_btn_all->onClick([syslog_filter, syslog_hl, rebuild_syslog = ctx.rebuild_syslog]() {
        *syslog_filter = 0; syslog_hl();
        if (rebuild_syslog && *rebuild_syslog) (*rebuild_syslog)();
    });
    syslog_btn_err->onClick([syslog_filter, syslog_hl, rebuild_syslog = ctx.rebuild_syslog]() {
        *syslog_filter = 1; syslog_hl();
        if (rebuild_syslog && *rebuild_syslog) (*rebuild_syslog)();
    });
    syslog_btn_warn->onClick([syslog_filter, syslog_hl, rebuild_syslog = ctx.rebuild_syslog]() {
        *syslog_filter = 2; syslog_hl();
        if (rebuild_syslog && *rebuild_syslog) (*rebuild_syslog)();
    });

    ctx.syslog_filter_row = std::make_shared<FlexRow>(
        std::vector<std::shared_ptr<Widget>>{
            syslog_btn_all,
            std::make_shared<Spacer>(4, 1),
            syslog_btn_err,
            std::make_shared<Spacer>(4, 1),
            syslog_btn_warn,
        });

    ctx.syslog_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh",
        [rebuild_syslog = ctx.rebuild_syslog]() {
            if (rebuild_syslog && *rebuild_syslog) (*rebuild_syslog)();
        });
    ctx.syslog_refresh_btn->setFixedSize(84, 26);
}

// Login Sessions (paginated Fetcher list). Header + result column + Refresh
// button; rebuild_lsess pages com.freebsdx.Sessions ListSessions (per-session
// icon-by-type/user + source/elapsed row + Log Out button that kills the
// session). The per-row builders (inline for page 0, duplicated for pages 1+)
// stay inside the reply. Behaviour identical to the former inline block.
void buildLoginSessionsPanel(ShellContext& ctx) {
    ctx.lsess_hdr_lbl = std::make_shared<TextWidget>("👤 Login Sessions", 14.0f);
    ctx.lsess_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});

    ctx.lsess_col = std::make_shared<FlexColumn>();

    ctx.rebuild_lsess = std::make_shared<std::function<void()>>();
    *ctx.rebuild_lsess = [lsess_col = ctx.lsess_col,
                      rlsess_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_lsess)]() {
        if (!g_shell_xbus) return;
        uint32_t page = 0;
        g_shell_xbus->Call("com.freebsdx.Sessions",
            static_cast<uint32_t>(SessionOpcode::ListSessions),
            {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
            [lsess_col, rlsess_weak](const XBusMessage& r) {
                SessionPage pg{};
                std::memcpy(&pg, r.payload_, sizeof(pg));
                lsess_col->clearChildren();
                if (!pg.count_) {
                    auto empty = std::make_shared<TextWidget>("No active sessions", 11.0f);
                    empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                    lsess_col->addChild(empty);
                    return;
                }
                for (uint32_t i = 0; i < pg.count_; ++i) {
                    const auto& e = pg.entries_[i];
                    // Icon by session type
                    std::string icon = "📟";
                    if (std::strncmp(e.type_, "graphical", 9) == 0) icon = "🖥";
                    else if (std::strncmp(e.type_, "ssh", 3) == 0) icon = "🔒";
                    // Elapsed time since login
                    time_t now = time(nullptr);
                    int64_t elapsed = static_cast<int64_t>(now) -
                                      static_cast<int64_t>(e.login_time_);
                    elapsed = std::max<int64_t>(elapsed, 0);
                    int64_t h = elapsed / 3600;
                    int64_t m = (elapsed % 3600) / 60;
                    std::string age = (h > 0)
                        ? std::format("{}h {}m ago", h, m)
                        : std::format("{}m ago", m);
                    // User + source label
                    std::string user(e.user_);
                    std::string src(e.source_);
                    auto icon_lbl = std::make_shared<TextWidget>(icon + " " + user, 12.0f);
                    icon_lbl->setColor({0.90f, 0.90f, 0.95f, 1.0f});
                    auto src_lbl = std::make_shared<TextWidget>(
                        src + "  " + age, 10.0f);
                    src_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
                    uint32_t sid = e.session_id_;
                    auto logout_btn = std::make_shared<ButtonWidget>("⏏ Log Out",
                        [sid]() {
                            if (!g_shell_xbus) return;
                            SessionKillRequest req{};
                            req.session_id_ = sid;
                            g_shell_xbus->Call("com.freebsdx.Sessions",
                                static_cast<uint32_t>(SessionOpcode::KillSession),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                nullptr);
                        });
                    logout_btn->setFontSize(10.0f);
                    logout_btn->setFixedSize(78, 22);
                    logout_btn->setBackgroundColor({0.45f, 0.12f, 0.12f, 1.0f});
                    auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                        std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
                            icon_lbl, src_lbl }),
                        std::make_shared<Spacer>(1, 1, true),
                        logout_btn
                    });
                    row->setBackgroundColor({0.10f, 0.12f, 0.18f, 1.0f});
                    row->setPadding(6);
                    lsess_col->addChild(row);
                }
                // Fetch remaining pages if any
                if (pg.has_more_) {
                    // Pages beyond the first are fetched by re-calling for page 1+
                    for (uint32_t np = 1; np < (pg.total_ + 2) / 3; ++np) {
                        uint32_t p2 = np;
                        if (!g_shell_xbus) break;
                        g_shell_xbus->Call("com.freebsdx.Sessions",
                            static_cast<uint32_t>(SessionOpcode::ListSessions),
                            {reinterpret_cast<const uint8_t*>(&p2), sizeof(p2)},
                            [lsess_col](const XBusMessage& r2) {
                                SessionPage pg2{};
                                std::memcpy(&pg2, r2.payload_, sizeof(pg2));
                                for (uint32_t j = 0; j < pg2.count_; ++j) {
                                    const auto& e2 = pg2.entries_[j];
                                    std::string icon2 = "📟";
                                    if (std::strncmp(e2.type_, "graphical", 9) == 0)
                                        icon2 = "🖥";
                                    else if (std::strncmp(e2.type_, "ssh", 3) == 0)
                                        icon2 = "🔒";
                                    time_t now2 = time(nullptr);
                                    int64_t el2 = std::max<int64_t>(
                                        static_cast<int64_t>(now2) -
                                        static_cast<int64_t>(e2.login_time_), 0);
                                    int64_t h2 = el2/3600, m2 = (el2%3600)/60;
                                    std::string age2 = h2>0
                                        ? std::format("{}h {}m ago",h2,m2)
                                        : std::format("{}m ago",m2);
                                    std::string usr2(e2.user_), src2(e2.source_);
                                    auto lbl2 = std::make_shared<TextWidget>(
                                        icon2+" "+usr2, 12.0f);
                                    lbl2->setColor({0.90f,0.90f,0.95f,1.0f});
                                    auto slbl2 = std::make_shared<TextWidget>(
                                        src2+"  "+age2, 10.0f);
                                    slbl2->setColor({0.55f,0.55f,0.60f,1.0f});
                                    uint32_t sid2 = e2.session_id_;
                                    auto lb2 = std::make_shared<ButtonWidget>("⏏ Log Out",
                                        [sid2]() {
                                            if (!g_shell_xbus) return;
                                            SessionKillRequest req2{};
                                            req2.session_id_ = sid2;
                                            g_shell_xbus->Call("com.freebsdx.Sessions",
                                                static_cast<uint32_t>(SessionOpcode::KillSession),
                                                {reinterpret_cast<const uint8_t*>(&req2),
                                                 sizeof(req2)}, nullptr);
                                        });
                                    lb2->setFontSize(10.0f);
                                    lb2->setFixedSize(78, 22);
                                    lb2->setBackgroundColor({0.45f,0.12f,0.12f,1.0f});
                                    auto row2 = std::make_shared<FlexRow>(
                                        std::vector<std::shared_ptr<Widget>>{
                                            std::make_shared<FlexColumn>(
                                                std::vector<std::shared_ptr<Widget>>{
                                                    lbl2, slbl2 }),
                                            std::make_shared<Spacer>(1,1,true),
                                            lb2
                                        });
                                    row2->setBackgroundColor({0.10f,0.12f,0.18f,1.0f});
                                    row2->setPadding(6);
                                    lsess_col->addChild(row2);
                                }
                            });
                    }
                }
            });
    };

    ctx.lsess_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh", [rebuild_lsess = ctx.rebuild_lsess]() {
        if (rebuild_lsess && *rebuild_lsess) (*rebuild_lsess)();
    });
    ctx.lsess_refresh_btn->setFixedSize(84, 26);
}

// Package Updates (paginated Fetcher list). Header + count label + result column
// + "Upgrade All" and "Check" buttons; rebuild_pkg_upd uses a local PkgFetcher
// struct (col + count_lbl + all_btn + rebuild-weak + total + self-held recursive
// fetch) to page com.freebsdx.PkgUpdates ListUpdates (per-pkg name + old→new
// version + install button that re-triggers the rebuild). The action buttons'
// onClick handlers are wired BEFORE the rebuild body is assigned (they capture
// the rebuild_pkg_upd shared_ptr) — that order is preserved. The PkgFetcher
// struct stays inside the rebuild closure. Behaviour identical to the former
// inline block.
void buildPackageUpdatesPanel(ShellContext& ctx) {
    ctx.pkg_upd_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x93\xa6 Package Updates", 14.0f);
    ctx.pkg_upd_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.pkg_upd_count_lbl = std::make_shared<TextWidget>("Checking…", 11.0f);
    ctx.pkg_upd_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.pkg_upd_col       = std::make_shared<FlexColumn>();
    ctx.pkg_upd_col->setGap(4);

    ctx.pkg_upd_all_btn   = std::make_shared<ButtonWidget>("Upgrade All");
    ctx.pkg_upd_all_btn->setFixedSize(110, 26);
    ctx.pkg_upd_all_btn->setBackgroundColor({0.20f, 0.45f, 0.85f, 1.0f});

    ctx.pkg_upd_check_btn = std::make_shared<ButtonWidget>("\xe2\x86\xbb Check");
    ctx.pkg_upd_check_btn->setFixedSize(80, 26);

    ctx.rebuild_pkg_upd = std::make_shared<std::function<void()>>();

    ctx.pkg_upd_all_btn->onClick([rebuild_pkg_upd = ctx.rebuild_pkg_upd]() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.PkgUpdates",
            static_cast<uint32_t>(PkgOpcode::InstallAll), {}, nullptr);
        // compositor broadcasts UpdatesChanged when done → rebuild fires via OnBroadcast
    });

    ctx.pkg_upd_check_btn->onClick([rebuild_pkg_upd = ctx.rebuild_pkg_upd]() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.PkgUpdates",
            static_cast<uint32_t>(PkgOpcode::CheckNow), {}, nullptr);
    });

    *ctx.rebuild_pkg_upd = [pkg_upd_col = ctx.pkg_upd_col, pkg_upd_count_lbl = ctx.pkg_upd_count_lbl,
                        pkg_upd_all_btn = ctx.pkg_upd_all_btn,
                        rpu_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_pkg_upd)]() {
        if (!g_shell_xbus) return;
        pkg_upd_col->clearChildren();

        struct PkgFetcher {
            std::shared_ptr<FlexColumn> col;
            std::shared_ptr<TextWidget> count_lbl;
            std::shared_ptr<ButtonWidget> all_btn;
            std::weak_ptr<std::function<void()>> rebuild_weak;
            uint32_t total{0};
            std::function<void(uint32_t)> fetch;
        };
        auto pf = std::make_shared<PkgFetcher>();
        pf->col          = pkg_upd_col;
        pf->count_lbl    = pkg_upd_count_lbl;
        pf->all_btn      = pkg_upd_all_btn;
        pf->rebuild_weak = rpu_weak;

        pf->fetch = [pf](uint32_t page) {
            g_shell_xbus->Call("com.freebsdx.PkgUpdates",
                static_cast<uint32_t>(PkgOpcode::ListUpdates),
                {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
                [pf, page](const XBusMessage& r) {
                    PkgUpdatePage pg{};
                    std::memcpy(&pg, r.payload_, sizeof(pg));
                    if (pg.page_ == 0) pf->total = pg.total_;

                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        std::string pkg_name(e.name_);
                        std::string old_v(e.old_ver_);
                        std::string new_v(e.new_ver_);

                        auto name_lbl = std::make_shared<TextWidget>(pkg_name, 11.0f);
                        name_lbl->setColor({0.45f, 0.65f, 0.90f, 1.0f});

                        auto arrow_lbl = std::make_shared<TextWidget>(
                            old_v + " \xe2\x86\x92 ", 10.0f); // →
                        arrow_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        auto new_v_lbl = std::make_shared<TextWidget>(new_v, 10.0f);
                        new_v_lbl->setColor({0.35f, 0.80f, 0.45f, 1.0f});

                        auto install_btn = std::make_shared<ButtonWidget>("\xe2\x86\x93");
                        install_btn->setFixedSize(26, 22);
                        install_btn->setBackgroundColor({0.18f, 0.40f, 0.75f, 1.0f});
                        install_btn->onClick([pkg_name, pf]() {
                            if (!g_shell_xbus) return;
                            PkgInstallRequest req{};
                            std::strncpy(req.name_, pkg_name.c_str(), sizeof(req.name_) - 1);
                            g_shell_xbus->Call("com.freebsdx.PkgUpdates",
                                static_cast<uint32_t>(PkgOpcode::InstallPackage),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                [pf](const XBusMessage&) {
                                    if (auto rp = pf->rebuild_weak.lock())
                                        if (*rp) (*rp)();
                                });
                        });

                        auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                            name_lbl,
                            std::make_shared<Spacer>(6, 1),
                            arrow_lbl,
                            new_v_lbl,
                            std::make_shared<Spacer>(1, 1, true),
                            install_btn,
                        });
                        pf->col->addChild(row);
                        pf->col->addChild(std::make_shared<Spacer>(1, 3));
                    }

                    if (pg.has_more_) {
                        pf->fetch(page + 1);
                    } else {
                        uint32_t n = pf->total;
                        pf->count_lbl->setText(
                            n == 0 ? "Up to date"
                                   : std::format("{} pending", n));
                        pf->count_lbl->setColor(n == 0
                            ? Color{0.35f, 0.80f, 0.45f, 1.0f}
                            : Color{0.95f, 0.72f, 0.20f, 1.0f});
                        if (n == 0)
                            pf->col->addChild(std::make_shared<TextWidget>("All packages up to date.", 11.0f));
                    }
                });
        };
        pf->fetch(0);
    };
}

// Active Jails (paginated Fetcher list). Header + count label + result column +
// Refresh button; rebuild_jails uses a local JailFetcher struct (col + count_lbl
// + rebuild-weak + total + self-held recursive fetch) to page com.freebsdx.Jails
// ListJails (per-jail JID/hostname/IP + dying badge + Stop button that
// re-triggers the rebuild). The Refresh button's onClick is wired after the
// rebuild body is assigned. The JailFetcher struct stays inside the rebuild
// closure. Behaviour identical to the former inline block.
void buildActiveJailsPanel(ShellContext& ctx) {
    ctx.jail_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x94\x92 Jails", 14.0f);
    ctx.jail_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.jail_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.jail_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.jail_col       = std::make_shared<FlexColumn>();
    ctx.jail_col->setGap(4);

    ctx.jail_refresh_btn = std::make_shared<ButtonWidget>("\xe2\x86\xbb Refresh");
    ctx.jail_refresh_btn->setFixedSize(84, 26);

    ctx.rebuild_jails = std::make_shared<std::function<void()>>();
    *ctx.rebuild_jails = [jail_col = ctx.jail_col, jail_count_lbl = ctx.jail_count_lbl,
                      rj_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_jails)]() {
        if (!g_shell_xbus) return;
        jail_col->clearChildren();

        struct JailFetcher {
            std::shared_ptr<FlexColumn> col;
            std::shared_ptr<TextWidget> count_lbl;
            std::weak_ptr<std::function<void()>> rebuild_weak;
            uint32_t total{0};
            std::function<void(uint32_t)> fetch;
        };
        auto jf = std::make_shared<JailFetcher>();
        jf->col          = jail_col;
        jf->count_lbl    = jail_count_lbl;
        jf->rebuild_weak = rj_weak;

        jf->fetch = [jf](uint32_t page) {
            g_shell_xbus->Call("com.freebsdx.Jails",
                static_cast<uint32_t>(JailOpcode::ListJails),
                {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
                [jf, page](const XBusMessage& r) {
                    JailPage pg{};
                    std::memcpy(&pg, r.payload_, sizeof(pg));
                    if (pg.page_ == 0) jf->total = pg.total_;

                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        std::string hostname_str(e.hostname_);
                        std::string ip_str(e.ip_);
                        uint32_t    jid = e.jid_;

                        auto jid_lbl = std::make_shared<TextWidget>(
                            std::format("JID {}", jid), 10.0f);
                        jid_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        auto host_lbl = std::make_shared<TextWidget>(hostname_str, 11.0f);
                        host_lbl->setColor({0.45f, 0.65f, 0.90f, 1.0f});

                        auto ip_lbl = std::make_shared<TextWidget>(ip_str, 10.0f);
                        ip_lbl->setColor({0.60f, 0.60f, 0.65f, 1.0f});

                        auto dying_lbl = std::make_shared<TextWidget>(
                            e.running_ ? "" : "dying", 9.0f);
                        dying_lbl->setColor({0.90f, 0.30f, 0.30f, 1.0f});

                        auto stop_btn = std::make_shared<ButtonWidget>("Stop");
                        stop_btn->setFixedSize(48, 22);
                        stop_btn->setBackgroundColor({0.45f, 0.12f, 0.12f, 1.0f});
                        stop_btn->onClick([jid, jf]() {
                            if (!g_shell_xbus) return;
                            JailStopRequest req{};
                            req.jid_ = jid;
                            g_shell_xbus->Call("com.freebsdx.Jails",
                                static_cast<uint32_t>(JailOpcode::StopJail),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                [jf](const XBusMessage&) {
                                    if (auto rj = jf->rebuild_weak.lock())
                                        if (*rj) (*rj)();
                                });
                        });

                        auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                            jid_lbl,
                            std::make_shared<Spacer>(6, 1),
                            host_lbl,
                            std::make_shared<Spacer>(6, 1),
                            ip_lbl,
                            std::make_shared<Spacer>(1, 1, true),
                            dying_lbl,
                            std::make_shared<Spacer>(6, 1),
                            stop_btn,
                        });
                        jf->col->addChild(row);
                        jf->col->addChild(std::make_shared<Spacer>(1, 3));
                    }

                    if (pg.has_more_) {
                        jf->fetch(page + 1);
                    } else {
                        uint32_t n = jf->total;
                        jf->count_lbl->setText(
                            n == 0 ? "None running"
                                   : std::format("{} jail{}", n, n != 1 ? "s" : ""));
                        if (n == 0)
                            jf->col->addChild(std::make_shared<TextWidget>("No jails running.", 11.0f));
                    }
                });
        };
        jf->fetch(0);
    };

    ctx.jail_refresh_btn->onClick([rebuild_jails = ctx.rebuild_jails]() {
        if (rebuild_jails && *rebuild_jails) (*rebuild_jails)();
    });
}

// Boot Environments (paginated Fetcher list). Header + count label + result column
// + Refresh button; rebuild_be uses a local BeFetcher struct (col + count_lbl +
// rebuild-weak + total_seen + a self-held shared_ptr<function> fetch) to page
// com.freebsdx.BootEnv ListBE (per-BE name + Active/Next-Boot badge + created date
// + Activate/Delete buttons, greyed when not applicable; both actions re-trigger
// the rebuild). The Refresh button's onClick is wired after the rebuild body is
// assigned. The BeFetcher struct stays inside the rebuild closure. Behaviour
// identical to the former inline block.
void buildBootEnvPanel(ShellContext& ctx) {
    ctx.be_hdr_lbl     = std::make_shared<TextWidget>("\xf0\x9f\xa5\xbe Boot Environments", 14.0f);
    ctx.be_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.be_count_lbl   = std::make_shared<TextWidget>("", 11.0f);
    ctx.be_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.be_col         = std::make_shared<FlexColumn>();
    ctx.be_col->setGap(6);

    ctx.be_refresh_btn = std::make_shared<ButtonWidget>("\xe2\x86\xbb Refresh");
    ctx.be_refresh_btn->setFixedSize(84, 26);
    ctx.be_refresh_btn->setBackgroundColor({0.20f, 0.22f, 0.28f, 1.0f});

    ctx.rebuild_be = std::make_shared<std::function<void()>>();
    *ctx.rebuild_be = [be_col = ctx.be_col, be_count_lbl = ctx.be_count_lbl,
                   rb_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_be)]() {
        if (!g_shell_xbus) return;
        be_col->clearChildren();

        struct BeFetcher {
            std::shared_ptr<FlexColumn>                    col;
            std::shared_ptr<TextWidget>                    count_lbl;
            std::weak_ptr<std::function<void()>>           rebuild_weak;
            uint32_t                                       total_seen{0};
            std::shared_ptr<std::function<void(uint32_t)>> fetch;
        };
        auto bf = std::make_shared<BeFetcher>();
        bf->col          = be_col;
        bf->count_lbl    = be_count_lbl;
        bf->rebuild_weak = rb_weak;

        bf->fetch = std::make_shared<std::function<void(uint32_t)>>(
            [bf](uint32_t page) {
                if (!g_shell_xbus) return;
                uint32_t pg = page;
                g_shell_xbus->Call("com.freebsdx.BootEnv",
                    static_cast<uint32_t>(BeOpcode::ListBE),
                    {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
                    [bf, page](const XBusMessage& r) {
                        BePage bp{};
                        std::memcpy(&bp, r.payload_, sizeof(bp));
                        if (page == 0) {
                            bf->col->clearChildren();
                            bf->total_seen = 0;
                        }
                        bf->total_seen += bp.count_;

                        for (uint32_t i = 0; i < bp.count_; ++i) {
                            const BeEntry& e = bp.entries_[i];
                            std::string be_name(e.name_);
                            std::string created(e.created_);
                            bool active    = e.active_ != 0;
                            bool next_boot = e.next_boot_ != 0;

                            auto row = std::make_shared<FlexColumn>();
                            row->setGap(3);

                            // Name row: name + active/next badge
                            auto name_row = std::make_shared<FlexRow>();
                            name_row->setGap(6);

                            auto name_lbl = std::make_shared<TextWidget>(be_name, 12.0f);
                            name_lbl->setColor({0.40f, 0.65f, 0.95f, 1.0f});
                            name_row->addChild(name_lbl);

                            if (active) {
                                auto badge = std::make_shared<TextWidget>("Active", 10.0f);
                                badge->setColor({0.35f, 0.80f, 0.45f, 1.0f});
                                name_row->addChild(badge);
                            } else if (next_boot) {
                                auto badge = std::make_shared<TextWidget>("Next Boot", 10.0f);
                                badge->setColor({0.35f, 0.75f, 0.90f, 1.0f});
                                name_row->addChild(badge);
                            }
                            name_row->addChild(std::make_shared<Spacer>(1, 1, true));

                            // Created date
                            auto date_lbl = std::make_shared<TextWidget>(created, 10.0f);
                            date_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                            // Action buttons row
                            auto btn_row = std::make_shared<FlexRow>();
                            btn_row->setGap(6);

                            auto act_btn = std::make_shared<ButtonWidget>("Activate");
                            act_btn->setFixedSize(70, 22);
                            if (active || next_boot) {
                                act_btn->setBackgroundColor({0.25f, 0.25f, 0.28f, 1.0f});
                                act_btn->setColor({0.45f, 0.45f, 0.50f, 1.0f});
                            } else {
                                act_btn->setBackgroundColor({0.15f, 0.35f, 0.70f, 1.0f});
                                act_btn->onClick([be_name, bf]() {
                                    if (!g_shell_xbus) return;
                                    BeActionRequest req{};
                                    std::strncpy(req.name_, be_name.c_str(), sizeof(req.name_) - 1);
                                    g_shell_xbus->Call("com.freebsdx.BootEnv",
                                        static_cast<uint32_t>(BeOpcode::ActivateBE),
                                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                        [bf](const XBusMessage&) {
                                            if (auto rb = bf->rebuild_weak.lock())
                                                if (*rb) (*rb)();
                                        });
                                });
                            }

                            auto del_btn = std::make_shared<ButtonWidget>("Delete");
                            del_btn->setFixedSize(56, 22);
                            if (active) {
                                del_btn->setBackgroundColor({0.25f, 0.25f, 0.28f, 1.0f});
                                del_btn->setColor({0.45f, 0.45f, 0.50f, 1.0f});
                            } else {
                                del_btn->setBackgroundColor({0.55f, 0.18f, 0.18f, 1.0f});
                                // Destroying a BE is irreversible: the first click
                                // only arms the button ("Confirm?"), the second deletes.
                                auto armed = std::make_shared<bool>(false);
                                del_btn->onClick([be_name, bf, armed,
                                                  del_weak = std::weak_ptr<ButtonWidget>(del_btn)]() {
                                    if (!*armed) {
                                        *armed = true;
                                        if (auto b = del_weak.lock()) b->setText("Confirm?");
                                        return;
                                    }
                                    if (!g_shell_xbus) return;
                                    BeActionRequest req{};
                                    std::strncpy(req.name_, be_name.c_str(), sizeof(req.name_) - 1);
                                    g_shell_xbus->Call("com.freebsdx.BootEnv",
                                        static_cast<uint32_t>(BeOpcode::DeleteBE),
                                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                        [bf](const XBusMessage&) {
                                            if (auto rb = bf->rebuild_weak.lock())
                                                if (*rb) (*rb)();
                                        });
                                });
                            }

                            btn_row->addChild(act_btn);
                            btn_row->addChild(del_btn);

                            row->addChild(name_row);
                            row->addChild(date_lbl);
                            row->addChild(btn_row);
                            bf->col->addChild(row);
                        }

                        uint32_t n = bf->total_seen;
                        bf->count_lbl->setText(n == 0 ? "No BEs" : std::format("{} environment{}", n, n != 1 ? "s" : ""));
                        bf->count_lbl->setColor(n == 0
                            ? Color{0.55f, 0.55f, 0.60f, 1.0f}
                            : Color{0.40f, 0.75f, 0.55f, 1.0f});

                        if (bp.has_more_ && bf->fetch)
                            (*bf->fetch)(page + 1);
                        else if (n == 0)
                            bf->col->addChild(std::make_shared<TextWidget>("No boot environments found.", 11.0f));
                    });
            });
        (*bf->fetch)(0);
    };

    ctx.be_refresh_btn->onClick([rebuild_be = ctx.rebuild_be]() {
        if (rebuild_be && *rebuild_be) (*rebuild_be)();
    });
}

// Containers (paginated Fetcher list). Header + count label + result column;
// rebuild_containers uses a local ContainerFetcher struct (col + count_lbl +
// rebuild-weak + total + self-held recursive fetch) to page
// com.freebsdx.Containers ListContainers (per-container id/name/image + state
// label + Start/Stop button that re-triggers the rebuild). The ContainerFetcher
// struct stays inside the rebuild closure. Behaviour identical to the former
// inline block.
void buildContainersPanel(ShellContext& ctx) {
    ctx.ct_hdr_lbl      = std::make_shared<TextWidget>("\xf0\x9f\x90\xb3 Containers", 14.0f);
    ctx.ct_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.ct_count_lbl    = std::make_shared<TextWidget>("No containers", 11.0f);
    ctx.ct_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.ct_col          = std::make_shared<FlexColumn>();
    ctx.ct_col->setGap(6);

    ctx.rebuild_containers = std::make_shared<std::function<void()>>();
    *ctx.rebuild_containers = [ct_col = ctx.ct_col, ct_count_lbl = ctx.ct_count_lbl,
                            rct_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_containers)]() {
        if (!g_shell_xbus) return;
        ct_col->clearChildren();

        struct ContainerFetcher {
            std::shared_ptr<FlexColumn>          col;
            std::shared_ptr<TextWidget>          count_lbl;
            std::weak_ptr<std::function<void()>> rebuild_weak;
            uint32_t                             total{0};
            std::function<void(uint32_t)>        fetch;
        };
        auto cf = std::make_shared<ContainerFetcher>();
        cf->col          = ct_col;
        cf->count_lbl    = ct_count_lbl;
        cf->rebuild_weak = rct_weak;

        cf->fetch = [cf](uint32_t page) {
            g_shell_xbus->Call("com.freebsdx.Containers",
                static_cast<uint32_t>(ContainerOpcode::ListContainers),
                {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
                [cf](const XBusMessage& r) {
                    ContainerPage pg{};
                    std::memcpy(&pg, r.payload_, sizeof(pg));
                    if (pg.page_ == 0) cf->total = pg.total_;

                    static const char* kCtStateLabels[] = {
                        "Stopped", "Running", "Paused", "Error"
                    };
                    static const Color kCtStateColors[] = {
                        {0.45f, 0.45f, 0.50f, 1.0f},
                        {0.30f, 0.85f, 0.45f, 1.0f},
                        {0.90f, 0.72f, 0.20f, 1.0f},
                        {0.85f, 0.30f, 0.25f, 1.0f},
                    };

                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        std::string id(e.id_,
                            std::min(std::strlen(e.id_), sizeof(e.id_) - 1));
                        std::string name(e.name_,
                            std::min(std::strlen(e.name_), sizeof(e.name_) - 1));
                        std::string image(e.image_,
                            std::min(std::strlen(e.image_), sizeof(e.image_) - 1));
                        uint32_t state = std::min(e.state_, 3u);
                        bool running   = (state == 1);

                        auto id_lbl    = std::make_shared<TextWidget>(id, 10.0f);
                        id_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});
                        auto name_lbl  = std::make_shared<TextWidget>(name, 11.0f);
                        name_lbl->setColor({0.45f, 0.70f, 1.0f, 1.0f});
                        auto image_lbl = std::make_shared<TextWidget>(image, 10.0f);
                        image_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
                        auto state_lbl = std::make_shared<TextWidget>(
                            kCtStateLabels[state], 10.0f);
                        state_lbl->setColor(kCtStateColors[state]);

                        auto act_btn = std::make_shared<ButtonWidget>(
                            running ? "Stop" : "Start");
                        act_btn->setFixedSize(52, 22);
                        act_btn->setBackgroundColor(running
                            ? Color{0.50f, 0.12f, 0.12f, 1.0f}
                            : Color{0.12f, 0.36f, 0.16f, 1.0f});
                        act_btn->onClick([id, name, running,
                                          rebuild_weak = cf->rebuild_weak]() {
                            if (!g_shell_xbus) return;
                            ContainerActionRequest req{};
                            std::strncpy(req.id_,   id.c_str(),   sizeof(req.id_)   - 1);
                            std::strncpy(req.name_, name.c_str(), sizeof(req.name_) - 1);
                            auto op = running
                                ? ContainerOpcode::StopContainer
                                : ContainerOpcode::StartContainer;
                            g_shell_xbus->Call("com.freebsdx.Containers",
                                static_cast<uint32_t>(op),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                [rebuild_weak](const XBusMessage&) {
                                    if (auto rb = rebuild_weak.lock()) (*rb)();
                                });
                        });

                        cf->col->addChild(std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                id_lbl,
                                std::make_shared<Spacer>(6, 1),
                                name_lbl,
                                std::make_shared<Spacer>(1, 1, true),
                                state_lbl,
                                std::make_shared<Spacer>(8, 1),
                                act_btn,
                            }));
                        cf->col->addChild(image_lbl);
                    }

                    if (pg.has_more_) {
                        cf->fetch(pg.page_ + 1);
                    } else {
                        if (cf->total == 0) {
                            auto empty = std::make_shared<TextWidget>("No containers", 11.0f);
                            empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                            cf->col->addChild(empty);
                            cf->count_lbl->setText("No containers");
                        } else {
                            cf->count_lbl->setText(
                                std::format("{} container{}", cf->total,
                                    cf->total == 1 ? "" : "s"));
                        }
                    }
                });
        };
        cf->fetch(0);
    };
}

// Virtual Machines (paginated Fetcher list). Header + count label + result column;
// rebuild_vms uses a local VmFetcher struct (col + count_lbl + rebuild-weak +
// total + self-held recursive fetch) to page com.freebsdx.VirtualMachines ListVMs
// (per-VM name/type + vCPU/RAM spec + state label + Start/Stop button that
// re-triggers the rebuild). The VmFetcher struct stays inside the rebuild closure.
// Behaviour identical to the former inline block.
void buildVirtualMachinesPanel(ShellContext& ctx) {
    ctx.vm_hdr_lbl    = std::make_shared<TextWidget>("\xf0\x9f\x96\xa5 Virtual Machines", 14.0f);
    ctx.vm_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.vm_count_lbl  = std::make_shared<TextWidget>("No VMs", 11.0f);
    ctx.vm_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.vm_col        = std::make_shared<FlexColumn>();
    ctx.vm_col->setGap(6);

    ctx.rebuild_vms   = std::make_shared<std::function<void()>>();
    *ctx.rebuild_vms = [vm_col = ctx.vm_col, vm_count_lbl = ctx.vm_count_lbl,
                    rvm_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_vms)]() {
        if (!g_shell_xbus) return;
        vm_col->clearChildren();

        struct VmFetcher {
            std::shared_ptr<FlexColumn>          col;
            std::shared_ptr<TextWidget>          count_lbl;
            std::weak_ptr<std::function<void()>> rebuild_weak;
            uint32_t                             total{0};
            std::function<void(uint32_t)>        fetch;
        };
        auto vf = std::make_shared<VmFetcher>();
        vf->col          = vm_col;
        vf->count_lbl    = vm_count_lbl;
        vf->rebuild_weak = rvm_weak;

        vf->fetch = [vf](uint32_t page) {
            g_shell_xbus->Call("com.freebsdx.VirtualMachines",
                static_cast<uint32_t>(VmOpcode::ListVMs),
                {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
                [vf](const XBusMessage& r) {
                    VmPage pg{};
                    std::memcpy(&pg, r.payload_, sizeof(pg));
                    if (pg.page_ == 0) vf->total = pg.total_;

                    static const char* kVmStateLabels[] = {
                        "Stopped", "Running", "Paused", "Error"
                    };
                    static const Color kVmStateColors[] = {
                        {0.45f, 0.45f, 0.50f, 1.0f}, // grey — stopped
                        {0.30f, 0.85f, 0.45f, 1.0f}, // green — running
                        {0.90f, 0.72f, 0.20f, 1.0f}, // amber — paused
                        {0.85f, 0.30f, 0.25f, 1.0f}, // red — error
                    };

                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        std::string name(e.name_,
                            std::min(std::strlen(e.name_), sizeof(e.name_) - 1));
                        std::string type(e.type_,
                            std::min(std::strlen(e.type_), sizeof(e.type_) - 1));
                        uint32_t state = std::min(e.state_, 3u);
                        bool running   = (state == 1);

                        auto name_lbl  = std::make_shared<TextWidget>(name, 11.0f);
                        name_lbl->setColor({0.45f, 0.70f, 1.0f, 1.0f});
                        auto type_lbl  = std::make_shared<TextWidget>(type, 10.0f);
                        type_lbl->setColor({0.60f, 0.60f, 0.65f, 1.0f});
                        auto spec_lbl  = std::make_shared<TextWidget>(
                            std::format("{}vCPU  {}MB", e.cpus_, e.ram_mb_), 10.0f);
                        spec_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
                        auto state_lbl = std::make_shared<TextWidget>(
                            kVmStateLabels[state], 10.0f);
                        state_lbl->setColor(kVmStateColors[state]);

                        auto act_btn = std::make_shared<ButtonWidget>(
                            running ? "Stop" : "Start");
                        act_btn->setFixedSize(52, 22);
                        act_btn->setBackgroundColor(running
                            ? Color{0.50f, 0.12f, 0.12f, 1.0f}
                            : Color{0.12f, 0.36f, 0.16f, 1.0f});
                        act_btn->onClick([name, running,
                                          rebuild_weak = vf->rebuild_weak]() {
                            if (!g_shell_xbus) return;
                            VmActionRequest req{};
                            std::strncpy(req.name_, name.c_str(), sizeof(req.name_) - 1);
                            auto op = running ? VmOpcode::StopVM : VmOpcode::StartVM;
                            g_shell_xbus->Call("com.freebsdx.VirtualMachines",
                                static_cast<uint32_t>(op),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                [rebuild_weak](const XBusMessage&) {
                                    if (auto rb = rebuild_weak.lock()) (*rb)();
                                });
                        });

                        vf->col->addChild(std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl,
                                std::make_shared<Spacer>(8, 1),
                                type_lbl,
                                std::make_shared<Spacer>(1, 1, true),
                                state_lbl,
                                std::make_shared<Spacer>(8, 1),
                                act_btn,
                            }));
                        vf->col->addChild(spec_lbl);
                    }

                    if (pg.has_more_) {
                        vf->fetch(pg.page_ + 1);
                    } else {
                        if (vf->total == 0) {
                            auto empty = std::make_shared<TextWidget>("No VMs configured", 11.0f);
                            empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                            vf->col->addChild(empty);
                            vf->count_lbl->setText("No VMs");
                        } else {
                            vf->count_lbl->setText(
                                std::format("{} VM{}", vf->total,
                                    vf->total == 1 ? "" : "s"));
                        }
                    }
                });
        };
        vf->fetch(0);
    };
}

// Kernel Modules (paginated Fetcher list). Header + count label + result column +
// Refresh button; rebuild_kmod uses a local KmodFetcher struct (col + count_lbl +
// rebuild-weak + total_seen + self-held shared_ptr<function> fetch) to page
// com.freebsdx.KernelModules ListModules (per-module name + size/refs meta +
// Unload button, enabled only when refs==0, that re-triggers the rebuild). The
// Refresh button's onClick is wired after the rebuild body is assigned. The
// KmodFetcher struct stays inside the rebuild closure. Behaviour identical to the
// former inline block.
void buildKernelModulesPanel(ShellContext& ctx) {
    ctx.kmod_hdr_lbl   = std::make_shared<TextWidget>("\xe2\x9a\x99 Kernel Modules", 14.0f);
    ctx.kmod_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.kmod_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.kmod_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.kmod_col       = std::make_shared<FlexColumn>();
    ctx.kmod_col->setGap(4);

    ctx.kmod_refresh_btn = std::make_shared<ButtonWidget>("\xe2\x86\xbb Refresh");
    ctx.kmod_refresh_btn->setFixedSize(84, 26);
    ctx.kmod_refresh_btn->setBackgroundColor({0.20f, 0.22f, 0.28f, 1.0f});

    ctx.rebuild_kmod = std::make_shared<std::function<void()>>();
    *ctx.rebuild_kmod = [kmod_col = ctx.kmod_col, kmod_count_lbl = ctx.kmod_count_lbl,
                     rk_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_kmod)]() {
        if (!g_shell_xbus) return;
        kmod_col->clearChildren();

        struct KmodFetcher {
            std::shared_ptr<FlexColumn>                   col;
            std::shared_ptr<TextWidget>                   count_lbl;
            std::weak_ptr<std::function<void()>>          rebuild_weak;
            uint32_t                                      total_seen{0};
            std::shared_ptr<std::function<void(uint32_t)>> fetch;
        };
        auto kf = std::make_shared<KmodFetcher>();
        kf->col          = kmod_col;
        kf->count_lbl    = kmod_count_lbl;
        kf->rebuild_weak = rk_weak;

        kf->fetch = std::make_shared<std::function<void(uint32_t)>>(
            [kf](uint32_t page) {
                if (!g_shell_xbus) return;
                uint32_t pg = page;
                g_shell_xbus->Call("com.freebsdx.KernelModules",
                    static_cast<uint32_t>(KmodOpcode::ListModules),
                    {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
                    [kf, page](const XBusMessage& r) {
                        KmodPage kp{};
                        std::memcpy(&kp, r.payload_, sizeof(kp));
                        if (page == 0) {
                            kf->col->clearChildren();
                            kf->total_seen = 0;
                        }
                        kf->total_seen += kp.count_;

                        for (uint32_t i = 0; i < kp.count_; ++i) {
                            const KmodEntry& e = kp.entries_[i];
                            std::string mod_name(e.name_);
                            uint32_t refs   = e.refs_;
                            uint32_t size_k = e.size_kb_;

                            auto row = std::make_shared<FlexRow>();
                            row->setGap(6);

                            auto name_lbl = std::make_shared<TextWidget>(mod_name, 11.0f);
                            name_lbl->setColor({0.40f, 0.65f, 0.95f, 1.0f});

                            auto meta_lbl = std::make_shared<TextWidget>(
                                std::format("{}KB  refs:{}", size_k, refs), 10.0f);
                            meta_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                            auto spacer = std::make_shared<Spacer>(1, 1, true);

                            auto unload_btn = std::make_shared<ButtonWidget>("Unload");
                            unload_btn->setFixedSize(60, 22);
                            if (refs == 0) {
                                unload_btn->setBackgroundColor({0.55f, 0.18f, 0.18f, 1.0f});
                                unload_btn->onClick([mod_name, kf]() {
                                    if (!g_shell_xbus) return;
                                    KmodUnloadRequest req{};
                                    std::strncpy(req.name_, mod_name.c_str(), sizeof(req.name_) - 1);
                                    g_shell_xbus->Call("com.freebsdx.KernelModules",
                                        static_cast<uint32_t>(KmodOpcode::UnloadModule),
                                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                        [kf](const XBusMessage&) {
                                            if (auto rb = kf->rebuild_weak.lock())
                                                if (*rb) (*rb)();
                                        });
                                });
                            } else {
                                unload_btn->setBackgroundColor({0.25f, 0.25f, 0.28f, 1.0f});
                                unload_btn->setColor({0.45f, 0.45f, 0.50f, 1.0f});
                            }

                            row->addChild(name_lbl);
                            row->addChild(meta_lbl);
                            row->addChild(spacer);
                            row->addChild(unload_btn);
                            kf->col->addChild(row);
                        }

                        uint32_t n = kf->total_seen;
                        kf->count_lbl->setText(n == 0 ? "No modules" : std::format("{} loaded", n));
                        kf->count_lbl->setColor(n == 0
                            ? Color{0.55f, 0.55f, 0.60f, 1.0f}
                            : Color{0.40f, 0.75f, 0.55f, 1.0f});

                        if (kp.has_more_ && kf->fetch)
                            (*kf->fetch)(page + 1);
                        else if (n == 0)
                            kf->col->addChild(std::make_shared<TextWidget>("No kernel modules.", 11.0f));
                    });
            });
        (*kf->fetch)(0);
    };

    ctx.kmod_refresh_btn->onClick([rebuild_kmod = ctx.rebuild_kmod]() {
        if (rebuild_kmod && *rebuild_kmod) (*rebuild_kmod)();
    });
}

// Keychain (paginated Fetcher list). Header + count label + result column + a Lock
// button; rebuild_keychain uses a local KcFetcher struct (col + count + weak-self
// recursive fetch + total) to page com.freebsdx.Keychain List (page 0 with total==0
// shows "Locked"; otherwise per-entry service/account rows). The Lock button's
// onClick is wired inline. The KcFetcher struct stays inside the rebuild closure.
// Behaviour identical to the former inline block.
void buildKeychainPanel(ShellContext& ctx) {
    ctx.kc_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x94\x91 Keychain", 14.0f);
    ctx.kc_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.kc_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.kc_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.kc_col = std::make_shared<FlexColumn>();
    ctx.kc_col->setGap(4);

    ctx.kc_lock_btn = std::make_shared<ButtonWidget>("Lock", []() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.Keychain",
            static_cast<uint32_t>(KeychainOpcode::Lock), {}, nullptr);
    });
    ctx.kc_lock_btn->setFixedSize(50, 26);
    ctx.kc_lock_btn->setBackgroundColor({0.35f, 0.20f, 0.55f, 1.0f});

    ctx.rebuild_keychain = std::make_shared<std::function<void()>>();
    *ctx.rebuild_keychain = [kc_col = ctx.kc_col, kc_count_lbl = ctx.kc_count_lbl,
                         rkc_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_keychain)]() {
        if (!g_shell_xbus) return;
        kc_col->clearChildren();

        struct KcFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<KcFetcher>();
        pf->col   = kc_col;
        pf->count = kc_count_lbl;
        pf->fetch = [pf_weak = std::weak_ptr<KcFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            KeychainPageRequest req{};
            req.page_ = page;
            g_shell_xbus->Call("com.freebsdx.Keychain",
                static_cast<uint32_t>(KeychainOpcode::List),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    KeychainListPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0) pf->total = pg.total_;
                    if (page == 0 && pg.total_ == 0) {
                        pf->count->setText("Locked");
                        pf->count->setColor({0.85f, 0.35f, 0.35f, 1.0f});
                        auto lbl = std::make_shared<TextWidget>(
                            "Keychain is locked", 11.0f);
                        lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});
                        pf->col->addChild(lbl);
                        return;
                    }
                    if (page == 0) {
                        pf->count->setText(std::format("{} item(s)", pf->total));
                        pf->count->setColor({0.55f, 0.55f, 0.60f, 1.0f});
                    }
                    if (pg.valid_) {
                        auto svc_lbl = std::make_shared<TextWidget>(
                            std::string(pg.entry_.service_), 11.0f);
                        svc_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});
                        svc_lbl->setFlexGrow(1.0f);

                        auto acct_lbl = std::make_shared<TextWidget>(
                            std::string(pg.entry_.account_), 10.0f);
                        acct_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{svc_lbl, acct_lbl});
                        row->setAlignment(Alignment::Center);
                        pf->col->addChild(row);
                    }
                    if (pg.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };
}

// System Services (paginated Fetcher list). Header + result column + Refresh
// button; rebuild_svc uses a local SvcFetcher struct (col + weak-self recursive
// fetch) to page com.freebsdx.Services ListServices (per-service state-coloured
// badge + name + Start/Stop buttons whose visibility follows the running state).
// The Refresh button's onClick is wired after the rebuild body is assigned. The
// SvcFetcher struct stays inside the rebuild closure. Behaviour identical to the
// former inline block.
void buildSystemServicesPanel(ShellContext& ctx) {
    ctx.svc_hdr_lbl = std::make_shared<TextWidget>("⚙ Services", 14.0f);
    ctx.svc_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.svc_col = std::make_shared<FlexColumn>();
    ctx.svc_col->setGap(6);

    ctx.rebuild_svc = std::make_shared<std::function<void()>>();
    *ctx.rebuild_svc = [svc_col = ctx.svc_col,
                    rsvc_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_svc)]() {
        if (!g_shell_xbus) return;
        svc_col->clearChildren();

        struct SvcFetcher {
            std::shared_ptr<FlexColumn> col;
            std::function<void(uint32_t)> fetch;
        };
        auto sf = std::make_shared<SvcFetcher>();
        sf->col = svc_col;
        sf->fetch = [sf_weak = std::weak_ptr<SvcFetcher>(sf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.Services",
                static_cast<uint32_t>(ServiceOpcode::ListServices),
                {reinterpret_cast<const uint8_t*>(&pg_num),
                 reinterpret_cast<const uint8_t*>(&pg_num) + sizeof(pg_num)},
                [sf_weak, page](const XBusMessage& resp) {
                    auto sf = sf_weak.lock();
                    if (!sf) return;
                    ServicePage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No services", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        sf->col->addChild(empty);
                        return;
                    }
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        std::string state(e.state_);

                        // Coloured badge by state
                        std::string badge;
                        Color badge_col;
                        if (state == "running") {
                            badge = "●"; badge_col = {0.30f, 0.85f, 0.50f, 1.0f};
                        } else if (state == "stopped") {
                            badge = "●"; badge_col = {0.85f, 0.35f, 0.35f, 1.0f};
                        } else {
                            badge = "—"; badge_col = {0.45f, 0.45f, 0.50f, 1.0f};
                        }

                        auto badge_lbl = std::make_shared<TextWidget>(badge, 14.0f);
                        badge_lbl->setColor(badge_col);

                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(e.name_), 12.0f);
                        name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});
                        name_lbl->setFlexGrow(1.0f);

                        std::string svc_name(e.name_);
                        auto start_btn = std::make_shared<ButtonWidget>("▶ Start",
                            [svc_name]() {
                                if (!g_shell_xbus) return;
                                ServiceRequest req{};
                                std::strncpy(req.name_, svc_name.c_str(),
                                             sizeof(req.name_) - 1);
                                g_shell_xbus->Call("com.freebsdx.Services",
                                    static_cast<uint32_t>(ServiceOpcode::StartService),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    nullptr);
                            });
                        start_btn->setFixedSize(60, 22);
                        start_btn->setVisibility(state != "running");

                        auto stop_btn = std::make_shared<ButtonWidget>("■ Stop",
                            [svc_name]() {
                                if (!g_shell_xbus) return;
                                ServiceRequest req{};
                                std::strncpy(req.name_, svc_name.c_str(),
                                             sizeof(req.name_) - 1);
                                g_shell_xbus->Call("com.freebsdx.Services",
                                    static_cast<uint32_t>(ServiceOpcode::StopService),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    nullptr);
                            });
                        stop_btn->setFixedSize(56, 22);
                        stop_btn->setVisibility(state == "running");

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                badge_lbl, std::make_shared<Spacer>(6, 1),
                                name_lbl,
                                start_btn, std::make_shared<Spacer>(4, 1), stop_btn});
                        row->setAlignment(Alignment::Center);
                        sf->col->addChild(row);
                    }
                    if (pg.has_more_) {
                        if (auto p = sf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        sf->fetch(0);
    };

    ctx.svc_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh", [rebuild_svc = ctx.rebuild_svc]() {
        if (rebuild_svc && *rebuild_svc) (*rebuild_svc)();
    });
    ctx.svc_refresh_btn->setFixedSize(84, 26);
}



// Security Keys (outer-scope-fetcher list). Header + count label + result column
// + a status label + a "Register New Key" button; the SeckeyFetcher struct + skf
// shared_ptr live at builder scope and are captured by rebuild_seckey. fetch pages
// com.freebsdx.SecurityKey ListKeys (per-key label + added-date + Remove button;
// Remove re-triggers the rebuild). Register posts RegisterKey and refreshes on
// completion. Behaviour identical to the former inline block.
void buildSecurityKeysPanel(ShellContext& ctx) {
    ctx.seckey_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x94\x91 Security Keys", 14.0f);
    ctx.seckey_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.seckey_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.seckey_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.seckey_col       = std::make_shared<FlexColumn>();
    ctx.seckey_col->setGap(8);

    ctx.rebuild_seckey = std::make_shared<std::function<void()>>();
    struct SeckeyFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto skf = std::make_shared<SeckeyFetcher>();
    auto skf_weak = std::weak_ptr<SeckeyFetcher>(skf);

    skf->col       = ctx.seckey_col;
    skf->count_lbl = ctx.seckey_count_lbl;
    skf->rw        = ctx.rebuild_seckey;
    skf->fetch = [skf_weak](uint32_t page) {
        auto skf = skf_weak.lock();
        if (!skf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.SecurityKey",
            static_cast<uint32_t>(SecurityKeyOpcode::ListKeys),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [skf_weak, page](const XBusMessage& r) {
                auto skf = skf_weak.lock();
                if (!skf) return;
                SecurityKeyPage kp{};
                std::memcpy(&kp, r.payload_, sizeof(kp));
                if (!kp.valid_) return;
                if (page == 0) skf->col->clearChildren();

                auto label_lbl = std::make_shared<TextWidget>(
                    std::string("\xf0\x9f\x94\x91 ") + (kp.label_[0] ? kp.label_ : "Security Key"),
                    11.0f);
                label_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                char ts[32] = {};
                if (kp.added_ts_ > 0) {
                    auto t = static_cast<std::time_t>(kp.added_ts_);
                    std::strftime(ts, sizeof(ts), "Added %b %d, %Y", std::localtime(&t));
                }
                auto added_lbl = std::make_shared<TextWidget>(ts[0] ? ts : "", 10.0f);
                added_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                uint32_t id_val = kp.key_id_;
                auto remove_btn = std::make_shared<ButtonWidget>("Remove");
                remove_btn->setFixedSize(60, 22);
                remove_btn->setBackgroundColor({0.55f, 0.20f, 0.20f, 0.90f});
                remove_btn->onClick([id_val, skf_weak]() {
                    if (!g_shell_xbus) return;
                    SecurityKeyRequest req{};
                    req.key_id_ = id_val;
                    g_shell_xbus->Call("com.freebsdx.SecurityKey",
                        static_cast<uint32_t>(SecurityKeyOpcode::RemoveKey),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [skf_weak](const XBusMessage&) {
                            if (auto skf = skf_weak.lock(); skf && skf->rw.lock())
                                (*skf->rw.lock())();
                        });
                });

                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    label_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    added_lbl,
                    std::make_shared<Spacer>(8, 1),
                    remove_btn,
                });
                skf->col->addChild(row);

                if (skf->count_lbl)
                    skf->count_lbl->setText(std::format("{}", kp.total_));
                if (kp.has_more_) skf->fetch(page + 1);
            });
    };

    *ctx.rebuild_seckey = [seckey_col = ctx.seckey_col, skf]() {
        seckey_col->clearChildren();
        if (skf->fetch) skf->fetch(0);
    };

    ctx.seckey_status_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.seckey_status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});
    ctx.seckey_register_btn = std::make_shared<ButtonWidget>("+ Register New Key");
    ctx.seckey_register_btn->setFixedSize(150, 24);
    ctx.seckey_register_btn->onClick([seckey_status_lbl = ctx.seckey_status_lbl, rebuild_seckey = ctx.rebuild_seckey]() {
        if (!g_shell_xbus) return;
        seckey_status_lbl->setText("Touch your security key\xe2\x80\xa6");
        g_shell_xbus->Call("com.freebsdx.SecurityKey",
            static_cast<uint32_t>(SecurityKeyOpcode::RegisterKey), {},
            [seckey_status_lbl, rebuild_seckey](const XBusMessage&) {
                seckey_status_lbl->setText("Key registered \xe2\x9c\x93");
                if (rebuild_seckey && *rebuild_seckey) (*rebuild_seckey)();
            });
    });
}

// TOTP Authenticator (outer-scope-fetcher list). Header + count label + result
// column; the TotpFetcher struct + tpf shared_ptr live at builder scope and are
// captured by rebuild_totp. fetch pages com.freebsdx.Totp ListAccounts, and for
// each account issues a nested GetCode to fill the live 6-digit code + countdown
// (colour by seconds remaining); Copy puts the current code on the clipboard.
// Refreshed on loop_tick%10. Behaviour identical to the former inline block.
void buildTotpPanel(ShellContext& ctx) {
    ctx.totp_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x94\x90 TOTP Authenticator", 14.0f);
    ctx.totp_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.totp_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.totp_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.totp_col       = std::make_shared<FlexColumn>();
    ctx.totp_col->setGap(6);

    ctx.rebuild_totp = std::make_shared<std::function<void()>>();
    struct TotpFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto tpf = std::make_shared<TotpFetcher>();
    auto tpf_weak = std::weak_ptr<TotpFetcher>(tpf);

    tpf->col       = ctx.totp_col;
    tpf->count_lbl = ctx.totp_count_lbl;
    tpf->rw        = ctx.rebuild_totp;
    tpf->fetch = [tpf_weak](uint32_t page) {
        auto tpf = tpf_weak.lock();
        if (!tpf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.Totp",
            static_cast<uint32_t>(TotpOpcode::ListAccounts),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [tpf_weak, page](const XBusMessage& r) {
                auto tpf = tpf_weak.lock();
                if (!tpf) return;
                TotpAccountPage ap{};
                std::memcpy(&ap, r.payload_, sizeof(ap));
                if (!ap.valid_) return;
                if (page == 0) tpf->col->clearChildren();

                // For each account: fetch its current code then build the row
                auto id_str  = std::make_shared<std::string>(ap.id_);
                auto issuer  = std::string(ap.issuer_[0] ? ap.issuer_ : "Unknown");
                auto account = std::string(ap.account_[0] ? ap.account_ : "");
                auto label   = issuer + (account.empty() ? "" : " (" + account + ")");

                auto label_lbl    = std::make_shared<TextWidget>(label, 11.0f);
                label_lbl->setColor({0.80f, 0.80f, 0.88f, 1.0f});
                auto code_lbl     = std::make_shared<TextWidget>("------", 15.0f);
                code_lbl->setColor({0.35f, 0.85f, 0.55f, 1.0f});
                auto remain_lbl   = std::make_shared<TextWidget>("", 10.0f);
                remain_lbl->setColor({0.60f, 0.60f, 0.65f, 1.0f});
                auto copy_btn     = std::make_shared<ButtonWidget>("Copy");
                copy_btn->setFixedSize(48, 22);
                copy_btn->setBackgroundColor({0.20f, 0.45f, 0.75f, 0.90f});

                // Capture code_lbl to let copy_btn copy current code
                auto cur_code = std::make_shared<std::string>("------");
                copy_btn->onClick([cur_code]() {
                    if (!g_shell_xbus || cur_code->empty() || *cur_code == "------") return;
                    Clipboard::setString(*g_shell_xbus, *cur_code);
                });

                // Fetch the live code
                TotpCodeRequest req{};
                std::strncpy(req.id_, id_str->c_str(), sizeof(req.id_)-1);
                g_shell_xbus->Call("com.freebsdx.Totp",
                    static_cast<uint32_t>(TotpOpcode::GetCode),
                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                    [code_lbl, remain_lbl, cur_code](const XBusMessage& r) {
                        TotpCodeResponse resp{};
                        std::memcpy(&resp, r.payload_, sizeof(resp));
                        std::string code(resp.code_[0] ? resp.code_ : "------");
                        *cur_code = code;
                        // Format as "123 456" for readability
                        if (code.size() == 6)
                            code = code.substr(0,3) + " " + code.substr(3);
                        code_lbl->setText(code);
                        // Color countdown: ≤5 s → red, ≤10 s → yellow, else green
                        auto secs = resp.seconds_remaining_;
                        remain_lbl->setText(std::format("{}s", secs));
                        Color c = secs <= 5
                            ? Color{0.85f,0.35f,0.35f,1.0f}
                            : secs <= 10
                                ? Color{0.80f,0.70f,0.30f,1.0f}
                                : Color{0.35f,0.75f,0.45f,1.0f};
                        remain_lbl->setColor(c);
                        code_lbl->setColor(c);
                    });

                auto code_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    code_lbl,
                    std::make_shared<Spacer>(8,1),
                    remain_lbl,
                    std::make_shared<Spacer>(1,1,true),
                    copy_btn,
                });
                auto entry_col = std::make_shared<FlexColumn>();
                entry_col->addChild(label_lbl);
                entry_col->addChild(std::make_shared<Spacer>(1,2));
                entry_col->addChild(code_row);
                tpf->col->addChild(entry_col);
                tpf->col->addChild(std::make_shared<Spacer>(1,2));

                if (tpf->count_lbl)
                    tpf->count_lbl->setText(std::format("{}", ap.total_));
                if (ap.has_more_) tpf->fetch(page + 1);
            });
    };

    *ctx.rebuild_totp = [totp_col = ctx.totp_col, tpf]() {
        totp_col->clearChildren();
        if (tpf->fetch) tpf->fetch(0);
    };
}

// App Sandbox (outer-scope-fetcher list). Header + count label + result column;
// the SandboxFetcher struct + sbf shared_ptr live at builder scope and are
// captured by rebuild_sandbox. fetch pages com.freebsdx.AppSandbox ListApps
// (per-app name + a 5-permission row (Net/Cam/Mic/FS/GPU), each a ✓/✗ toggle that
// flips the bit via SetPermission and re-triggers the rebuild). Behaviour
// identical to the former inline block.
void buildAppSandboxPanel(ShellContext& ctx) {
    ctx.sandbox_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x93\xa6 App Sandbox", 14.0f);
    ctx.sandbox_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.sandbox_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.sandbox_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.sandbox_col       = std::make_shared<FlexColumn>();
    ctx.sandbox_col->setGap(6);

    ctx.rebuild_sandbox = std::make_shared<std::function<void()>>();
    struct SandboxFetcher {
        std::shared_ptr<FlexColumn>   col;
        std::shared_ptr<TextWidget>   count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)> fetch;
    };
    auto sbf      = std::make_shared<SandboxFetcher>();
    auto sbf_weak = std::weak_ptr<SandboxFetcher>(sbf);
    sbf->col       = ctx.sandbox_col;
    sbf->count_lbl = ctx.sandbox_count_lbl;
    sbf->rw        = ctx.rebuild_sandbox;
    sbf->fetch = [sbf_weak](uint32_t page) {
        if (!g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.AppSandbox",
            static_cast<uint32_t>(AppSandboxOpcode::ListApps),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [sbf_weak, page](const XBusMessage& r) {
                auto pf = sbf_weak.lock();
                if (!pf) return;
                AppSandboxPage sp{};
                std::memcpy(&sp, r.payload_, sizeof(sp));
                if (page == 0) {
                    if (sp.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No sandboxed apps", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                        pf->count_lbl->setText("");
                        return;
                    }
                    pf->count_lbl->setText(
                        std::format("{} app{}", sp.total_, sp.total_ == 1 ? "" : "s"));
                }
                if (!sp.valid_) {
                    if (sp.has_more_)
                        if (auto p = sbf_weak.lock()) p->fetch(page + 1);
                    return;
                }
                std::string id(sp.identifier_);
                std::string name(sp.display_name_[0] ? sp.display_name_ : sp.identifier_);
                auto app_name_lbl = std::make_shared<TextWidget>(name, 11.0f);
                app_name_lbl->setColor({0.88f, 0.88f, 0.92f, 1.0f});

                // Permission toggle helper
                struct Perm { const char* label; uint32_t bit; uint32_t allowed; };
                std::array<Perm, 6> perms{{
                    {"Net", 0, sp.net_allowed_},
                    {"Cam", 1, sp.cam_allowed_},
                    {"Mic", 2, sp.mic_allowed_},
                    {"FS",  3, sp.fs_allowed_},
                    {"GPU", 4, sp.gpu_allowed_},
                    {"Screen", 5, sp.screencap_allowed_},
                }};
                std::vector<std::shared_ptr<Widget>> perm_ws;
                for (const auto& p : perms) {
                    Color pc = p.allowed ? Color{0.30f, 0.80f, 0.45f, 1.0f}
                                         : Color{0.55f, 0.20f, 0.20f, 1.0f};
                    auto lbl = std::make_shared<TextWidget>(p.label, 10.0f);
                    lbl->setColor(pc);
                    auto btn = std::make_shared<ButtonWidget>(p.allowed ? "\xe2\x9c\x93" : "\xe2\x9c\x97",
                        [id, bit = p.bit, allowed = p.allowed, sbf_weak]() {
                            if (!g_shell_xbus) return;
                            AppSandboxPermRequest req{};
                            std::strncpy(req.identifier_, id.c_str(), sizeof(req.identifier_) - 1);
                            req.permission_ = bit;
                            req.allowed_    = allowed ? 0u : 1u;
                            g_shell_xbus->Call("com.freebsdx.AppSandbox",
                                static_cast<uint32_t>(AppSandboxOpcode::SetPermission),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                [sbf_weak](const XBusMessage&) {
                                    if (auto p = sbf_weak.lock())
                                        if (auto rw = p->rw.lock())
                                            if (*rw) (*rw)();
                                });
                        });
                    btn->setFixedSize(22, 20);
                    btn->setBackgroundColor(pc);
                    perm_ws.push_back(lbl);
                    perm_ws.push_back(std::make_shared<Spacer>(2, 1));
                    perm_ws.push_back(btn);
                    perm_ws.push_back(std::make_shared<Spacer>(6, 1));
                }
                auto perm_row = std::make_shared<FlexRow>(perm_ws);
                auto app_col  = std::make_shared<FlexColumn>();
                app_col->setGap(3);
                app_col->addChild(app_name_lbl);
                app_col->addChild(perm_row);
                pf->col->addChild(app_col);
                pf->col->addChild(std::make_shared<Spacer>(1, 4));
                if (sp.has_more_)
                    if (auto p = sbf_weak.lock()) p->fetch(page + 1);
            });
    };
    *ctx.rebuild_sandbox = [sandbox_col = ctx.sandbox_col, sbf]() {
        sandbox_col->clearChildren();
        if (sbf->fetch) sbf->fetch(0);
    };
}

// System Log (in-rebuild fetcher). Header + result column + a 4-way level filter
// button row (ERR/WARN/INFO/ALL, driven by a shared level_mask + a highlight
// closure); rebuild_slog builds a local SlogFetcher that pages com.freebsdx.SysLog
// GetEntries (per-entry severity badge + facility + message, filtered by mask).
// Only header/col/level-mask/filter-buttons + rebuild_slog escape (the filter
// button array is carded); the SlogFetcher and highlight closure stay
// builder-local. The kSlog* filter tables stay builder-local statics. Refreshed on
// loop_tick%30. Behaviour identical to the former inline block.
void buildSystemLogPanel(ShellContext& ctx) {
    ctx.slog_hdr_lbl    = std::make_shared<TextWidget>("\xf0\x9f\x93\x9c System Log", 14.0f);
    ctx.slog_col        = std::make_shared<FlexColumn>();
    ctx.slog_col->setGap(3);
    ctx.slog_level_mask = std::make_shared<uint32_t>(0u); // 0 = all levels

    // Level filter buttons: ERR / WARN / INFO / ALL
    static constexpr uint32_t kSlogFilterCount = 4u;
    static constexpr const char* kSlogFilterLabels[] = {"ERR", "WARN", "INFO", "ALL"};
    static constexpr uint32_t   kSlogFilterMasks[]   = {0x0Fu, 0x10u, 0x60u, 0x00u};
    ctx.slog_filter_btns = std::make_shared<
        std::array<std::shared_ptr<ButtonWidget>, kSlogFilterCount>>();
    for (uint32_t i = 0; i < kSlogFilterCount; ++i)
        (*ctx.slog_filter_btns)[i] = std::make_shared<ButtonWidget>(kSlogFilterLabels[i], [](){});

    ctx.rebuild_slog = std::make_shared<std::function<void()>>();

    auto slog_filter_hl = [slog_filter_btns = ctx.slog_filter_btns, slog_level_mask = ctx.slog_level_mask]() {
        for (uint32_t i = 0; i < kSlogFilterCount; ++i) {
            bool sel = (*slog_level_mask == kSlogFilterMasks[i]);
            (*slog_filter_btns)[i]->setBackgroundColor(
                sel ? Color{0.25f, 0.45f, 0.70f, 1.0f}
                    : Color{0.18f, 0.20f, 0.28f, 1.0f});
        }
    };
    slog_filter_hl();

    for (uint32_t i = 0; i < kSlogFilterCount; ++i) {
        (*ctx.slog_filter_btns)[i]->setFixedSize(48, 24);
        (*ctx.slog_filter_btns)[i]->onClick(
            [i, slog_level_mask = ctx.slog_level_mask, slog_filter_hl, rebuild_slog = ctx.rebuild_slog]() {
                *slog_level_mask = kSlogFilterMasks[i];
                slog_filter_hl();
                if (rebuild_slog && *rebuild_slog) (*rebuild_slog)();
            });
    }
    // Start with ALL selected (index 3)
    *ctx.slog_level_mask = kSlogFilterMasks[3];

    *ctx.rebuild_slog = [slog_col = ctx.slog_col, slog_level_mask = ctx.slog_level_mask,
                     rsl_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_slog)]() {
        if (!g_shell_xbus) return;
        slog_col->clearChildren();
        struct SlogFetcher {
            std::shared_ptr<FlexColumn>   col;
            uint32_t                      level_mask;
            std::function<void(uint32_t)> fetch;
        };
        auto pf = std::make_shared<SlogFetcher>();
        pf->col        = slog_col;
        pf->level_mask = *slog_level_mask;
        pf->fetch = [pf_weak = std::weak_ptr<SlogFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            SysLogRequest req{};
            req.page_       = page;
            req.level_mask_ = pf_weak.lock() ? pf_weak.lock()->level_mask : 0u;
            std::vector<uint8_t> buf(sizeof(req));
            std::memcpy(buf.data(), &req, sizeof(req));
            g_shell_xbus->Call("com.freebsdx.SysLog",
                static_cast<uint32_t>(SysLogOpcode::GetEntries), buf,
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    SysLogPageFlat ep{};
                    std::memcpy(&ep, resp.payload_, sizeof(ep));
                    if (ep.valid_) {
                        // Severity badge color + symbol
                        Color badge_col;
                        const char* badge_sym;
                        if (ep.level_ <= 3) {
                            badge_col = {0.80f, 0.25f, 0.25f, 1.0f};
                            badge_sym = "\xf0\x9f\x94\xb4"; // 🔴
                        } else if (ep.level_ == 4) {
                            badge_col = {0.85f, 0.55f, 0.15f, 1.0f};
                            badge_sym = "\xf0\x9f\x9f\xa0"; // 🟠
                        } else {
                            badge_col = {0.55f, 0.55f, 0.60f, 1.0f};
                            badge_sym = "\xe2\x97\x8f"; // ⚫ (info)
                        }
                        auto badge_lbl = std::make_shared<TextWidget>(badge_sym, 10.0f);
                        badge_lbl->setFixedSize(16, 16);

                        std::string fac(ep.facility_);
                        if (fac.size() > 10) fac = fac.substr(0, 9) + "\xe2\x80\xa6";
                        auto fac_lbl = std::make_shared<TextWidget>(fac, 10.0f);
                        fac_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});
                        fac_lbl->setFixedSize(76, 16);

                        std::string msg(ep.message_);
                        if (msg.size() > 60) msg = msg.substr(0, 59) + "\xe2\x80\xa6";
                        auto msg_lbl = std::make_shared<TextWidget>(msg, 10.0f);
                        msg_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});
                        msg_lbl->setFlexGrow(1.0f);
                        (void)badge_col;

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                badge_lbl, std::make_shared<Spacer>(4, 1),
                                fac_lbl,   std::make_shared<Spacer>(4, 1),
                                msg_lbl});
                        row->setAlignment(Alignment::Center);
                        pf->col->addChild(row);
                    }
                    if (!ep.valid_ && page == 0) {
                        auto empty = std::make_shared<TextWidget>("No log entries", 10.0f);
                        empty->setColor({0.40f, 0.40f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                    } else if (ep.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };
}

// Update Channel tile: Stable/Beta preset buttons + a "N pending" / "Up to
// date" badge, wrapped in a labelled row. Pure move out of main().
void buildUpdateChannelPanel(ShellContext& ctx) {
    constexpr int kUpdateChannelCount = 2;
    static const char* kUpdateChannelLabels[] = {"Stable", "Beta"};
    ctx.upd_ch_cur       = std::make_shared<uint32_t>(0u);
    ctx.upd_ch_btns      = std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kUpdateChannelCount>>();
    ctx.upd_ch_badge_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.upd_ch_badge_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

    auto upd_ch_hl = [upd_ch_btns = ctx.upd_ch_btns, upd_ch_cur = ctx.upd_ch_cur]() {
        for (int i = 0; i < kUpdateChannelCount; ++i) {
            bool active = (*upd_ch_cur == static_cast<uint32_t>(i));
            (*upd_ch_btns)[i]->setBackgroundColor(
                active ? Color{0.20f, 0.45f, 0.85f, 1.0f}
                       : Color{0.22f, 0.22f, 0.26f, 1.0f});
        }
    };

    for (int i = 0; i < kUpdateChannelCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kUpdateChannelLabels[i]);
        btn->setFixedSize(52, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});
        btn->onClick([i, upd_ch_cur = ctx.upd_ch_cur, upd_ch_hl]() {
            *upd_ch_cur = static_cast<uint32_t>(i);
            upd_ch_hl();
            if (!g_shell_xbus) return;
            UpdateChannelRequest req{};
            req.channel_ = static_cast<uint32_t>(i);
            g_shell_xbus->Call("com.freebsdx.UpdateChannel",
                static_cast<uint32_t>(UpdateChannelOpcode::SetChannel),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
        (*ctx.upd_ch_btns)[i] = btn;
    }
    upd_ch_hl();

    ctx.update_upd_ch_ui = [upd_ch_cur = ctx.upd_ch_cur, upd_ch_hl,
                            upd_ch_badge_lbl = ctx.upd_ch_badge_lbl](const UpdateChannelState& st) {
        *upd_ch_cur = std::min(st.channel_, 1u);
        upd_ch_hl();
        if (st.pending_count_ > 0) {
            upd_ch_badge_lbl->setText(std::format("{} pending", st.pending_count_));
            upd_ch_badge_lbl->setColor({0.95f, 0.72f, 0.20f, 1.0f});
        } else {
            upd_ch_badge_lbl->setText("Up to date");
            upd_ch_badge_lbl->setColor({0.40f, 0.75f, 0.55f, 1.0f});
        }
    };

    ctx.upd_ch_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("\xe2\xac\x86 Channel", 12.0f),
        std::make_shared<Spacer>(1, 1, true),
        (*ctx.upd_ch_btns)[0],
        std::make_shared<Spacer>(4, 1),
        (*ctx.upd_ch_btns)[1],
    });
}

// Package Signing tile: two switches (require-signed / allow-unknown) that post
// a combined PkgSigningPolicy. Pure move out of main().
void buildPackageSigningPanel(ShellContext& ctx) {
    ctx.pkgsign_req_switch = std::make_shared<SwitchWidget>(false);
    ctx.pkgsign_unk_switch = std::make_shared<SwitchWidget>(false);
    ctx.pkgsign_cur        = std::make_shared<PkgSigningPolicy>();

    auto send_pkgsign_policy = [pkgsign_req_switch = ctx.pkgsign_req_switch,
                                pkgsign_unk_switch = ctx.pkgsign_unk_switch,
                                pkgsign_cur = ctx.pkgsign_cur]() {
        if (!g_shell_xbus) return;
        PkgSigningPolicy pol{};
        pol.require_signed_ = pkgsign_req_switch->value() ? 1u : 0u;
        pol.allow_unknown_  = pkgsign_unk_switch->value() ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.PackageSigning",
            static_cast<uint32_t>(PackageSigningOpcode::SetPolicy),
            {reinterpret_cast<const uint8_t*>(&pol), sizeof(pol)},
            nullptr);
    };

    ctx.pkgsign_req_switch->setOnChanged([send_pkgsign_policy](bool) { send_pkgsign_policy(); });
    ctx.pkgsign_unk_switch->setOnChanged([send_pkgsign_policy](bool) { send_pkgsign_policy(); });

    ctx.update_pkgsign_ui = [pkgsign_req_switch = ctx.pkgsign_req_switch,
                             pkgsign_unk_switch = ctx.pkgsign_unk_switch,
                             pkgsign_cur = ctx.pkgsign_cur](const PkgSigningPolicy& pol) {
        *pkgsign_cur = pol;
        pkgsign_req_switch->setValue(pol.require_signed_ != 0);
        pkgsign_unk_switch->setValue(pol.allow_unknown_ != 0);
    };
}

// License & Support section (action center). Builds the label stack + Copy
// Support ID button and the update_lic_ui closure that populates them from a
// LicenseState (also re-binds the copy button's onClick to the current support
// ID). Behaviour is identical to the former inline block.
void buildLicensePanel(ShellContext& ctx) {
    ctx.lic_hdr_lbl      = std::make_shared<TextWidget>("\xf0\x9f\xaa\xaa License & Support", 14.0f);
    ctx.lic_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.lic_key_lbl      = std::make_shared<TextWidget>("••••-••••-••••-••••", 11.0f);
    ctx.lic_key_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.lic_tier_lbl     = std::make_shared<TextWidget>("", 12.0f);
    ctx.lic_tier_lbl->setColor({0.40f, 0.65f, 0.95f, 1.0f});
    ctx.lic_expiry_lbl   = std::make_shared<TextWidget>("", 11.0f);
    ctx.lic_expiry_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.lic_seats_lbl    = std::make_shared<TextWidget>("", 11.0f);
    ctx.lic_seats_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.lic_support_lbl  = std::make_shared<TextWidget>("", 11.0f);
    ctx.lic_support_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.lic_copy_btn     = std::make_shared<ButtonWidget>("Copy Support ID");
    ctx.lic_copy_btn->setFixedSize(130, 26);
    ctx.lic_copy_btn->setBackgroundColor({0.20f, 0.22f, 0.28f, 1.0f});

    ctx.update_lic_ui = [lic_key_lbl = ctx.lic_key_lbl, lic_tier_lbl = ctx.lic_tier_lbl,
                          lic_expiry_lbl = ctx.lic_expiry_lbl, lic_seats_lbl = ctx.lic_seats_lbl,
                          lic_support_lbl = ctx.lic_support_lbl, lic_copy_btn = ctx.lic_copy_btn]
                          (const LicenseState& st) {
        lic_key_lbl->setText(st.license_key_[0] ? std::string(st.license_key_) : "No license");
        lic_tier_lbl->setText(st.tier_[0] ? std::string(st.tier_) : "");

        // Expiry: red if within 30 days, amber if within 90, green/dim otherwise
        if (st.expiry_[0]) {
            std::string exp(st.expiry_);
            lic_expiry_lbl->setText("Expires: " + exp);
            // Simple check: "Never" keeps dim, otherwise amber/red logic left to compositor
            lic_expiry_lbl->setColor(exp == "Never"
                ? Color{0.55f, 0.55f, 0.60f, 1.0f}
                : Color{0.95f, 0.72f, 0.20f, 1.0f});
        }

        if (st.seats_total_ > 0) {
            bool near_full = st.seats_used_ >= static_cast<uint32_t>(st.seats_total_ * 0.9f);
            lic_seats_lbl->setText(std::format("Seats: {} / {}", st.seats_used_, st.seats_total_));
            lic_seats_lbl->setColor(near_full
                ? Color{0.95f, 0.72f, 0.20f, 1.0f}
                : Color{0.55f, 0.55f, 0.60f, 1.0f});
        } else {
            lic_seats_lbl->setText("Seats: Unlimited");
        }

        std::string sid(st.support_id_[0] ? st.support_id_ : "");
        lic_support_lbl->setText(sid.empty() ? "" : "Support ID: " + sid);

        lic_copy_btn->onClick([sid]() {
            if (!sid.empty() && g_shell_xbus)
                Clipboard::setString(*g_shell_xbus, sid);
        });
    };
}

// Memory Pressure monitor (action center). Builds the label stack + a 10-block
// RAM fill bar and the update_mem_ui closure that recolours them from a
// MemPressureState. Behaviour is identical to the former inline block.
void buildMemPressurePanel(ShellContext& ctx) {
    ctx.mem_hdr_lbl      = std::make_shared<TextWidget>("\xf0\x9f\xa7\xa0 Memory Pressure", 14.0f);
    ctx.mem_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.mem_level_lbl    = std::make_shared<TextWidget>("Normal", 11.0f);
    ctx.mem_level_lbl->setColor({0.35f, 0.80f, 0.45f, 1.0f});
    ctx.mem_ram_lbl      = std::make_shared<TextWidget>("RAM: — / —", 11.0f);
    ctx.mem_ram_lbl->setColor({0.70f, 0.70f, 0.75f, 1.0f});
    ctx.mem_swap_lbl     = std::make_shared<TextWidget>("Swap: — / —", 11.0f);
    ctx.mem_swap_lbl->setColor({0.60f, 0.60f, 0.65f, 1.0f});
    ctx.mem_faults_lbl   = std::make_shared<TextWidget>("Faults: —/s", 10.0f);
    ctx.mem_faults_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

    // 10-block fill bar for RAM usage
    ctx.mem_bar_row = std::make_shared<FlexRow>();
    ctx.mem_bar_row->setGap(2);
    constexpr int kMemBarBlocks = 10;
    ctx.mem_bar_blocks = std::make_shared<std::array<std::shared_ptr<TextWidget>, kMemBarBlocks>>();
    for (int i = 0; i < kMemBarBlocks; ++i) {
        auto blk = std::make_shared<TextWidget>("\xe2\x96\x88", 10.0f); // █
        blk->setColor({0.20f, 0.20f, 0.22f, 1.0f});
        (*ctx.mem_bar_blocks)[i] = blk;
        ctx.mem_bar_row->addChild(blk);
    }

    ctx.update_mem_ui = [mem_level_lbl = ctx.mem_level_lbl, mem_ram_lbl = ctx.mem_ram_lbl,
                          mem_swap_lbl = ctx.mem_swap_lbl, mem_faults_lbl = ctx.mem_faults_lbl,
                          mem_bar_blocks = ctx.mem_bar_blocks](const MemPressureState& st) {
        // pressure badge colour
        static const char* kLevelLabels[] = {"Normal", "Moderate", "Critical"};
        static const Color kLevelColors[] = {
            {0.35f, 0.80f, 0.45f, 1.0f},  // green
            {0.95f, 0.72f, 0.20f, 1.0f},  // amber
            {0.90f, 0.30f, 0.30f, 1.0f},  // red
        };
        uint32_t lvl = std::min(st.pressure_level_, 2u);
        mem_level_lbl->setText(kLevelLabels[lvl]);
        mem_level_lbl->setColor(kLevelColors[lvl]);

        // RAM
        if (st.total_kb_ > 0) {
            uint64_t used_kb = st.total_kb_ - st.free_kb_;
            mem_ram_lbl->setText(std::format("RAM: {:.1f} / {:.1f} GB",
                used_kb / 1048576.0, st.total_kb_ / 1048576.0));
            // fill bar
            constexpr int kMemBarBlocks = 10;
            int filled = static_cast<int>((used_kb * kMemBarBlocks) / st.total_kb_);
            filled = std::clamp(filled, 0, +kMemBarBlocks);
            for (int i = 0; i < kMemBarBlocks; ++i) {
                (*mem_bar_blocks)[i]->setColor(i < filled
                    ? kLevelColors[lvl]
                    : Color{0.20f, 0.20f, 0.22f, 1.0f});
            }
        }

        // swap
        if (st.swap_total_kb_ > 0) {
            mem_swap_lbl->setText(std::format("Swap: {:.1f} / {:.1f} GB",
                st.swap_used_kb_ / 1048576.0, st.swap_total_kb_ / 1048576.0));
        } else {
            mem_swap_lbl->setText("Swap: none");
        }

        mem_faults_lbl->setText(std::format("Faults: {}/s", st.page_faults_sec_));
    };
}

// System Information monitor (action center). Builds an 8-row key:value table
// (make_si_row stays a builder-local helper) and the update_sysinfo_ui closure
// that populates the value labels from a SysInfoState. Behaviour is identical
// to the former inline block.
void buildSysInfoPanel(ShellContext& ctx) {
    ctx.si_hdr_lbl      = std::make_shared<TextWidget>("\xe2\x84\xb9 System Info", 14.0f);
    ctx.si_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.si_col          = std::make_shared<FlexColumn>();
    ctx.si_col->setGap(5);

    // Pre-build key:value row helper
    auto make_si_row = [](const std::string& key,
                          std::shared_ptr<TextWidget> val_lbl) {
        auto key_lbl = std::make_shared<TextWidget>(key, 10.0f);
        key_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
        val_lbl->setColor({0.88f, 0.88f, 0.92f, 1.0f});
        return std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            key_lbl, std::make_shared<Spacer>(1, 1, true), val_lbl,
        });
    };

    ctx.si_hostname_lbl = std::make_shared<TextWidget>("\xe2\x80\x94", 10.0f);
    ctx.si_os_lbl       = std::make_shared<TextWidget>("\xe2\x80\x94", 10.0f);
    ctx.si_cpu_lbl      = std::make_shared<TextWidget>("\xe2\x80\x94", 10.0f);
    ctx.si_cores_lbl    = std::make_shared<TextWidget>("\xe2\x80\x94", 10.0f);
    ctx.si_ram_lbl      = std::make_shared<TextWidget>("\xe2\x80\x94", 10.0f);
    ctx.si_gpu_lbl      = std::make_shared<TextWidget>("\xe2\x80\x94", 10.0f);
    ctx.si_uptime_lbl   = std::make_shared<TextWidget>("\xe2\x80\x94", 10.0f);
    ctx.si_serial_lbl   = std::make_shared<TextWidget>("\xe2\x80\x94", 10.0f);

    ctx.si_col->addChild(make_si_row("Hostname", ctx.si_hostname_lbl));
    ctx.si_col->addChild(make_si_row("OS",       ctx.si_os_lbl));
    ctx.si_col->addChild(make_si_row("CPU",      ctx.si_cpu_lbl));
    ctx.si_col->addChild(make_si_row("Cores",    ctx.si_cores_lbl));
    ctx.si_col->addChild(make_si_row("RAM",      ctx.si_ram_lbl));
    ctx.si_col->addChild(make_si_row("GPU",      ctx.si_gpu_lbl));
    ctx.si_col->addChild(make_si_row("Uptime",   ctx.si_uptime_lbl));
    ctx.si_col->addChild(make_si_row("Serial",   ctx.si_serial_lbl));

    ctx.update_sysinfo_ui = [si_hostname_lbl = ctx.si_hostname_lbl, si_os_lbl = ctx.si_os_lbl,
                              si_cpu_lbl = ctx.si_cpu_lbl, si_cores_lbl = ctx.si_cores_lbl,
                              si_ram_lbl = ctx.si_ram_lbl, si_gpu_lbl = ctx.si_gpu_lbl,
                              si_uptime_lbl = ctx.si_uptime_lbl, si_serial_lbl = ctx.si_serial_lbl]
                              (const SysInfoState& st) {
        si_hostname_lbl->setText(st.hostname_[0] ? std::string(st.hostname_) : "—");
        si_os_lbl->setText(st.os_version_[0] ? std::string(st.os_version_) : "—");
        si_cpu_lbl->setText(st.cpu_model_[0] ? std::string(st.cpu_model_) : "—");
        si_cores_lbl->setText(st.cpu_cores_ ? std::format("{}", st.cpu_cores_) : "—");
        // RAM: show in GB if ≥ 1024 MB, else MB
        if (st.ram_total_mb_ >= 1024) {
            uint32_t gb = st.ram_total_mb_ / 1024;
            si_ram_lbl->setText(std::format("{} GB", gb));
        } else {
            si_ram_lbl->setText(std::format("{} MB", st.ram_total_mb_));
        }
        si_gpu_lbl->setText(st.gpu_model_[0] ? std::string(st.gpu_model_) : "—");
        // Uptime: d h m
        uint32_t u = st.uptime_secs_;
        uint32_t d = u / 86400; u %= 86400;
        uint32_t h = u / 3600;  u %= 3600;
        uint32_t m = u / 60;
        si_uptime_lbl->setText(d > 0
            ? std::format("{}d {}h {}m", d, h, m)
            : std::format("{}h {}m", h, m));
        si_serial_lbl->setText(st.serial_[0] ? std::string(st.serial_) : "N/A");
    };
}

// Admin Unlock (shown only when the screen is locked). Username/password inputs
// + Unlock button that authenticates over com.freebsdx.Lock. Only unlock_locked
// + unlock_inputs_row + unlock_status_lbl escape (their visibility is toggled by
// the lock-state flow); the inputs/button/row-holder stay builder-local (kept
// alive by unlock_inputs_row). Behaviour is identical to the former inline
// block.
void buildAdminUnlockPanel(ShellContext& ctx) {
    ctx.unlock_locked       = std::make_shared<bool>(false);
    auto unlock_row_widget  = std::make_shared<std::shared_ptr<FlexRow>>();
    auto unlock_user_input  = std::make_shared<TextInputWidget>();
    auto unlock_pass_input  = std::make_shared<TextInputWidget>();
    ctx.unlock_status_lbl   = std::make_shared<TextWidget>("", 10.0f);
    unlock_user_input->setPlaceholder("Username\xe2\x80\xa6");
    unlock_pass_input->setPlaceholder("Password\xe2\x80\xa6");
    /* password mode unsupported */
    unlock_user_input->setFlexGrow(1.0f);
    unlock_pass_input->setFlexGrow(1.0f);
    ctx.unlock_status_lbl->setColor({0.80f, 0.30f, 0.30f, 1.0f});

    (void)unlock_row_widget; // holder retained for parity with the former inline block

    auto unlock_btn = std::make_shared<ButtonWidget>("Unlock",
        [unlock_user_input, unlock_pass_input, unlock_status_lbl = ctx.unlock_status_lbl]() {
            if (!g_shell_xbus) return;
            LockAuthRequest req{};
            std::strncpy(req.username_, unlock_user_input->getText().c_str(),
                         sizeof(req.username_) - 1);
            std::strncpy(req.password_, unlock_pass_input->getText().c_str(),
                         sizeof(req.password_) - 1);
            std::vector<uint8_t> buf(sizeof(req));
            std::memcpy(buf.data(), &req, sizeof(req));
            g_shell_xbus->Call("com.freebsdx.Lock",
                static_cast<uint32_t>(LockOpcode::Unlock), buf,
                [unlock_status_lbl, unlock_pass_input](const XBusMessage& r) {
                    LockState ls{};
                    std::memcpy(&ls, r.payload_, sizeof(ls));
                    if (ls.locked_ == 0) {
                        unlock_status_lbl->setText("");
                        unlock_pass_input->setText("");
                    } else {
                        unlock_status_lbl->setText("Authentication failed");
                        unlock_status_lbl->setColor({0.80f, 0.30f, 0.30f, 1.0f});
                    }
                });
        });
    unlock_btn->setFixedSize(64, 28);
    unlock_btn->setBackgroundColor({0.40f, 0.25f, 0.15f, 1.0f});

    ctx.unlock_inputs_row = std::make_shared<FlexRow>(
        std::vector<std::shared_ptr<Widget>>{
            unlock_user_input, std::make_shared<Spacer>(6, 1),
            unlock_pass_input, std::make_shared<Spacer>(6, 1),
            unlock_btn});
    ctx.unlock_inputs_row->setAlignment(Alignment::Center);
    // Shown only when screen is locked
    ctx.unlock_inputs_row->setVisibility(false);
    ctx.unlock_status_lbl->setVisibility(false);
}

// Thermal Zones (paginated Fetcher list). Count/status labels + result column +
// rebuild_tz, which spins a builder-local TzFetcher that pages com.freebsdx.
// Thermal GetZones, rendering per-zone temp rows (colour-graded) + optional fan
// switch. Behaviour is identical to the former inline block.
void buildThermalZonesPanel(ShellContext& ctx) {
    ctx.tz_count_lbl  = std::make_shared<TextWidget>("", 11.0f);
    ctx.tz_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.tz_col        = std::make_shared<FlexColumn>();
    ctx.tz_col->setGap(4);
    ctx.tz_status_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.tz_status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});

    ctx.rebuild_tz = std::make_shared<std::function<void()>>();
    struct TzFetcher {
        std::shared_ptr<FlexColumn> col;
        std::shared_ptr<TextWidget> count_lbl;
        std::function<void(uint32_t)> fetch;
    };
    auto tzpf      = std::make_shared<TzFetcher>();
    auto tzpf_weak = std::weak_ptr<TzFetcher>(tzpf);
    tzpf->col       = ctx.tz_col;
    tzpf->count_lbl = ctx.tz_count_lbl;
    tzpf->fetch = [tzpf_weak](uint32_t page) {
        if (!g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.Thermal",
            static_cast<uint32_t>(ThermalOpcode::GetZones),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [tzpf_weak, page](const XBusMessage& r) {
                auto pf = tzpf_weak.lock();
                if (!pf) return;
                ThermalZonePage zp{};
                std::memcpy(&zp, r.payload_, sizeof(zp));
                if (page == 0) {
                    if (zp.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No thermal zones", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                        pf->count_lbl->setText("");
                        return;
                    }
                    pf->count_lbl->setText(
                        std::format("{} zone{}", zp.total_, zp.total_ == 1 ? "" : "s"));
                }
                if (!zp.valid_) {
                    if (zp.has_more_)
                        if (auto p = tzpf_weak.lock()) p->fetch(page + 1);
                    return;
                }
                int t = static_cast<int>(zp.temp_c_);
                Color tc =
                    t >= 90 ? Color{0.98f, 0.25f, 0.20f, 1.0f} :
                    t >= 75 ? Color{0.98f, 0.65f, 0.20f, 1.0f} :
                    t >= 60 ? Color{0.95f, 0.88f, 0.30f, 1.0f} :
                              Color{0.35f, 0.85f, 0.45f, 1.0f};
                std::string zone_label(zp.label_[0] ? zp.label_ : "Zone");
                auto tz_name_lbl = std::make_shared<TextWidget>(zone_label, 11.0f);
                tz_name_lbl->setColor({0.75f, 0.75f, 0.80f, 1.0f});
                auto tz_temp_lbl = std::make_shared<TextWidget>(
                    std::format("{} \xc2\xb0""C", t), 12.0f);
                tz_temp_lbl->setColor(tc);
                pf->col->addChild(std::make_shared<FlexRow>(
                    std::vector<std::shared_ptr<Widget>>{
                        tz_name_lbl,
                        std::make_shared<Spacer>(1, 1, true),
                        tz_temp_lbl,
                    }));
                if (zp.fan_rpm_ > 0) {
                    uint32_t zone_id = zp.zone_id_;
                    bool fan_manual  = zp.fan_mode_ == 1;
                    auto fan_sw = std::make_shared<SwitchWidget>(fan_manual);
                    fan_sw->setOnChanged([zone_id](bool m) {
                        if (!g_shell_xbus) return;
                        ThermalFanRequest req{};
                        req.zone_id_  = zone_id;
                        req.fan_mode_ = m ? 1u : 0u;
                        req.fan_pct_  = 50u;
                        g_shell_xbus->Call("com.freebsdx.Thermal",
                            static_cast<uint32_t>(ThermalOpcode::SetFanMode),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                            nullptr);
                    });
                    auto fan_rpm_lbl = std::make_shared<TextWidget>(
                        std::format("Fan {} RPM", zp.fan_rpm_), 10.0f);
                    fan_rpm_lbl->setColor({0.55f, 0.65f, 0.80f, 1.0f});
                    auto fan_mode_lbl = std::make_shared<TextWidget>(
                        fan_manual ? "Manual" : "Auto", 10.0f);
                    fan_mode_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
                    pf->col->addChild(std::make_shared<FlexRow>(
                        std::vector<std::shared_ptr<Widget>>{
                            fan_rpm_lbl,
                            std::make_shared<Spacer>(1, 1, true),
                            fan_mode_lbl,
                            std::make_shared<Spacer>(6, 1),
                            fan_sw,
                        }));
                }
                pf->col->addChild(std::make_shared<Spacer>(1, 4));
                if (zp.has_more_)
                    if (auto p = tzpf_weak.lock()) p->fetch(page + 1);
            });
    };
    *ctx.rebuild_tz = [tz_col = ctx.tz_col, tzpf]() {
        tz_col->clearChildren();
        if (tzpf->fetch) tzpf->fetch(0);
    };
}

// Multi-Seat (paginated Fetcher list). Count label + result column +
// rebuild_seats, which spins a builder-local SeatFetcher paging com.freebsdx.
// Seat ListSeats, rendering per-seat name/user rows + input-device badges.
// Behaviour is identical to the former inline block.
void buildMultiSeatPanel(ShellContext& ctx) {
    ctx.seat_count_lbl = std::make_shared<TextWidget>("Single seat", 10.0f);
    ctx.seat_count_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
    ctx.seat_col = std::make_shared<FlexColumn>();
    ctx.seat_col->setGap(4);

    ctx.rebuild_seats = std::make_shared<std::function<void()>>();
    *ctx.rebuild_seats = [seat_col = ctx.seat_col, seat_count_lbl = ctx.seat_count_lbl,
                      rse_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_seats)]() {
        if (!g_shell_xbus) return;
        seat_col->clearChildren();

        struct SeatFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<SeatFetcher>();
        pf->col   = seat_col;
        pf->count = seat_count_lbl;
        pf->fetch = [pf_weak = std::weak_ptr<SeatFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.Seat",
                static_cast<uint32_t>(SeatOpcode::ListSeats),
                {reinterpret_cast<const uint8_t*>(&pg_num),
                 reinterpret_cast<const uint8_t*>(&pg_num) + sizeof(pg_num)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    SeatPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0) pf->total = pg.total_;
                    if (page == 0) {
                        pf->count->setText(pf->total <= 1
                            ? "Single seat"
                            : std::format("{} seats", pf->total));
                        pf->count->setColor({0.50f, 0.50f, 0.55f, 1.0f});
                    }
                    if (pg.valid_) {
                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(pg.name_), 11.0f);
                        name_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});

                        std::string user_str = std::strlen(pg.user_) > 0
                            ? std::string(pg.user_)
                            : "\xe2\x80\x94";  // em-dash
                        auto user_lbl = std::make_shared<TextWidget>(user_str, 10.0f);
                        user_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
                        user_lbl->setFlexGrow(1.0f);

                        // Device badges: ⌨ 🖱 👆
                        std::string badges;
                        if (pg.has_keyboard_) badges += "\xe2\x8c\xa8";
                        if (pg.has_pointer_)  badges += " \xf0\x9f\x96\xb1";
                        if (pg.has_touch_)    badges += " \xf0\x9f\x91\x86";
                        auto badge_lbl = std::make_shared<TextWidget>(badges, 10.0f);
                        badge_lbl->setColor({0.45f, 0.65f, 0.45f, 1.0f});

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl, std::make_shared<Spacer>(6, 1),
                                user_lbl, badge_lbl});
                        row->setAlignment(Alignment::Center);
                        pf->col->addChild(row);
                    }
                    if (pg.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };
}

// Crash Reporter (paginated Fetcher list). Header + result column + Clear-All
// button; rebuild_crash spins a builder-local CrashFetcher paging com.freebsdx.
// CrashReporter ListReports (app/signal + timestamp rows with per-report Delete).
// Behaviour is identical to the former inline block.
void buildCrashReporterPanel(ShellContext& ctx) {
    ctx.crash_hdr_lbl = std::make_shared<TextWidget>("💥 Crash Reports", 14.0f);
    ctx.crash_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.crash_col = std::make_shared<FlexColumn>();
    ctx.crash_col->setGap(6);

    ctx.rebuild_crash = std::make_shared<std::function<void()>>();
    *ctx.rebuild_crash = [crash_col = ctx.crash_col,
                      rcr_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_crash)]() {
        if (!g_shell_xbus) return;
        crash_col->clearChildren();

        struct CrashFetcher {
            std::shared_ptr<FlexColumn> col;
            std::function<void(uint32_t)> fetch;
        };
        auto cf = std::make_shared<CrashFetcher>();
        cf->col = crash_col;
        cf->fetch = [cf_weak = std::weak_ptr<CrashFetcher>(cf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.CrashReporter",
                static_cast<uint32_t>(CrashOpcode::ListReports),
                {reinterpret_cast<const uint8_t*>(&pg_num),
                 reinterpret_cast<const uint8_t*>(&pg_num) + sizeof(pg_num)},
                [cf_weak, page](const XBusMessage& resp) {
                    auto cf = cf_weak.lock();
                    if (!cf) return;
                    CrashReportPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No crash reports", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        cf->col->addChild(empty);
                        return;
                    }
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        // Format timestamp as local date/time
                        std::time_t ts = static_cast<std::time_t>(e.timestamp_);
                        char tsbuf[24] = {};
                        std::strftime(tsbuf, sizeof(tsbuf), "%b %d  %H:%M",
                                      std::localtime(&ts));

                        auto app_lbl = std::make_shared<TextWidget>(
                            std::string(e.app_) + "  " + std::string(e.signal_), 12.0f);
                        app_lbl->setColor({0.95f, 0.55f, 0.40f, 1.0f});

                        auto ts_lbl = std::make_shared<TextWidget>(
                            std::string(tsbuf), 10.0f);
                        ts_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        uint32_t rid = e.report_id_;
                        auto del_btn = std::make_shared<ButtonWidget>("✕ Delete",
                            [rid, cf_weak]() {
                                if (!g_shell_xbus) return;
                                CrashDeleteRequest req{};
                                req.report_id_ = rid;
                                g_shell_xbus->Call("com.freebsdx.CrashReporter",
                                    static_cast<uint32_t>(CrashOpcode::DeleteReport),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    nullptr);
                                // Refresh list after delete
                                if (auto p = cf_weak.lock()) p->fetch(0);
                            });
                        del_btn->setFixedSize(72, 22);

                        auto info_col = std::make_shared<FlexColumn>(
                            std::vector<std::shared_ptr<Widget>>{app_lbl, ts_lbl});
                        info_col->setGap(2);
                        info_col->setFlexGrow(1.0f);

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{info_col, del_btn});
                        row->setAlignment(Alignment::Center);
                        cf->col->addChild(row);
                    }
                    if (pg.has_more_) {
                        if (auto p = cf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        cf->fetch(0);
    };

    ctx.crash_clear_btn = std::make_shared<ButtonWidget>("🗑 Clear All",
        [rebuild_crash = ctx.rebuild_crash]() {
            if (!g_shell_xbus) return;
            g_shell_xbus->Call("com.freebsdx.CrashReporter",
                static_cast<uint32_t>(CrashOpcode::ClearAll), {}, nullptr);
            if (rebuild_crash && *rebuild_crash) (*rebuild_crash)();
        });
    ctx.crash_clear_btn->setFixedSize(96, 26);
}

// Security Events (paginated Fetcher list). Header + result column + a
// Refresh/Clear button row; rebuild_sec spins a builder-local SecFetcher paging
// com.freebsdx.Security ListEvents (badge/type/user + source·timestamp rows,
// colour-coded by success). The two buttons stay builder-local (kept alive by
// sec_btn_row). Behaviour is identical to the former inline block.
void buildSecurityEventsPanel(ShellContext& ctx) {
    ctx.sec_hdr_lbl = std::make_shared<TextWidget>("🔒 Security Events", 14.0f);
    ctx.sec_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.sec_col = std::make_shared<FlexColumn>();
    ctx.sec_col->setGap(6);

    ctx.rebuild_sec = std::make_shared<std::function<void()>>();
    *ctx.rebuild_sec = [sec_col = ctx.sec_col,
                    rsec_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_sec)]() {
        if (!g_shell_xbus) return;
        sec_col->clearChildren();

        struct SecFetcher {
            std::shared_ptr<FlexColumn> col;
            std::function<void(uint32_t)> fetch;
        };
        auto sf = std::make_shared<SecFetcher>();
        sf->col = sec_col;
        sf->fetch = [sf_weak = std::weak_ptr<SecFetcher>(sf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.Security",
                static_cast<uint32_t>(SecurityOpcode::ListEvents),
                {reinterpret_cast<const uint8_t*>(&pg_num),
                 reinterpret_cast<const uint8_t*>(&pg_num) + sizeof(pg_num)},
                [sf_weak, page](const XBusMessage& resp) {
                    auto sf = sf_weak.lock();
                    if (!sf) return;
                    SecurityEventPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No events recorded", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        sf->col->addChild(empty);
                        return;
                    }
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        // Badge emoji by type
                        std::string badge;
                        std::string type(e.type_);
                        if      (type == "SSH")   badge = "🔐";
                        else if (type == "sudo")  badge = "🔑";
                        else if (type == "login") badge = "👤";
                        else                      badge = "⚠";

                        std::time_t ts = static_cast<std::time_t>(e.timestamp_);
                        char tsbuf[20] = {};
                        std::strftime(tsbuf, sizeof(tsbuf), "%b %d %H:%M",
                                      std::localtime(&ts));

                        // Colour: green = success, amber = fail
                        Color ev_col = e.success_
                            ? Color{0.35f, 0.85f, 0.55f, 1.0f}
                            : Color{0.95f, 0.70f, 0.25f, 1.0f};

                        auto type_lbl = std::make_shared<TextWidget>(
                            badge + " " + type + "  " + std::string(e.user_), 12.0f);
                        type_lbl->setColor(ev_col);

                        auto detail_lbl = std::make_shared<TextWidget>(
                            std::string(e.source_) + "  ·  " + tsbuf, 10.0f);
                        detail_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        auto entry = std::make_shared<FlexColumn>(
                            std::vector<std::shared_ptr<Widget>>{type_lbl, detail_lbl});
                        entry->setGap(2);
                        sf->col->addChild(entry);
                    }
                    if (pg.has_more_) {
                        if (auto p = sf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        sf->fetch(0);
    };

    auto sec_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh", [rebuild_sec = ctx.rebuild_sec]() {
        if (rebuild_sec && *rebuild_sec) (*rebuild_sec)();
    });
    sec_refresh_btn->setFixedSize(84, 26);

    auto sec_clear_btn = std::make_shared<ButtonWidget>("🗑 Clear Log", [rebuild_sec = ctx.rebuild_sec]() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.Security",
            static_cast<uint32_t>(SecurityOpcode::ClearEvents), {}, nullptr);
        if (rebuild_sec && *rebuild_sec) (*rebuild_sec)();
    });
    sec_clear_btn->setFixedSize(96, 26);

    ctx.sec_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        sec_refresh_btn, std::make_shared<Spacer>(6, 1), sec_clear_btn
    });
}
// Taskbar "Updates" button. Takes `apps` by reference (program-lifetime) to
// resolve the AppManager executable for the click handler.
void buildUpdatesButtonPanel(ShellContext& ctx, const std::vector<AppMetadata>& apps) {
    // Shows "⬆N" badge when N pkg updates are available; hidden when up-to-date.
    // Clicking launches AppManager so the user can review/apply updates.
    auto upd_count = std::make_shared<uint32_t>(0);
    auto upd_badge_lbl = std::make_shared<TextWidget>("", 9.0f);
    upd_badge_lbl->setColor({1.0f, 1.0f, 1.0f, 1.0f});
    auto upd_badge_box = std::make_shared<ContainerWidget>(upd_badge_lbl);
    upd_badge_box->setBackgroundColor({0.20f, 0.70f, 0.35f, 1.0f}); // green
    upd_badge_box->setPadding(3);
    upd_badge_box->setVisibility(false);
    auto upd_icon_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("⬆", 14.0f), upd_badge_box
    });
    // Callback wired after apps list is populated (forward-declare first)
    ctx.updates_btn = std::make_shared<ButtonWidget>(upd_icon_row, [](){});
    ctx.updates_btn->setPadding(6);
    ctx.updates_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});
    ctx.updates_btn->setVisibility(false); // hidden until first check

    ctx.update_upd_btn = [upd_count, upd_badge_lbl, upd_badge_box, updates_btn = ctx.updates_btn](uint32_t n) {
        *upd_count = n;
        if (n == 0) {
            updates_btn->setVisibility(false);
        } else {
            upd_badge_lbl->setText(std::to_string(n));
            upd_badge_box->setVisibility(true);
            updates_btn->setVisibility(true);
        }
    };

    // Wire click: launch AppManager so the user can review pending updates
    {
        std::string mgr_exec;
        for (const auto& a : apps)
            if (a.identifier == "com.freebsdx.AppManager") { mgr_exec = a.executable_path; break; }
        if (mgr_exec.empty()) mgr_exec = "./out/appmanager"; // fallback
        *ctx.updates_btn = ButtonWidget(ctx.updates_btn->child(), [mgr_exec]() { launchApp(mgr_exec); });
        ctx.updates_btn->setPadding(6);
        ctx.updates_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f}); // reassignment reset styling
    }
}

// Startup apps section (action center). Builds the header label + result column;
// *rebuild_startup_apps is wired later in OnConnect once the apps list is
// available. Behaviour is identical to the former inline block.
void buildStartupAppsPanel(ShellContext& ctx) {
    ctx.startup_apps_hdr_lbl = std::make_shared<TextWidget>("🚀 Startup Apps", 14.0f);
    ctx.startup_apps_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.startup_apps_col = std::make_shared<FlexColumn>();
    ctx.startup_apps_col->setGap(2);
    // *rebuild_startup_apps is wired in OnConnect once apps list is available
    ctx.rebuild_startup_apps = std::make_shared<std::function<void()>>();
}

// ── Builders retained in main.cc (reference file-scope statics) ──
// Session Restore panel. `apps` is main()'s program-lifetime app list, passed by
// reference so the "Save Session" button can match live window titles to known
// executables. Uses the g_session_* file globals + saveSession() directly.
void buildSessionRestorePanel(ShellContext& ctx, const std::vector<AppMetadata>& apps) {
    // live_titles is updated on every WindowListUpdate broadcast
    ctx.live_titles = std::make_shared<std::vector<std::string>>();

    ctx.sess_hdr_lbl = std::make_shared<TextWidget>("💾 Session Restore", 13.0f);
    ctx.sess_hdr_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});
    ctx.sess_switch = std::make_shared<SwitchWidget>(g_session_enabled);
    ctx.sess_switch->setOnChanged([](bool on) {
        g_session_enabled = on;
        saveSession();
    });
    auto sess_sw_lbl = std::make_shared<TextWidget>("Restore on login", 12.0f);
    sess_sw_lbl->setColor({0.70f, 0.70f, 0.75f, 1.0f});
    ctx.sess_sw_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        sess_sw_lbl, std::make_shared<Spacer>(8, 1), ctx.sess_switch
    });
    ctx.sess_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.sess_count_lbl->setColor({0.55f, 0.75f, 0.55f, 1.0f});
    auto sess_save_btn = std::make_shared<ButtonWidget>("💾 Save Session",
        [live_titles = ctx.live_titles, sess_count_lbl = ctx.sess_count_lbl, &apps]() {
            g_session_execs.clear();
            for (const auto& title : *live_titles) {
                std::string tl(title.size(), '\0');
                std::transform(title.begin(), title.end(), tl.begin(), ::tolower);
                for (const auto& app : apps) {
                    std::string nl(app.name.size(), '\0');
                    std::transform(app.name.begin(), app.name.end(), nl.begin(), ::tolower);
                    if (tl.find(nl) != std::string::npos || nl.find(tl) != std::string::npos) {
                        if (!app.executable_path.empty())
                            g_session_execs.push_back(app.executable_path);
                        break;
                    }
                }
            }
            // deduplicate
            std::sort(g_session_execs.begin(), g_session_execs.end());
            g_session_execs.erase(std::unique(g_session_execs.begin(), g_session_execs.end()),
                                  g_session_execs.end());
            saveSession();
            int n = static_cast<int>(g_session_execs.size());
            sess_count_lbl->setText(std::format("{} app{} saved", n, n == 1 ? "" : "s"));
        });
    sess_save_btn->setBackgroundColor({0.18f, 0.28f, 0.45f, 1.0f});
    ctx.sess_save_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        sess_save_btn, std::make_shared<Spacer>(8, 1), ctx.sess_count_lbl
    });
}
