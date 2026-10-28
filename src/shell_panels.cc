// ── xshell panel builders (relocated from main.cc, Phase 2) ─────────────────
// Definitions of the buildXPanel(ctx) functions declared in shell_panels.h.
// See shell_panels.h for the ShellContext + the `using namespace` rationale.
#include "shell_panels.h"
#include <format>
#include <cerrno>
#include <cstring>
#include <algorithm>
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

// Mounted Volumes (paginated Fetcher list). Header + result column; rebuild_vol
// pages com.freebsdx.MountedVolumes ListVolumes (per-mount usage bar + fstype +
// readonly badge). The inline fetch closure stays builder-local. Behaviour is
// identical to the former inline block.
void buildMountedVolumesPanel(ShellContext& ctx) {
    ctx.vol_hdr_lbl = std::make_shared<TextWidget>("💾 Mounted Volumes", 14.0f);
    ctx.vol_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.vol_col     = std::make_shared<FlexColumn>();

    ctx.rebuild_vol = std::make_shared<std::function<void()>>();
    *ctx.rebuild_vol = [vol_col = ctx.vol_col,
                    rv_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_vol)]() {
        if (!g_shell_xbus) return;
        vol_col->clearChildren();
        auto fetch = std::make_shared<std::function<void(uint32_t)>>();
        *fetch = [vol_col, fetch_weak = std::weak_ptr<std::function<void(uint32_t)>>(fetch)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg = page;
            g_shell_xbus->Call("com.freebsdx.MountedVolumes",
                static_cast<uint32_t>(MountedVolOpcode::ListVolumes),
                {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
                [vol_col, fetch_weak](const XBusMessage& r) {
                    VolumePage vp{};
                    std::memcpy(&vp, r.payload_, sizeof(vp));
                    for (uint32_t i = 0; i < vp.count_; ++i) {
                        const auto& e = vp.entries_[i];
                        // Usage percentage and bar (10 blocks)
                        double pct = (e.total_kb_ > 0)
                            ? 100.0 * static_cast<double>(e.used_kb_)
                                    / static_cast<double>(e.total_kb_)
                            : 0.0;
                        int filled = static_cast<int>(pct / 10.0);
                        std::string bar;
                        for (int bi = 0; bi < 10; ++bi)
                            bar += (bi < filled) ? "\xe2\x96\x88" : "\xe2\x96\x91";

                        // Human-readable sizes
                        auto fmt_gb = [](uint64_t kb) -> std::string {
                            if (kb >= 1024*1024)
                                return std::format("{:.1f} GB", kb / (1024.0*1024.0));
                            if (kb >= 1024)
                                return std::format("{:.0f} MB", kb / 1024.0);
                            return std::format("{} KB", kb);
                        };

                        std::string mount_str = e.mount_[0] ? std::string(e.mount_) : "?";
                        std::string fs_str    = e.fstype_[0] ? std::string(e.fstype_) : "";
                        std::string size_str  = fmt_gb(e.used_kb_) + " / " + fmt_gb(e.total_kb_);

                        auto mount_lbl = std::make_shared<TextWidget>(mount_str, 12.0f);
                        auto fs_lbl    = std::make_shared<TextWidget>(fs_str, 10.0f);
                        fs_lbl->setColor({0.50f, 0.65f, 0.90f, 1.0f});
                        auto ro_lbl    = std::make_shared<TextWidget>(
                            e.readonly_ ? "🔒" : "", 10.0f);
                        auto hdr_row   = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                mount_lbl,
                                std::make_shared<Spacer>(1, 1, true),
                                fs_lbl,
                                std::make_shared<Spacer>(4, 1),
                                ro_lbl,
                            });

                        // Bar colour: red if >90%, yellow if >75%, green otherwise
                        Color bar_color = pct > 90.0 ? Color{0.85f, 0.30f, 0.30f, 1.0f}
                                        : pct > 75.0 ? Color{0.85f, 0.70f, 0.20f, 1.0f}
                                                     : Color{0.30f, 0.75f, 0.45f, 1.0f};
                        auto bar_lbl   = std::make_shared<TextWidget>(bar, 10.0f);
                        bar_lbl->setColor(bar_color);
                        auto size_lbl  = std::make_shared<TextWidget>(
                            std::format("{:.0f}%  {}", pct, size_str), 10.0f);
                        size_lbl->setColor({0.65f, 0.65f, 0.70f, 1.0f});

                        vol_col->addChild(hdr_row);
                        vol_col->addChild(bar_lbl);
                        vol_col->addChild(size_lbl);
                        vol_col->addChild(std::make_shared<Spacer>(1, 8));
                    }
                    if (vp.has_more_) {
                        if (auto f = fetch_weak.lock())
                            if (*f) (*f)(vp.page_ + 1);
                    }
                });
        };
        if (*fetch) (*fetch)(0);
    };
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

// Open Network Connections (paginated Fetcher list). Header + result column +
// Refresh button; rebuild_netconn pages com.freebsdx.NetConns ListConns (per-conn
// proto/state/pid header row + local→remote address row, state-coloured). The
// recursive fetch closure stays builder-local. Behaviour is identical to the
// former inline block.
void buildNetConnsPanel(ShellContext& ctx) {
    ctx.netconn_hdr_lbl = std::make_shared<TextWidget>("🌐 Open Connections", 14.0f);
    ctx.netconn_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.netconn_col     = std::make_shared<FlexColumn>();

    ctx.rebuild_netconn = std::make_shared<std::function<void()>>();
    *ctx.rebuild_netconn = [netconn_col = ctx.netconn_col,
                        rnc_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_netconn)]() {
        if (!g_shell_xbus) return;
        netconn_col->clearChildren();
        auto fetch = std::make_shared<std::function<void(uint32_t)>>();
        *fetch = [netconn_col,
                  fetch_weak = std::weak_ptr<std::function<void(uint32_t)>>(fetch)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg = page;
            g_shell_xbus->Call("com.freebsdx.NetConns",
                static_cast<uint32_t>(NetConnOpcode::ListConns),
                {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
                [netconn_col, fetch_weak](const XBusMessage& r) {
                    ConnPage cp{};
                    std::memcpy(&cp, r.payload_, sizeof(cp));
                    for (uint32_t i = 0; i < cp.count_; ++i) {
                        const auto& e = cp.entries_[i];
                        std::string_view state{e.state_};

                        // State colour: green=ESTABL, grey=LISTEN, yellow=TIMEWT/CLOSWT
                        Color state_color =
                            state == "ESTABL" ? Color{0.35f, 0.85f, 0.45f, 1.0f} :
                            state == "LISTEN" ? Color{0.55f, 0.55f, 0.60f, 1.0f} :
                                               Color{0.85f, 0.72f, 0.20f, 1.0f};

                        auto proto_lbl = std::make_shared<TextWidget>(
                            std::string(e.proto_), 10.0f);
                        proto_lbl->setColor({0.50f, 0.65f, 0.90f, 1.0f});

                        auto state_lbl = std::make_shared<TextWidget>(
                            std::string(e.state_), 10.0f);
                        state_lbl->setColor(state_color);

                        auto hdr_row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                proto_lbl,
                                std::make_shared<Spacer>(6, 1),
                                state_lbl,
                                std::make_shared<Spacer>(1, 1, true),
                                std::make_shared<TextWidget>(
                                    e.pid_ ? std::format("pid {}", e.pid_) : "", 10.0f),
                            });

                        auto local_lbl = std::make_shared<TextWidget>(
                            std::string(e.local_), 11.0f);
                        auto arrow_lbl = std::make_shared<TextWidget>("→", 10.0f);
                        arrow_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});
                        auto remote_lbl = std::make_shared<TextWidget>(
                            std::string(e.remote_), 11.0f);
                        remote_lbl->setColor({0.72f, 0.72f, 0.78f, 1.0f});

                        auto addr_row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                local_lbl,
                                std::make_shared<Spacer>(4, 1),
                                arrow_lbl,
                                std::make_shared<Spacer>(4, 1),
                                remote_lbl,
                            });

                        netconn_col->addChild(hdr_row);
                        netconn_col->addChild(addr_row);
                        netconn_col->addChild(std::make_shared<Spacer>(1, 6));
                    }
                    if (cp.has_more_) {
                        if (auto f = fetch_weak.lock())
                            if (*f) (*f)(cp.page_ + 1);
                    }
                });
        };
        if (*fetch) (*fetch)(0);
    };

    ctx.netconn_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh",
        [rebuild_netconn = ctx.rebuild_netconn]() {
            if (rebuild_netconn && *rebuild_netconn) (*rebuild_netconn)();
        });
    ctx.netconn_refresh_btn->setFixedSize(84, 26);
}

// SSH Keys (paginated Fetcher list). Header + count/known-hosts labels + result
// column; rebuild_sshkey pages com.freebsdx.SshKeys ListKeys (per-key algo-coloured
// row + Remove button whose reply re-triggers the rebuild). Page 0 updates the
// count/known-hosts labels. The per-row builder stays inside the fetch closure.
// Behaviour is identical to the former inline block.
void buildSshKeysPanel(ShellContext& ctx) {
    ctx.sshkey_hdr_lbl   = std::make_shared<TextWidget>("🔑 SSH Keys", 14.0f);
    ctx.sshkey_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.sshkey_count_lbl = std::make_shared<TextWidget>("0 keys", 11.0f);
    ctx.sshkey_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.sshkey_known_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.sshkey_known_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});
    ctx.sshkey_col       = std::make_shared<FlexColumn>();

    ctx.rebuild_sshkey   = std::make_shared<std::function<void()>>();
    *ctx.rebuild_sshkey = [sshkey_col = ctx.sshkey_col, sshkey_count_lbl = ctx.sshkey_count_lbl,
                       sshkey_known_lbl = ctx.sshkey_known_lbl,
                       rsk_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_sshkey)]() {
        if (!g_shell_xbus) return;
        sshkey_col->clearChildren();
        auto fetch = std::make_shared<std::function<void(uint32_t)>>();
        *fetch = [sshkey_col, sshkey_count_lbl, sshkey_known_lbl,
                  fetch_weak = std::weak_ptr<std::function<void(uint32_t)>>(fetch),
                  rsk_weak](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg = page;
            g_shell_xbus->Call("com.freebsdx.SshKeys",
                static_cast<uint32_t>(SshKeyOpcode::ListKeys),
                {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
                [sshkey_col, sshkey_count_lbl, sshkey_known_lbl,
                 fetch_weak, rsk_weak](const XBusMessage& r) {
                    SshKeyPage kp{};
                    std::memcpy(&kp, r.payload_, sizeof(kp));
                    if (kp.page_ == 0) {
                        sshkey_count_lbl->setText(
                            std::format("{} key{}", kp.total_,
                                        kp.total_ == 1 ? "" : "s"));
                        if (kp.known_hosts_count_ > 0)
                            sshkey_known_lbl->setText(
                                std::format("{} known host{}",
                                    kp.known_hosts_count_,
                                    kp.known_hosts_count_ == 1 ? "" : "s"));
                    }
                    for (uint32_t i = 0; i < kp.count_; ++i) {
                        const auto& e = kp.entries_[i];
                        std::string_view algo{e.algo_};

                        Color algo_color =
                            algo.find("ed25519") != std::string_view::npos
                                ? Color{0.40f, 0.65f, 0.95f, 1.0f} :
                            algo.find("rsa") != std::string_view::npos
                                ? Color{0.40f, 0.80f, 0.55f, 1.0f} :
                                  Color{0.55f, 0.55f, 0.60f, 1.0f};

                        auto algo_lbl = std::make_shared<TextWidget>(
                            std::string(e.algo_), 10.0f);
                        algo_lbl->setColor(algo_color);

                        auto comment_lbl = std::make_shared<TextWidget>(
                            e.comment_[0] ? std::string(e.comment_) : "(no comment)",
                            11.0f);
                        comment_lbl->setColor({0.82f, 0.82f, 0.88f, 1.0f});

                        uint32_t kid = e.key_id_;
                        auto rm_btn = std::make_shared<ButtonWidget>("\xf0\x9f\x97\x91");
                        rm_btn->setFixedSize(28, 22);
                        rm_btn->setFontSize(12.0f);
                        rm_btn->setBackgroundColor({0.50f, 0.15f, 0.15f, 0.85f});
                        rm_btn->onClick([kid, rsk_weak]() {
                            if (!g_shell_xbus) return;
                            SshKeyRemoveRequest req{};
                            req.key_id_ = kid;
                            g_shell_xbus->Call("com.freebsdx.SshKeys",
                                static_cast<uint32_t>(SshKeyOpcode::RemoveKey),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                [rsk_weak](const XBusMessage&) {
                                    if (auto rsk = rsk_weak.lock())
                                        if (*rsk) (*rsk)();
                                });
                        });

                        auto key_row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                algo_lbl,
                                std::make_shared<Spacer>(6, 1),
                                comment_lbl,
                                std::make_shared<Spacer>(1, 1, true),
                                rm_btn,
                            });
                        sshkey_col->addChild(key_row);
                        sshkey_col->addChild(std::make_shared<Spacer>(1, 5));
                    }
                    if (kp.has_more_) {
                        if (auto f = fetch_weak.lock())
                            if (*f) (*f)(kp.page_ + 1);
                    }
                });
        };
        if (*fetch) (*fetch)(0);
    };
}

// Environment Variables (paginated Fetcher list). Header + count label + result
// column + a "Set Var" form (key/value inputs + Set button, pinned at the top of
// the AC card via envvar_form_row); rebuild_envvar pages com.freebsdx.EnvVars
// ListVars (per-var key=value row, readonly ones get a lock, editable ones an
// Unset button). The form inputs + Set button stay builder-local, kept alive by
// the escaping envvar_form_row. Behaviour is identical to the former inline block.
void buildEnvVarsPanel(ShellContext& ctx) {
    ctx.envvar_hdr_lbl  = std::make_shared<TextWidget>("\xf0\x9f\x8c\x90 Environment", 14.0f);
    ctx.envvar_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.envvar_count_lbl = std::make_shared<TextWidget>("0 vars", 11.0f);
    ctx.envvar_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.envvar_col      = std::make_shared<FlexColumn>();

    // "Set Var" form pinned at top of the AC card
    auto envvar_key_input = std::make_shared<TextInputWidget>("KEY");
    auto envvar_val_input = std::make_shared<TextInputWidget>("value");
    envvar_key_input->setFixedSize(90, 26);
    envvar_val_input->setFixedSize(120, 26);

    ctx.rebuild_envvar  = std::make_shared<std::function<void()>>();
    *ctx.rebuild_envvar = [envvar_col = ctx.envvar_col, envvar_count_lbl = ctx.envvar_count_lbl,
                       rev_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_envvar)]() {
        if (!g_shell_xbus) return;
        envvar_col->clearChildren();
        auto fetch = std::make_shared<std::function<void(uint32_t)>>();
        *fetch = [envvar_col, envvar_count_lbl,
                  fetch_weak = std::weak_ptr<std::function<void(uint32_t)>>(fetch)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg = page;
            g_shell_xbus->Call("com.freebsdx.EnvVars",
                static_cast<uint32_t>(EnvVarOpcode::ListVars),
                {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
                [envvar_col, envvar_count_lbl, fetch_weak](const XBusMessage& r) {
                    EnvVarPage ep{};
                    std::memcpy(&ep, r.payload_, sizeof(ep));
                    if (ep.page_ == 0)
                        envvar_count_lbl->setText(
                            std::format("{} var{}", ep.total_,
                                        ep.total_ == 1 ? "" : "s"));
                    for (uint32_t i = 0; i < ep.count_; ++i) {
                        const auto& e = ep.entries_[i];

                        auto key_lbl = std::make_shared<TextWidget>(
                            std::string(e.key_), 11.0f);
                        key_lbl->setColor({0.50f, 0.70f, 0.95f, 1.0f});

                        auto eq_lbl  = std::make_shared<TextWidget>("=", 11.0f);
                        eq_lbl->setColor({0.40f, 0.40f, 0.45f, 1.0f});

                        auto val_lbl = std::make_shared<TextWidget>(
                            std::string(e.value_), 11.0f);
                        val_lbl->setColor({0.65f, 0.65f, 0.70f, 1.0f});

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                key_lbl,
                                std::make_shared<Spacer>(3, 1),
                                eq_lbl,
                                std::make_shared<Spacer>(3, 1),
                                val_lbl,
                                std::make_shared<Spacer>(1, 1, true),
                            });

                        if (e.readonly_) {
                            auto lock_lbl = std::make_shared<TextWidget>("\xf0\x9f\x94\x92", 11.0f);
                            lock_lbl->setColor({0.40f, 0.40f, 0.45f, 1.0f});
                            row->addChild(lock_lbl);
                        } else {
                                std::string key_str(e.key_);
                            auto unset_btn = std::make_shared<ButtonWidget>("\xe2\x8a\x97");
                            unset_btn->setFixedSize(26, 22);
                            unset_btn->setFontSize(12.0f);
                            unset_btn->setBackgroundColor({0.40f, 0.15f, 0.15f, 0.85f});
                            unset_btn->onClick([key_str, fetch_weak]() {
                                if (!g_shell_xbus) return;
                                EnvUnsetRequest req{};
                                std::strncpy(req.key_, key_str.c_str(), sizeof(req.key_) - 1);
                                g_shell_xbus->Call("com.freebsdx.EnvVars",
                                    static_cast<uint32_t>(EnvVarOpcode::UnsetVar),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    nullptr);
                            });
                            row->addChild(unset_btn);
                        }

                        envvar_col->addChild(row);
                        envvar_col->addChild(std::make_shared<Spacer>(1, 4));
                    }
                    if (ep.has_more_) {
                        if (auto f = fetch_weak.lock())
                            if (*f) (*f)(ep.page_ + 1);
                    }
                });
        };
        if (*fetch) (*fetch)(0);
    };

    auto envvar_set_btn = std::make_shared<ButtonWidget>("Set");
    envvar_set_btn->setFixedSize(38, 26);
    envvar_set_btn->onClick([envvar_key_input, envvar_val_input, rebuild_envvar = ctx.rebuild_envvar]() {
        if (!g_shell_xbus) return;
        const std::string& k = envvar_key_input->text();
        if (k.empty()) return;
        EnvSetRequest req{};
        std::strncpy(req.key_,   k.c_str(), sizeof(req.key_)   - 1);
        std::strncpy(req.value_, envvar_val_input->text().c_str(),
                     sizeof(req.value_) - 1);
        g_shell_xbus->Call("com.freebsdx.EnvVars",
            static_cast<uint32_t>(EnvVarOpcode::SetVar),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [rebuild_envvar](const XBusMessage&) {
                if (rebuild_envvar && *rebuild_envvar) (*rebuild_envvar)();
            });
    });

    ctx.envvar_form_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        envvar_key_input,
        std::make_shared<Spacer>(4, 1),
        envvar_val_input,
        std::make_shared<Spacer>(4, 1),
        envvar_set_btn,
    });
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

// USB Devices (paginated Fetcher list). Header + count label + result column +
// Rescan button; rebuild_usb pages com.freebsdx.UsbDevices ListDevices (per-device
// class-badge/name/vid:pid row; page 0 sets the count label; Rescan calls the
// service then rebuilds). The recursive fetch closure stays builder-local.
// Behaviour identical to the former inline block.
void buildUsbDevicesPanel(ShellContext& ctx) {
    ctx.usb_hdr_lbl  = std::make_shared<TextWidget>("\xf0\x9f\x94\x8c USB Devices", 14.0f);
    ctx.usb_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.usb_count_lbl = std::make_shared<TextWidget>("0 devices", 11.0f);
    ctx.usb_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.usb_col      = std::make_shared<FlexColumn>();

    ctx.rebuild_usb  = std::make_shared<std::function<void()>>();
    *ctx.rebuild_usb = [usb_col = ctx.usb_col, usb_count_lbl = ctx.usb_count_lbl,
                    ru_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_usb)]() {
        if (!g_shell_xbus) return;
        usb_col->clearChildren();
        auto fetch = std::make_shared<std::function<void(uint32_t)>>();
        *fetch = [usb_col, usb_count_lbl,
                  fetch_weak = std::weak_ptr<std::function<void(uint32_t)>>(fetch)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg = page;
            g_shell_xbus->Call("com.freebsdx.UsbDevices",
                static_cast<uint32_t>(UsbDevOpcode::ListDevices),
                {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
                [usb_col, usb_count_lbl, fetch_weak](const XBusMessage& r) {
                    UsbDevPage up{};
                    std::memcpy(&up, r.payload_, sizeof(up));
                    if (up.page_ == 0)
                        usb_count_lbl->setText(
                            std::format("{} device{}", up.total_,
                                        up.total_ == 1 ? "" : "s"));
                    for (uint32_t i = 0; i < up.count_; ++i) {
                        const auto& e = up.entries_[i];
                        std::string_view cls{e.class_};

                        // Class badge colour
                        Color cls_color =
                            cls == "HID" ? Color{0.45f, 0.75f, 0.95f, 1.0f} :
                            cls == "MSC" ? Color{0.45f, 0.85f, 0.55f, 1.0f} :
                            cls == "HUB" ? Color{0.85f, 0.72f, 0.30f, 1.0f} :
                            cls == "AUD" ? Color{0.75f, 0.50f, 0.90f, 1.0f} :
                            cls == "CDC" ? Color{0.90f, 0.55f, 0.35f, 1.0f} :
                                           Color{0.55f, 0.55f, 0.60f, 1.0f};

                        auto cls_lbl  = std::make_shared<TextWidget>(
                            std::string(e.class_), 10.0f);
                        cls_lbl->setColor(cls_color);

                        auto name_lbl = std::make_shared<TextWidget>(
                            e.name_[0] ? std::string(e.name_) : "(unknown)", 11.0f);
                        name_lbl->setColor({0.82f, 0.82f, 0.88f, 1.0f});

                        auto vid_pid_lbl = std::make_shared<TextWidget>(
                            std::format("{:04x}:{:04x}", e.vid_, e.pid_), 10.0f);
                        vid_pid_lbl->setColor({0.42f, 0.42f, 0.48f, 1.0f});

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                cls_lbl,
                                std::make_shared<Spacer>(6, 1),
                                name_lbl,
                                std::make_shared<Spacer>(1, 1, true),
                                vid_pid_lbl,
                            });
                        usb_col->addChild(row);
                        usb_col->addChild(std::make_shared<Spacer>(1, 5));
                    }
                    if (up.has_more_) {
                        if (auto f = fetch_weak.lock())
                            if (*f) (*f)(up.page_ + 1);
                    }
                });
        };
        if (*fetch) (*fetch)(0);
    };

    ctx.usb_rescan_btn = std::make_shared<ButtonWidget>("\xe2\x86\xba Rescan",
        [rebuild_usb = ctx.rebuild_usb]() {
            if (!g_shell_xbus) return;
            g_shell_xbus->Call("com.freebsdx.UsbDevices",
                static_cast<uint32_t>(UsbDevOpcode::Rescan), {}, nullptr);
            if (rebuild_usb && *rebuild_usb) (*rebuild_usb)();
        });
    ctx.usb_rescan_btn->setFixedSize(80, 26);
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

// Battery Drain (paginated Fetcher list). Header + result column + Refresh button
// (Refresh calls RefreshDrain then rebuilds); rebuild_bdrain pages
// com.freebsdx.BatteryDrain ListDrain (per-app impact-badge/name + energy-% + Quit
// button that SIGTERMs the pid, greyed when pid==0). The add_entry row-builder
// stays a local closure inside the reply. Behaviour identical to the former
// inline block.
void buildBatteryDrainPanel(ShellContext& ctx) {
    ctx.bdrain_hdr_lbl = std::make_shared<TextWidget>("🔋 Battery Drain", 14.0f);
    ctx.bdrain_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});

    ctx.bdrain_col = std::make_shared<FlexColumn>();

    ctx.rebuild_bdrain = std::make_shared<std::function<void()>>();
    *ctx.rebuild_bdrain = [bdrain_col = ctx.bdrain_col,
                       rbd_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_bdrain)]() {
        if (!g_shell_xbus) return;
        uint32_t page = 0;
        g_shell_xbus->Call("com.freebsdx.BatteryDrain",
            static_cast<uint32_t>(BatteryDrainOpcode::ListDrain),
            {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
            [bdrain_col, rbd_weak](const XBusMessage& r) {
                BatteryDrainPage pg{};
                std::memcpy(&pg, r.payload_, sizeof(pg));
                bdrain_col->clearChildren();
                if (!pg.count_) {
                    auto empty = std::make_shared<TextWidget>(
                        "No drain data available", 11.0f);
                    empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                    bdrain_col->addChild(empty);
                    return;
                }
                auto add_entry = [bdrain_col](const BatteryDrainEntry& e) {
                    static const char* kImpactBadge[] = { "🟢", "🟡", "🔴" };
                    uint32_t imp = std::min(e.impact_, 2u);
                    std::string badge = kImpactBadge[imp];
                    std::string name(e.app_name_);
                    auto name_lbl = std::make_shared<TextWidget>(
                        badge + " " + name, 12.0f);
                    name_lbl->setColor({0.90f, 0.90f, 0.95f, 1.0f});
                    auto pct_lbl = std::make_shared<TextWidget>(
                        std::format("{}%", e.energy_pct_), 11.0f);
                    pct_lbl->setColor(imp == 2
                        ? Color{0.90f, 0.40f, 0.35f, 1.0f}
                        : Color{0.55f, 0.55f, 0.60f, 1.0f});
                    uint32_t pid = e.pid_;
                    auto quit_btn = std::make_shared<ButtonWidget>("⏏ Quit",
                        [pid]() {
                            if (!g_shell_xbus || pid == 0) return;
                            ProcessKillRequest req{};
                            req.pid_    = pid;
                            req.signal_ = 15; // SIGTERM
                            g_shell_xbus->Call("com.freebsdx.Process",
                                static_cast<uint32_t>(ProcessOpcode::Kill),
                                {reinterpret_cast<const uint8_t*>(&req),
                                 sizeof(req)}, nullptr);
                        });
                    quit_btn->setFontSize(10.0f);
                    quit_btn->setFixedSize(58, 22);
                    quit_btn->setBackgroundColor(pid == 0
                        ? Color{0.20f, 0.20f, 0.22f, 1.0f}  // greyed: no pid
                        : Color{0.35f, 0.10f, 0.10f, 1.0f});
                    auto row = std::make_shared<FlexRow>(
                        std::vector<std::shared_ptr<Widget>>{
                            name_lbl,
                            std::make_shared<Spacer>(1, 1, true),
                            pct_lbl,
                            std::make_shared<Spacer>(6, 1),
                            quit_btn,
                        });
                    row->setBackgroundColor({0.10f, 0.12f, 0.18f, 1.0f});
                    row->setPadding(6);
                    bdrain_col->addChild(row);
                };
                for (uint32_t i = 0; i < pg.count_; ++i)
                    add_entry(pg.entries_[i]);
                if (pg.has_more_) {
                    for (uint32_t np = 1; np < (pg.total_ + 2) / 3; ++np) {
                        uint32_t p2 = np;
                        if (!g_shell_xbus) break;
                        g_shell_xbus->Call("com.freebsdx.BatteryDrain",
                            static_cast<uint32_t>(BatteryDrainOpcode::ListDrain),
                            {reinterpret_cast<const uint8_t*>(&p2), sizeof(p2)},
                            [bdrain_col, add_entry](const XBusMessage& r2) {
                                BatteryDrainPage pg2{};
                                std::memcpy(&pg2, r2.payload_, sizeof(pg2));
                                for (uint32_t j = 0; j < pg2.count_; ++j)
                                    add_entry(pg2.entries_[j]);
                            });
                    }
                }
            });
    };

    ctx.bdrain_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh",
        [rebuild_bdrain = ctx.rebuild_bdrain]() {
            if (!g_shell_xbus) return;
            g_shell_xbus->Call("com.freebsdx.BatteryDrain",
                static_cast<uint32_t>(BatteryDrainOpcode::RefreshDrain), {},
                [rebuild_bdrain](const XBusMessage&) {
                    if (rebuild_bdrain && *rebuild_bdrain) (*rebuild_bdrain)();
                });
        });
    ctx.bdrain_refresh_btn->setFixedSize(84, 26);
}

// File Sync (paginated Fetcher list). Header + count label + result column;
// rebuild_file_sync uses a local FsFetcher struct (col + count_lbl + a weak-self
// recursive fetch) to page com.freebsdx.FileSync ListJobs (per-job name/path +
// state label with progress + Pause/Resume button). The FsFetcher struct stays
// inside the rebuild closure. Behaviour identical to the former inline block.
void buildFileSyncPanel(ShellContext& ctx) {
    ctx.fs_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x94\x84 File Sync", 14.0f);
    ctx.fs_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.fs_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.fs_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.fs_col       = std::make_shared<FlexColumn>();
    ctx.fs_col->setGap(6);

    ctx.rebuild_file_sync = std::make_shared<std::function<void()>>();
    *ctx.rebuild_file_sync = [fs_col = ctx.fs_col, fs_count_lbl = ctx.fs_count_lbl,
                          rfs_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_file_sync)]() {
        if (!g_shell_xbus) return;
        fs_col->clearChildren();

        struct FsFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count_lbl;
            std::function<void(uint32_t)> fetch;
        };
        auto ff = std::make_shared<FsFetcher>();
        ff->col       = fs_col;
        ff->count_lbl = fs_count_lbl;
        ff->fetch = [ff_weak = std::weak_ptr<FsFetcher>(ff)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.FileSync",
                static_cast<uint32_t>(FileSyncOpcode::ListJobs),
                {reinterpret_cast<const uint8_t*>(&pg_num), sizeof(pg_num)},
                [ff_weak, page](const XBusMessage& resp) {
                    auto ff = ff_weak.lock();
                    if (!ff) return;
                    FileSyncPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>(
                            "No sync jobs configured", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        ff->col->addChild(empty);
                        ff->count_lbl->setText("");
                        return;
                    }
                    if (page == 0)
                        ff->count_lbl->setText(std::format("{} job{}",
                            pg.total_, pg.total_ == 1u ? "" : "s"));
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        static constexpr const char* kFsStateLabel[] = {
                            "Up to Date", "Syncing", "Paused", "Error"};
                        static const Color kFsStateColor[] = {
                            {0.35f, 0.85f, 0.45f, 1.0f}, // green
                            {0.30f, 0.65f, 0.95f, 1.0f}, // blue
                            {0.50f, 0.50f, 0.55f, 1.0f}, // grey
                            {0.90f, 0.30f, 0.30f, 1.0f}, // red
                        };
                        uint32_t s = e.state_ < 4u ? e.state_ : 3u;
                        bool is_paused = (s == 2u);

                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(e.name_), 12.0f);
                        name_lbl->setColor({0.88f, 0.88f, 0.92f, 1.0f});

                        std::string state_text = kFsStateLabel[s];
                        if (s == 1u)
                            state_text += std::format("  {}%", e.progress_);
                        auto state_lbl = std::make_shared<TextWidget>(state_text, 11.0f);
                        state_lbl->setColor(kFsStateColor[s]);

                        auto path_lbl = std::make_shared<TextWidget>(
                            std::string(e.path_), 10.0f);
                        path_lbl->setColor({0.40f, 0.40f, 0.45f, 1.0f});

                        std::string job_name(e.name_);
                        auto action_btn = std::make_shared<ButtonWidget>(
                            is_paused ? "Resume" : "Pause",
                            [job_name, is_paused]() {
                                if (!g_shell_xbus) return;
                                FileSyncActionRequest req{};
                                std::strncpy(req.name_, job_name.c_str(),
                                    sizeof(req.name_) - 1);
                                g_shell_xbus->Call("com.freebsdx.FileSync",
                                    static_cast<uint32_t>(is_paused
                                        ? FileSyncOpcode::ResumeJob
                                        : FileSyncOpcode::PauseJob),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    nullptr);
                            });
                        action_btn->setFixedSize(64, 24);
                        action_btn->setBackgroundColor(is_paused
                            ? Color{0.20f, 0.55f, 0.90f, 1.0f}
                            : Color{0.30f, 0.30f, 0.35f, 1.0f});

                        auto hdr_row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl,
                                std::make_shared<Spacer>(1, 1, true),
                                action_btn});
                        auto entry = std::make_shared<FlexColumn>(
                            std::vector<std::shared_ptr<Widget>>{
                                hdr_row, path_lbl, state_lbl});
                        entry->setGap(2);
                        ff->col->addChild(entry);
                    }
                    if (pg.has_more_) {
                        if (auto p = ff_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        ff->fetch(0);
    };
}

// Audio Routing (paginated Fetcher list). Header + count label + result column;
// rebuild_audio_routes uses a local AudioRouteFetcher struct (col + count_lbl +
// rebuild-weak + total_apps + a self-held recursive fetch) to page
// com.freebsdx.AudioRouting ListRoutes (per-app name/device + a cycle button that
// advances the device index then re-triggers the rebuild). The fetcher struct
// stays inside the rebuild closure. Behaviour identical to the former inline block.
void buildAudioRoutingPanel(ShellContext& ctx) {
    ctx.ar_hdr_lbl      = std::make_shared<TextWidget>("\xf0\x9f\x94\x8a Audio Routing", 14.0f);
    ctx.ar_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.ar_count_lbl    = std::make_shared<TextWidget>("No active audio", 11.0f);
    ctx.ar_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.audio_route_col = std::make_shared<FlexColumn>();
    ctx.audio_route_col->setGap(4);

    ctx.rebuild_audio_routes = std::make_shared<std::function<void()>>();
    *ctx.rebuild_audio_routes = [audio_route_col = ctx.audio_route_col, ar_count_lbl = ctx.ar_count_lbl,
                              rar_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_audio_routes)]() {
        if (!g_shell_xbus) return;
        audio_route_col->clearChildren();

        struct AudioRouteFetcher {
            std::shared_ptr<FlexColumn>             col;
            std::shared_ptr<TextWidget>             count_lbl;
            std::weak_ptr<std::function<void()>>    rebuild_weak;
            uint32_t                                total_apps{0};
            std::function<void(uint32_t)>           fetch;
        };
        auto arf = std::make_shared<AudioRouteFetcher>();
        arf->col          = audio_route_col;
        arf->count_lbl    = ar_count_lbl;
        arf->rebuild_weak = rar_weak;

        arf->fetch = [arf](uint32_t page) {
            g_shell_xbus->Call("com.freebsdx.AudioRouting",
                static_cast<uint32_t>(AudioRoutingOpcode::ListRoutes),
                {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
                [arf](const XBusMessage& r) {
                    AudioRoutePage pg{};
                    std::memcpy(&pg, r.payload_, sizeof(pg));
                    if (pg.page_ == 0) arf->total_apps = pg.total_;

                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        std::string app_name(e.app_name_,
                            std::min(std::strlen(e.app_name_), sizeof(e.app_name_) - 1));
                        std::string dev_name(e.device_name_,
                            std::min(std::strlen(e.device_name_), sizeof(e.device_name_) - 1));
                        std::string app_id(e.app_id_,
                            std::min(std::strlen(e.app_id_), sizeof(e.app_id_) - 1));
                        uint32_t dev_count = e.device_count_;

                        auto name_lbl  = std::make_shared<TextWidget>(app_name, 11.0f);
                        name_lbl->setColor({0.45f, 0.70f, 1.0f, 1.0f});
                        auto dev_lbl   = std::make_shared<TextWidget>(dev_name, 10.0f);
                        dev_lbl->setColor({0.65f, 0.65f, 0.70f, 1.0f});
                        auto cycle_btn = std::make_shared<ButtonWidget>("\xe2\x96\xb6");
                        cycle_btn->setFixedSize(28, 22);
                        cycle_btn->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});
                        uint32_t cur_idx = e.device_idx_;
                        cycle_btn->onClick([app_id, dev_lbl, cur_idx, dev_count,
                                            rebuild_weak = arf->rebuild_weak]() mutable {
                            if (!g_shell_xbus || dev_count == 0) return;
                            AudioRouteSetRequest req{};
                            std::strncpy(req.app_id_, app_id.c_str(), sizeof(req.app_id_) - 1);
                            req.device_idx_ = (cur_idx + 1) % dev_count;
                            g_shell_xbus->Call("com.freebsdx.AudioRouting",
                                static_cast<uint32_t>(AudioRoutingOpcode::SetRoute),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                [rebuild_weak](const XBusMessage&) {
                                    if (auto rb = rebuild_weak.lock()) (*rb)();
                                });
                        });

                        arf->col->addChild(std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl,
                                std::make_shared<Spacer>(6, 1),
                                dev_lbl,
                                std::make_shared<Spacer>(1, 1, true),
                                cycle_btn,
                            }));
                    }

                    if (pg.has_more_) {
                        arf->fetch(pg.page_ + 1);
                    } else {
                        if (arf->total_apps == 0) {
                            auto empty = std::make_shared<TextWidget>("No active audio", 11.0f);
                            empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                            arf->col->addChild(empty);
                            arf->count_lbl->setText("No active audio");
                        } else {
                            arf->count_lbl->setText(
                                std::format("{} stream{}", arf->total_apps,
                                    arf->total_apps == 1 ? "" : "s"));
                        }
                    }
                });
        };
        arf->fetch(0);
    };
}

// Network Shares (paginated Fetcher list). Header + count label + result column;
// rebuild_shares uses a local ShareFetcher struct (col + count_lbl + rebuild-weak
// + total + self-held recursive fetch) to page com.freebsdx.NetworkShares
// ListShares (per-share status dot + name/type/path + Mount/Unmount button that
// re-triggers the rebuild). The ShareFetcher struct stays inside the rebuild
// closure. Behaviour identical to the former inline block.
void buildNetworkSharesPanel(ShellContext& ctx) {
    ctx.ns_hdr_lbl      = std::make_shared<TextWidget>("\xf0\x9f\x93\x81 Network Shares", 14.0f);
    ctx.ns_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.ns_count_lbl    = std::make_shared<TextWidget>("No shares", 11.0f);
    ctx.ns_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.share_col       = std::make_shared<FlexColumn>();
    ctx.share_col->setGap(6);

    ctx.rebuild_shares  = std::make_shared<std::function<void()>>();
    *ctx.rebuild_shares = [share_col = ctx.share_col, ns_count_lbl = ctx.ns_count_lbl,
                       rsw = std::weak_ptr<std::function<void()>>(ctx.rebuild_shares)]() {
        if (!g_shell_xbus) return;
        share_col->clearChildren();

        struct ShareFetcher {
            std::shared_ptr<FlexColumn>          col;
            std::shared_ptr<TextWidget>          count_lbl;
            std::weak_ptr<std::function<void()>> rebuild_weak;
            uint32_t                             total{0};
            std::function<void(uint32_t)>        fetch;
        };
        auto sf = std::make_shared<ShareFetcher>();
        sf->col          = share_col;
        sf->count_lbl    = ns_count_lbl;
        sf->rebuild_weak = rsw;

        sf->fetch = [sf](uint32_t page) {
            g_shell_xbus->Call("com.freebsdx.NetworkShares",
                static_cast<uint32_t>(NetworkShareOpcode::ListShares),
                {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
                [sf](const XBusMessage& r) {
                    NetworkSharePage pg{};
                    std::memcpy(&pg, r.payload_, sizeof(pg));
                    if (pg.page_ == 0) sf->total = pg.total_;

                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        std::string name(e.name_,
                            std::min(std::strlen(e.name_), sizeof(e.name_) - 1));
                        std::string path(e.path_,
                            std::min(std::strlen(e.path_), sizeof(e.path_) - 1));
                        std::string type(e.type_,
                            std::min(std::strlen(e.type_), sizeof(e.type_) - 1));
                        uint32_t mounted = e.mounted_;

                        auto name_lbl = std::make_shared<TextWidget>(name, 11.0f);
                        name_lbl->setColor({0.45f, 0.70f, 1.0f, 1.0f});
                        auto path_lbl = std::make_shared<TextWidget>(path, 10.0f);
                        path_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
                        auto type_lbl = std::make_shared<TextWidget>(type, 10.0f);
                        type_lbl->setColor({0.60f, 0.60f, 0.65f, 1.0f});

                        // Mount/Unmount button — colour by state
                        bool is_mounted = (mounted == 1);
                        bool is_error   = (mounted == 2);
                        auto act_btn = std::make_shared<ButtonWidget>(
                            is_mounted ? "Unmount" : "Mount");
                        act_btn->setFixedSize(64, 22);
                        act_btn->setBackgroundColor(is_error
                            ? Color{0.50f, 0.25f, 0.08f, 1.0f}
                            : is_mounted
                                ? Color{0.12f, 0.36f, 0.16f, 1.0f}
                                : Color{0.22f, 0.22f, 0.26f, 1.0f});
                        act_btn->onClick([name, is_mounted,
                                          rebuild_weak = sf->rebuild_weak]() {
                            if (!g_shell_xbus) return;
                            ShareActionRequest req{};
                            std::strncpy(req.name_, name.c_str(), sizeof(req.name_) - 1);
                            auto op = is_mounted
                                ? NetworkShareOpcode::UnmountShare
                                : NetworkShareOpcode::MountShare;
                            g_shell_xbus->Call("com.freebsdx.NetworkShares",
                                static_cast<uint32_t>(op),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                [rebuild_weak](const XBusMessage&) {
                                    if (auto rb = rebuild_weak.lock()) (*rb)();
                                });
                        });

                        // Status dot
                        auto dot_lbl = std::make_shared<TextWidget>(
                            is_error ? "⚠" : (is_mounted ? "●" : "○"), 11.0f);
                        dot_lbl->setColor(is_error
                            ? Color{0.95f, 0.55f, 0.20f, 1.0f}
                            : is_mounted
                                ? Color{0.30f, 0.85f, 0.45f, 1.0f}
                                : Color{0.45f, 0.45f, 0.50f, 1.0f});

                        sf->col->addChild(std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                dot_lbl,
                                std::make_shared<Spacer>(6, 1),
                                name_lbl,
                                std::make_shared<Spacer>(6, 1),
                                type_lbl,
                                std::make_shared<Spacer>(1, 1, true),
                                act_btn,
                            }));
                        sf->col->addChild(path_lbl);
                    }

                    if (pg.has_more_) {
                        sf->fetch(pg.page_ + 1);
                    } else {
                        if (sf->total == 0) {
                            auto empty = std::make_shared<TextWidget>("No shares configured", 11.0f);
                            empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                            sf->col->addChild(empty);
                            sf->count_lbl->setText("No shares");
                        } else {
                            sf->count_lbl->setText(
                                std::format("{} share{}", sf->total,
                                    sf->total == 1 ? "" : "s"));
                        }
                    }
                });
        };
        sf->fetch(0);
    };
}

// Printer Queue (paginated Fetcher list). Count label + result column;
// rebuild_print_queue uses a local PrintFetcher struct (col + count_lbl +
// rebuild-weak + total_jobs + self-held recursive fetch) to page
// com.freebsdx.PrintQueue ListJobs (per-job printer/document/pages + status label
// + Cancel button that re-triggers the rebuild). The PrintFetcher struct stays
// inside the rebuild closure. The header label is latent (never added to a card),
// so it stays a builder-local. Behaviour identical to the former inline block.
void buildPrinterQueuePanel(ShellContext& ctx) {
    auto print_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x96\xa8 Printer Queue", 14.0f);
    print_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.print_count_lbl = std::make_shared<TextWidget>("No jobs", 11.0f);
    ctx.print_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.print_col       = std::make_shared<FlexColumn>();
    ctx.print_col->setGap(4);

    ctx.rebuild_print_queue = std::make_shared<std::function<void()>>();
    *ctx.rebuild_print_queue = [print_col = ctx.print_col, print_count_lbl = ctx.print_count_lbl,
                            rpq_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_print_queue)]() {
        if (!g_shell_xbus) return;
        print_col->clearChildren();

        struct PrintFetcher {
            std::shared_ptr<FlexColumn> col;
            std::shared_ptr<TextWidget> count_lbl;
            std::weak_ptr<std::function<void()>> rebuild_weak;
            uint32_t total_jobs{0};
            std::function<void(uint32_t)> fetch;
        };
        auto pf = std::make_shared<PrintFetcher>();
        pf->col        = print_col;
        pf->count_lbl  = print_count_lbl;
        pf->rebuild_weak = rpq_weak;

        pf->fetch = [pf](uint32_t page) {
            g_shell_xbus->Call("com.freebsdx.PrintQueue",
                static_cast<uint32_t>(PrintOpcode::ListJobs),
                {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
                [pf, page](const XBusMessage& r) {
                    PrintJobPage pg{};
                    std::memcpy(&pg, r.payload_, sizeof(pg));
                    if (pg.page_ == 0) pf->total_jobs = pg.total_;

                    static const char* kStatusLabels[] = {
                        "Pending", "Held", "Printing", "Stopped"
                    };
                    static const Color kStatusColors[] = {
                        {0.70f, 0.70f, 0.75f, 1.0f}, // pending — grey
                        {0.80f, 0.65f, 0.20f, 1.0f}, // held — amber
                        {0.35f, 0.80f, 0.45f, 1.0f}, // printing — green
                        {0.85f, 0.35f, 0.35f, 1.0f}, // stopped — red
                    };

                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        uint32_t    st_idx = std::min(e.status_, 3u);
                        std::string job_id_str = std::to_string(e.job_id_);
                        std::string printer_str(e.printer_);
                        std::string doc_str(e.document_);

                        auto printer_lbl = std::make_shared<TextWidget>(printer_str, 10.0f);
                        printer_lbl->setColor({0.45f, 0.65f, 0.90f, 1.0f});

                        auto doc_lbl = std::make_shared<TextWidget>(doc_str, 11.0f);
                        doc_lbl->setColor({0.78f, 0.78f, 0.82f, 1.0f});

                        auto st_lbl = std::make_shared<TextWidget>(kStatusLabels[st_idx], 10.0f);
                        st_lbl->setColor(kStatusColors[st_idx]);

                        auto pages_lbl = std::make_shared<TextWidget>(
                            e.pages_ ? std::format("{} pg", e.pages_) : "? pg", 10.0f);
                        pages_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

                        auto cancel_btn = std::make_shared<ButtonWidget>("\xc3\x97");
                        cancel_btn->setFixedSize(26, 22);
                        cancel_btn->setBackgroundColor({0.45f, 0.12f, 0.12f, 1.0f});
                        uint32_t jid = e.job_id_;
                        cancel_btn->onClick([jid, pf]() {
                            if (!g_shell_xbus) return;
                            PrintCancelRequest req{};
                            req.job_id_ = jid;
                            g_shell_xbus->Call("com.freebsdx.PrintQueue",
                                static_cast<uint32_t>(PrintOpcode::CancelJob),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                [pf](const XBusMessage&) {
                                    if (auto rp = pf->rebuild_weak.lock())
                                        if (*rp) (*rp)();
                                });
                        });

                        auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                            printer_lbl,
                            std::make_shared<Spacer>(6, 1),
                            doc_lbl,
                            std::make_shared<Spacer>(1, 1, true),
                            pages_lbl,
                            std::make_shared<Spacer>(6, 1),
                            st_lbl,
                            std::make_shared<Spacer>(6, 1),
                            cancel_btn,
                        });
                        pf->col->addChild(row);
                        pf->col->addChild(std::make_shared<Spacer>(1, 4));
                    }

                    if (pg.has_more_) {
                        pf->fetch(page + 1);
                    } else {
                        uint32_t shown = pf->total_jobs;
                        pf->count_lbl->setText(
                            shown == 0 ? "No jobs" : std::format("{} job{}", shown, shown != 1 ? "s" : ""));
                        if (shown == 0)
                            pf->col->addChild(std::make_shared<TextWidget>("Queue is empty", 11.0f));
                    }
                });
        };
        pf->fetch(0);
    };
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

// ZFS Pool Health (paginated Fetcher list). Header + count label + result column;
// rebuild_zfs uses a local ZfsFetcher struct (col + count_lbl + rebuild-weak +
// total + self-held recursive fetch) to page com.freebsdx.ZfsPools ListPools
// (per-pool name/health badge + capacity bar + used/total size + a Scrub button
// or scrub-progress label; Scrub re-triggers the rebuild). The ZfsFetcher struct
// stays inside the rebuild closure. Behaviour identical to the former inline block.
void buildZfsPoolsPanel(ShellContext& ctx) {
    ctx.zfs_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x97\x84 ZFS Pools", 14.0f);
    ctx.zfs_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.zfs_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.zfs_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.zfs_col       = std::make_shared<FlexColumn>();
    ctx.zfs_col->setGap(8);

    ctx.rebuild_zfs = std::make_shared<std::function<void()>>();
    *ctx.rebuild_zfs = [zfs_col = ctx.zfs_col, zfs_count_lbl = ctx.zfs_count_lbl,
                   rz_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_zfs)]() {
        if (!g_shell_xbus) return;
        zfs_col->clearChildren();

        struct ZfsFetcher {
            std::shared_ptr<FlexColumn> col;
            std::shared_ptr<TextWidget> count_lbl;
            std::weak_ptr<std::function<void()>> rebuild_weak;
            uint32_t total{0};
            std::function<void(uint32_t)> fetch;
        };
        auto zf = std::make_shared<ZfsFetcher>();
        zf->col          = zfs_col;
        zf->count_lbl    = zfs_count_lbl;
        zf->rebuild_weak = rz_weak;

        zf->fetch = [zf](uint32_t page) {
            g_shell_xbus->Call("com.freebsdx.ZfsPools",
                static_cast<uint32_t>(ZfsOpcode::ListPools),
                {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
                [zf, page](const XBusMessage& r) {
                    ZfsPoolPage pg{};
                    std::memcpy(&pg, r.payload_, sizeof(pg));
                    if (pg.page_ == 0) zf->total = pg.total_;

                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        std::string pool_name(e.name_);
                        std::string health_str(e.health_);

                        // Health badge colour
                        Color health_color = {0.50f, 0.50f, 0.55f, 1.0f}; // dim default
                        if (health_str == "ONLINE")
                            health_color = {0.35f, 0.80f, 0.45f, 1.0f};
                        else if (health_str == "DEGRADED")
                            health_color = {0.95f, 0.72f, 0.20f, 1.0f};
                        else if (health_str == "FAULTED" || health_str == "UNAVAIL")
                            health_color = {0.90f, 0.30f, 0.30f, 1.0f};

                        auto name_lbl   = std::make_shared<TextWidget>(pool_name, 12.0f);
                        name_lbl->setColor({0.88f, 0.88f, 0.92f, 1.0f});

                        auto health_lbl = std::make_shared<TextWidget>(health_str, 10.0f);
                        health_lbl->setColor(health_color);

                        // Capacity bar (10 blocks)
                        constexpr int kZfsBarBlocks = 10;
                        auto bar_row = std::make_shared<FlexRow>();
                        bar_row->setGap(2);
                        int filled = (e.total_bytes_ > 0)
                            ? static_cast<int>((e.used_bytes_ * kZfsBarBlocks) / e.total_bytes_)
                            : 0;
                        filled = std::clamp(filled, 0, kZfsBarBlocks);
                        for (int b = 0; b < kZfsBarBlocks; ++b) {
                            auto blk = std::make_shared<TextWidget>("\xe2\x96\x88", 9.0f);
                            blk->setColor(b < filled ? health_color
                                                      : Color{0.20f, 0.20f, 0.22f, 1.0f});
                            bar_row->addChild(blk);
                        }

                        double used_gib  = static_cast<double>(e.used_bytes_)  / (1024.0*1024.0*1024.0);
                        double total_gib = static_cast<double>(e.total_bytes_) / (1024.0*1024.0*1024.0);
                        auto size_lbl = std::make_shared<TextWidget>(
                            std::format("{:.1f} / {:.1f} GiB", used_gib, total_gib), 10.0f);
                        size_lbl->setColor({0.60f, 0.60f, 0.65f, 1.0f});

                        // Scrub button or scrub progress
                        std::shared_ptr<Widget> scrub_widget;
                        if (e.scrubbing_) {
                            auto prog_lbl = std::make_shared<TextWidget>(
                                std::format("scrubbing {}%…", e.scrub_pct_), 10.0f);
                            prog_lbl->setColor({0.45f, 0.65f, 0.90f, 1.0f});
                            scrub_widget = prog_lbl;
                        } else {
                            auto scrub_btn = std::make_shared<ButtonWidget>("\xe2\x86\xbb Scrub");
                            scrub_btn->setFixedSize(72, 22);
                            scrub_btn->onClick([pool_name, zf]() {
                                if (!g_shell_xbus) return;
                                ZfsScrubRequest req{};
                                std::strncpy(req.name_, pool_name.c_str(), sizeof(req.name_) - 1);
                                g_shell_xbus->Call("com.freebsdx.ZfsPools",
                                    static_cast<uint32_t>(ZfsOpcode::StartScrub),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    [zf](const XBusMessage&) {
                                        if (auto rz = zf->rebuild_weak.lock())
                                            if (*rz) (*rz)();
                                    });
                            });
                            scrub_widget = scrub_btn;
                        }

                        auto hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                            name_lbl,
                            std::make_shared<Spacer>(1, 1, true),
                            health_lbl,
                        });
                        auto info_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                            size_lbl,
                            std::make_shared<Spacer>(1, 1, true),
                            scrub_widget,
                        });

                        auto pool_col = std::make_shared<FlexColumn>();
                        pool_col->setGap(4);
                        pool_col->addChild(hdr_row);
                        pool_col->addChild(bar_row);
                        pool_col->addChild(info_row);
                        zf->col->addChild(pool_col);
                    }

                    if (pg.has_more_) {
                        zf->fetch(page + 1);
                    } else {
                        uint32_t n = zf->total;
                        zf->count_lbl->setText(
                            n == 0 ? "No pools" : std::format("{} pool{}", n, n != 1 ? "s" : ""));
                        if (n == 0)
                            zf->col->addChild(std::make_shared<TextWidget>("No ZFS pools found.", 11.0f));
                    }
                });
        };
        zf->fetch(0);
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

// Screenshot & Recordings (paginated Fetcher list). Header + count label + result
// column + a Screenshot action button; rebuild_captures uses a local CapFetcher
// struct (col + count_lbl + rebuild-weak + total_seen + self-held
// shared_ptr<function> fetch) to page com.freebsdx.Captures ListCaptures (per-file
// REC/IMG badge + name + timestamp/size + Copy (to clipboard) and Delete buttons;
// Delete re-triggers the rebuild). The Screenshot button's onClick is wired inline
// (independent of the rebuild). The CapFetcher struct stays inside the rebuild
// closure. Behaviour identical to the former inline block.
void buildCapturesPanel(ShellContext& ctx) {
    ctx.cap_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x93\xb8 Captures", 14.0f);
    ctx.cap_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.cap_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.cap_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.cap_col       = std::make_shared<FlexColumn>();
    ctx.cap_col->setGap(6);

    ctx.cap_screenshot_btn = std::make_shared<ButtonWidget>("\xf0\x9f\x93\xb8 Screenshot");
    ctx.cap_screenshot_btn->setFixedSize(110, 26);
    ctx.cap_screenshot_btn->setBackgroundColor({0.15f, 0.35f, 0.70f, 1.0f});
    ctx.cap_screenshot_btn->onClick([]() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.Captures",
            static_cast<uint32_t>(CaptureOpcode::TakeScreenshot), {}, nullptr);
    });

    ctx.rebuild_captures = std::make_shared<std::function<void()>>();
    *ctx.rebuild_captures = [cap_col = ctx.cap_col, cap_count_lbl = ctx.cap_count_lbl,
                         rc_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_captures)]() {
        if (!g_shell_xbus) return;
        cap_col->clearChildren();

        struct CapFetcher {
            std::shared_ptr<FlexColumn>                    col;
            std::shared_ptr<TextWidget>                    count_lbl;
            std::weak_ptr<std::function<void()>>           rebuild_weak;
            uint32_t                                       total_seen{0};
            std::shared_ptr<std::function<void(uint32_t)>> fetch;
        };
        auto cf = std::make_shared<CapFetcher>();
        cf->col          = cap_col;
        cf->count_lbl    = cap_count_lbl;
        cf->rebuild_weak = rc_weak;

        cf->fetch = std::make_shared<std::function<void(uint32_t)>>(
            [cf](uint32_t page) {
                if (!g_shell_xbus) return;
                uint32_t pg = page;
                g_shell_xbus->Call("com.freebsdx.Captures",
                    static_cast<uint32_t>(CaptureOpcode::ListCaptures),
                    {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
                    [cf, page](const XBusMessage& r) {
                        CapturePage cp{};
                        std::memcpy(&cp, r.payload_, sizeof(cp));
                        if (page == 0) {
                            cf->col->clearChildren();
                            cf->total_seen = 0;
                        }
                        cf->total_seen += cp.count_;

                        for (uint32_t i = 0; i < cp.count_; ++i) {
                            const CaptureEntry& e = cp.entries_[i];
                            std::string fname(e.filename_);
                            std::string ts(e.timestamp_);
                            bool is_recording = e.type_ == 1;
                            uint32_t size_k   = e.size_kb_;

                            auto row = std::make_shared<FlexColumn>();
                            row->setGap(3);

                            // Top row: type badge + filename
                            auto top = std::make_shared<FlexRow>();
                            top->setGap(6);
                            auto badge = std::make_shared<TextWidget>(
                                is_recording ? "REC" : "IMG", 10.0f);
                            badge->setColor(is_recording
                                ? Color{0.85f, 0.35f, 0.35f, 1.0f}
                                : Color{0.40f, 0.65f, 0.95f, 1.0f});
                            auto name_lbl = std::make_shared<TextWidget>(fname, 11.0f);
                            name_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});
                            top->addChild(badge);
                            top->addChild(name_lbl);

                            // Meta row: timestamp + size
                            auto meta = std::make_shared<FlexRow>();
                            meta->setGap(8);
                            auto ts_lbl = std::make_shared<TextWidget>(ts, 10.0f);
                            ts_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
                            auto sz_lbl = std::make_shared<TextWidget>(
                                std::format("{}KB", size_k), 10.0f);
                            sz_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
                            meta->addChild(ts_lbl);
                            meta->addChild(sz_lbl);
                            meta->addChild(std::make_shared<Spacer>(1, 1, true));

                            // Action buttons
                            auto copy_btn = std::make_shared<ButtonWidget>("Copy");
                            copy_btn->setFixedSize(46, 22);
                            copy_btn->setBackgroundColor({0.20f, 0.22f, 0.28f, 1.0f});
                            copy_btn->onClick([fname]() {
                                if (!g_shell_xbus) return;
                                Clipboard::setString(*g_shell_xbus, fname);
                            });

                            auto del_btn = std::make_shared<ButtonWidget>("Delete");
                            del_btn->setFixedSize(56, 22);
                            del_btn->setBackgroundColor({0.55f, 0.18f, 0.18f, 1.0f});
                            del_btn->onClick([fname, cf]() {
                                if (!g_shell_xbus) return;
                                CaptureDeleteRequest req{};
                                std::strncpy(req.filename_, fname.c_str(),
                                             sizeof(req.filename_) - 1);
                                g_shell_xbus->Call("com.freebsdx.Captures",
                                    static_cast<uint32_t>(CaptureOpcode::DeleteCapture),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    [cf](const XBusMessage&) {
                                        if (auto rb = cf->rebuild_weak.lock())
                                            if (*rb) (*rb)();
                                    });
                            });

                            meta->addChild(copy_btn);
                            meta->addChild(del_btn);

                            row->addChild(top);
                            row->addChild(meta);
                            cf->col->addChild(row);
                        }

                        uint32_t n = cf->total_seen;
                        cf->count_lbl->setText(n == 0 ? "No captures"
                            : std::format("{} capture{}", n, n != 1 ? "s" : ""));
                        cf->count_lbl->setColor(n == 0
                            ? Color{0.55f, 0.55f, 0.60f, 1.0f}
                            : Color{0.40f, 0.75f, 0.55f, 1.0f});

                        if (cp.has_more_ && cf->fetch)
                            (*cf->fetch)(page + 1);
                        else if (n == 0)
                            cf->col->addChild(
                                std::make_shared<TextWidget>("No screenshots or recordings.", 11.0f));
                    });
            });
        (*cf->fetch)(0);
    };
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



// Storage quick-view (Fetcher list). Header + result column; rebuild_storage uses
// a local PoolFetcher struct (col + fmtBytes/fmtBar helper closures + weak-self
// recursive fetch) to page com.freebsdx.Disk ListZpools (per-pool name/health +
// text usage bar + used/size/% info, warn-coloured at ≥85%). The PoolFetcher
// struct and its helper closures stay inside the rebuild closure. Behaviour
// identical to the former inline block.
void buildStoragePanel(ShellContext& ctx) {
    ctx.stor_hdr_lbl = std::make_shared<TextWidget>("💾 Storage", 14.0f);
    ctx.stor_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.stor_col = std::make_shared<FlexColumn>();
    ctx.stor_col->setGap(6);

    ctx.rebuild_storage = std::make_shared<std::function<void()>>();
    *ctx.rebuild_storage = [stor_col = ctx.stor_col,
                        rstor_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_storage)]() {
        if (!g_shell_xbus) return;
        stor_col->clearChildren();
        // Helper: format bytes as "X.X GB" / "X MB"
        auto fmtBytes = [](uint64_t b) -> std::string {
            if (b >= 1024ULL * 1024 * 1024)
                return std::format("{:.1f} GB", b / (1024.0 * 1024 * 1024));
            if (b >= 1024ULL * 1024)
                return std::format("{} MB", b / (1024 * 1024));
            return std::format("{} KB", b / 1024);
        };
        // Helper: text progress bar (width = 16 blocks, UTF-8 block chars)
        auto fmtBar = [](uint64_t used, uint64_t total) -> std::string {
            int filled = (total > 0) ? static_cast<int>(used * 16 / total) : 0;
            std::string s;
            for (int i = 0; i < 16; ++i) s += (i < filled) ? "▓" : "░";
            return s;
        };

        struct PoolFetcher {
            std::shared_ptr<FlexColumn>       col;
            std::function<std::string(uint64_t)>          fmtB;
            std::function<std::string(uint64_t,uint64_t)> fmtBr;
            std::function<void(uint32_t)>     fetch;
        };
        auto pf = std::make_shared<PoolFetcher>();
        pf->col  = stor_col;
        pf->fmtB = fmtBytes;
        pf->fmtBr = fmtBar;
        pf->fetch = [pf_weak = std::weak_ptr<PoolFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            g_shell_xbus->Call("com.freebsdx.Disk",
                static_cast<uint32_t>(DiskOpcode::ListZpools), {},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    ZpoolListPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No volumes found", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                        return;
                    }
                    if (pg.count_ > 0) {
                        const auto& pool = pg.pool_;
                        uint64_t used = pool.used_bytes_;
                        uint64_t size = pool.size_bytes_;
                        int pct = (size > 0)
                            ? static_cast<int>(used * 100 / size) : 0;
                        bool warn = pct >= 85;

                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(pool.name_) + "  " + std::string(pool.health_), 12.0f);
                        name_lbl->setColor(warn
                            ? Color{0.95f, 0.55f, 0.25f, 1.0f}
                            : Color{0.85f, 0.85f, 0.90f, 1.0f});

                        auto bar_lbl = std::make_shared<TextWidget>(
                            pf->fmtBr(used, size), 11.0f);
                        bar_lbl->setFontFamily("monospace");
                        bar_lbl->setColor(warn
                            ? Color{0.95f, 0.55f, 0.25f, 1.0f}
                            : Color{0.35f, 0.75f, 0.60f, 1.0f});

                        auto info_lbl = std::make_shared<TextWidget>(
                            std::format("{}  /  {}  ({}%)",
                                pf->fmtB(used), pf->fmtB(size), pct), 10.0f);
                        info_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        auto entry = std::make_shared<FlexColumn>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl, bar_lbl, info_lbl});
                        entry->setGap(2);
                        pf->col->addChild(entry);
                    }
                    if (pg.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };
}

// Disk I/O monitor (Fetcher list). Header + result column + Refresh button;
// rebuild_diskio uses a local DioFetcher struct (col + weak-self recursive fetch)
// to page com.freebsdx.Disk GetIoStats (per-disk name + read/write MB/s (colour by
// peak) + a mini utilisation bar). The Refresh button's onClick is wired after the
// rebuild body is assigned. The DioFetcher struct stays inside the rebuild closure.
// Behaviour identical to the former inline block.
void buildDiskIoPanel(ShellContext& ctx) {
    ctx.diskio_hdr_lbl = std::make_shared<TextWidget>("💿 Disk I/O", 14.0f);
    ctx.diskio_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.diskio_col = std::make_shared<FlexColumn>();
    ctx.diskio_col->setGap(6);

    ctx.rebuild_diskio = std::make_shared<std::function<void()>>();
    *ctx.rebuild_diskio = [diskio_col = ctx.diskio_col,
                       rdio_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_diskio)]() {
        if (!g_shell_xbus) return;
        diskio_col->clearChildren();

        struct DioFetcher {
            std::shared_ptr<FlexColumn> col;
            std::function<void(uint32_t)> fetch;
        };
        auto df = std::make_shared<DioFetcher>();
        df->col = diskio_col;
        df->fetch = [df_weak = std::weak_ptr<DioFetcher>(df)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.Disk",
                static_cast<uint32_t>(DiskOpcode::GetIoStats),
                {reinterpret_cast<const uint8_t*>(&pg_num),
                 reinterpret_cast<const uint8_t*>(&pg_num) + sizeof(pg_num)},
                [df_weak, page](const XBusMessage& resp) {
                    auto df = df_weak.lock();
                    if (!df) return;
                    DiskIoPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No disks found", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        df->col->addChild(empty);
                        return;
                    }
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        float r_mbs = e.read_kbps_  / 1024.0f;
                        float w_mbs = e.write_kbps_ / 1024.0f;
                        float peak  = std::max(r_mbs, w_mbs);

                        Color io_col;
                        if      (peak < 10.0f)  io_col = {0.30f, 0.85f, 0.50f, 1.0f}; // green
                        else if (peak < 100.0f) io_col = {0.95f, 0.75f, 0.20f, 1.0f}; // amber
                        else                    io_col = {0.95f, 0.35f, 0.30f, 1.0f}; // red

                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(e.name_), 12.0f);
                        name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                        auto io_lbl = std::make_shared<TextWidget>(
                            std::format("↓ {:.1f} MB/s  ↑ {:.1f} MB/s", r_mbs, w_mbs), 11.0f);
                        io_lbl->setColor(io_col);

                        // Mini utilisation bar: 10 blocks
                        int filled = std::min(static_cast<int>(e.util_pct_ / 10), 10);
                        std::string bar;
                        for (int b = 0; b < 10; ++b) bar += (b < filled) ? "▓" : "░";
                        auto util_lbl = std::make_shared<TextWidget>(
                            std::format("{}  {}%", bar, e.util_pct_), 10.0f);
                        util_lbl->setFontFamily("monospace");
                        util_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        auto entry = std::make_shared<FlexColumn>(
                            std::vector<std::shared_ptr<Widget>>{name_lbl, io_lbl, util_lbl});
                        entry->setGap(2);
                        df->col->addChild(entry);
                    }
                    if (pg.has_more_) {
                        if (auto p = df_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        df->fetch(0);
    };

    ctx.diskio_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh", [rebuild_diskio = ctx.rebuild_diskio]() {
        if (rebuild_diskio && *rebuild_diskio) (*rebuild_diskio)();
    });
    ctx.diskio_refresh_btn->setFixedSize(84, 26);
}

// SMART Disk Health (Fetcher list). Header + result column + Refresh button;
// rebuild_smart uses a local SmartFetcher struct (col + weak-self recursive fetch)
// to page com.freebsdx.Smart ListDrives (per-drive health badge + temperature +
// reallocated-sectors + power-on-days, all threshold-coloured). The Refresh
// button's onClick is wired after the rebuild body is assigned. The SmartFetcher
// struct stays inside the rebuild closure. Behaviour identical to the former
// inline block.
void buildSmartHealthPanel(ShellContext& ctx) {
    ctx.smart_hdr_lbl = std::make_shared<TextWidget>("🖴 Disk Health", 14.0f);
    ctx.smart_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.smart_col = std::make_shared<FlexColumn>();
    ctx.smart_col->setGap(6);

    ctx.rebuild_smart = std::make_shared<std::function<void()>>();
    *ctx.rebuild_smart = [smart_col = ctx.smart_col,
                      rsm_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_smart)]() {
        if (!g_shell_xbus) return;
        smart_col->clearChildren();

        struct SmartFetcher {
            std::shared_ptr<FlexColumn> col;
            std::function<void(uint32_t)> fetch;
        };
        auto sf = std::make_shared<SmartFetcher>();
        sf->col = smart_col;
        sf->fetch = [sf_weak = std::weak_ptr<SmartFetcher>(sf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.Smart",
                static_cast<uint32_t>(SmartOpcode::ListDrives),
                {reinterpret_cast<const uint8_t*>(&pg_num),
                 reinterpret_cast<const uint8_t*>(&pg_num) + sizeof(pg_num)},
                [sf_weak, page](const XBusMessage& resp) {
                    auto sf = sf_weak.lock();
                    if (!sf) return;
                    SmartPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No SMART drives", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        sf->col->addChild(empty);
                        return;
                    }
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& d = pg.entries_[i];
                        std::string health(d.health_);

                        // Health badge colour
                        std::string badge;
                        Color hcol;
                        if (health == "PASSED") {
                            badge = "🟢"; hcol = {0.30f, 0.85f, 0.50f, 1.0f};
                        } else if (health == "CAUTION") {
                            badge = "🟡"; hcol = {0.95f, 0.75f, 0.20f, 1.0f};
                        } else {
                            badge = "🔴"; hcol = {0.90f, 0.30f, 0.30f, 1.0f};
                        }

                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(d.name_) + "  " + badge, 12.0f);
                        name_lbl->setColor(hcol);

                        // Temperature
                        std::string temp_str = (d.temp_c_ == INT32_MIN)
                            ? "Temp: —"
                            : std::format("Temp: {}°C", d.temp_c_);
                        Color temp_col = (d.temp_c_ >= 55) ? Color{0.90f, 0.30f, 0.30f, 1.0f}
                                       : (d.temp_c_ >= 45) ? Color{0.95f, 0.70f, 0.20f, 1.0f}
                                       :                     Color{0.45f, 0.45f, 0.50f, 1.0f};
                        auto temp_lbl = std::make_shared<TextWidget>(temp_str, 10.0f);
                        temp_lbl->setColor(temp_col);

                        // Reallocated sectors
                        Color rs_col = d.reallocated_ > 0
                            ? Color{0.90f, 0.35f, 0.30f, 1.0f}
                            : Color{0.45f, 0.45f, 0.50f, 1.0f};
                        auto rs_lbl = std::make_shared<TextWidget>(
                            std::format("Reallocated: {}", d.reallocated_), 10.0f);
                        rs_lbl->setColor(rs_col);

                        // Power-on time
                        uint32_t days = d.power_on_hours_ / 24;
                        auto poh_lbl = std::make_shared<TextWidget>(
                            std::format("On: {} days", days), 10.0f);
                        poh_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        auto detail_row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                temp_lbl, std::make_shared<Spacer>(10, 1),
                                rs_lbl,   std::make_shared<Spacer>(10, 1),
                                poh_lbl});
                        auto entry = std::make_shared<FlexColumn>(
                            std::vector<std::shared_ptr<Widget>>{name_lbl, detail_row});
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

    ctx.smart_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh", [rebuild_smart = ctx.rebuild_smart]() {
        if (rebuild_smart && *rebuild_smart) (*rebuild_smart)();
    });
    ctx.smart_refresh_btn->setFixedSize(84, 26);
}

// Disk Usage (Fetcher list). Header + status label + result column + a Scan
// button; rebuild_disk_usage uses a local DuFetcher struct (col + status_lbl +
// weak-self recursive fetch) to page com.freebsdx.DiskUsage GetResults (per-dir
// path + human size + proportional bar; status shows "Loading…"/dir count). Scan
// posts a Scan request for "/"; the ScanComplete broadcast re-triggers the
// rebuild. The DuFetcher struct stays inside the rebuild closure. Behaviour
// identical to the former inline block.
void buildDiskUsagePanel(ShellContext& ctx) {
    ctx.du_hdr_lbl    = std::make_shared<TextWidget>("\xf0\x9f\x92\xbe Disk Usage", 14.0f);
    ctx.du_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.du_status_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.du_status_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});
    ctx.du_col        = std::make_shared<FlexColumn>();
    ctx.du_col->setGap(5);

    ctx.rebuild_disk_usage = std::make_shared<std::function<void()>>();
    *ctx.rebuild_disk_usage = [du_col = ctx.du_col, du_status_lbl = ctx.du_status_lbl,
                           rdu_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_disk_usage)]() {
        if (!g_shell_xbus) return;
        du_col->clearChildren();
        du_status_lbl->setText("Loading\xe2\x80\xa6");

        struct DuFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   status_lbl;
            std::function<void(uint32_t)> fetch;
        };
        auto df = std::make_shared<DuFetcher>();
        df->col        = du_col;
        df->status_lbl = du_status_lbl;
        df->fetch = [df_weak = std::weak_ptr<DuFetcher>(df)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.DiskUsage",
                static_cast<uint32_t>(DiskUsageOpcode::GetResults),
                {reinterpret_cast<const uint8_t*>(&pg_num), sizeof(pg_num)},
                [df_weak, page](const XBusMessage& resp) {
                    auto df = df_weak.lock();
                    if (!df) return;
                    DiskUsagePage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>(
                            "No data \xe2\x80\x94 press \xe2\x86\xba Scan", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        df->col->addChild(empty);
                        df->status_lbl->setText("");
                        return;
                    }
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        std::string size_str;
                        if (e.size_bytes_ >= 1073741824ULL)
                            size_str = std::format("{:.1f} GB",
                                static_cast<double>(e.size_bytes_) / 1073741824.0);
                        else if (e.size_bytes_ >= 1048576ULL)
                            size_str = std::format("{:.1f} MB",
                                static_cast<double>(e.size_bytes_) / 1048576.0);
                        else
                            size_str = std::format("{} KB", e.size_bytes_ / 1024ULL);

                        // Proportional bar (10 cells, █ filled / ░ empty)
                        uint32_t bars = std::min(e.pct_of_total_, 100u) / 10;
                        std::string bar;
                        for (uint32_t b = 0; b < bars;      ++b) bar += "\xe2\x96\x88";
                        for (uint32_t b = bars; b < 10u; ++b) bar += "\xe2\x96\x91";

                        auto path_lbl = std::make_shared<TextWidget>(
                            std::string(e.path_), 11.0f);
                        path_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});

                        auto size_lbl = std::make_shared<TextWidget>(
                            size_str + "  " + bar, 10.0f);
                        size_lbl->setColor({0.45f, 0.75f, 0.95f, 1.0f});

                        auto entry = std::make_shared<FlexColumn>(
                            std::vector<std::shared_ptr<Widget>>{path_lbl, size_lbl});
                        entry->setGap(2);
                        df->col->addChild(entry);
                    }
                    if (pg.has_more_) {
                        if (auto p = df_weak.lock()) p->fetch(page + 1);
                    } else {
                        df->status_lbl->setText(
                            std::format("{} directories", pg.total_));
                    }
                });
        };
        df->fetch(0);
    };

    ctx.du_scan_btn = std::make_shared<ButtonWidget>("\xe2\x86\xba Scan", [rebuild_disk_usage = ctx.rebuild_disk_usage]() {
        if (!g_shell_xbus) return;
        DiskUsageScanRequest req{};
        std::strncpy(req.path_, "/", sizeof(req.path_) - 1);
        g_shell_xbus->Call("com.freebsdx.DiskUsage",
            static_cast<uint32_t>(DiskUsageOpcode::Scan),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            nullptr);
        // ScanComplete broadcast triggers rebuild_disk_usage
    });
    ctx.du_scan_btn->setFixedSize(70, 26);
}

// Archive. Header + status label + an Extract form row (archive+dest inputs +
// Extract button) + a Create form row (source+output inputs + Create button).
// Extract/Create post ArchiveExtractRequest/ArchiveCreateRequest to
// com.freebsdx.Archive and set the status label; completion is reported via
// broadcast/reply handlers elsewhere. The form inputs + buttons stay builder-local
// (held by the escaping extract/create rows). Behaviour identical to the former
// inline block.
void buildArchivePanel(ShellContext& ctx) {
    ctx.arch_hdr_lbl    = std::make_shared<TextWidget>("\xf0\x9f\x97\x9c Archive", 14.0f);
    ctx.arch_status_lbl = std::make_shared<TextWidget>("Idle", 11.0f);
    ctx.arch_status_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

    // Extract row
    auto arch_src_input  = std::make_shared<TextInputWidget>();
    arch_src_input->setPlaceholder("Archive path\xe2\x80\xa6");
    arch_src_input->setFlexGrow(1.0f);
    auto arch_dest_input = std::make_shared<TextInputWidget>();
    arch_dest_input->setPlaceholder("Destination\xe2\x80\xa6");
    arch_dest_input->setFlexGrow(1.0f);
    auto arch_extract_btn = std::make_shared<ButtonWidget>("Extract",
        [arch_src_input, arch_dest_input, arch_status_lbl = ctx.arch_status_lbl]() {
            ArchiveExtractRequest req{};
            std::strncpy(req.archive_path_, arch_src_input->getText().c_str(),
                         sizeof(req.archive_path_) - 1);
            std::strncpy(req.dest_path_, arch_dest_input->getText().c_str(),
                         sizeof(req.dest_path_) - 1);
            arch_status_lbl->setText("Extracting\xe2\x80\xa6");
            arch_status_lbl->setColor({0.65f, 0.65f, 0.35f, 1.0f});
            std::vector<uint8_t> buf(sizeof(req));
            std::memcpy(buf.data(), &req, sizeof(req));
            g_shell_xbus->Call("com.freebsdx.Archive",
                static_cast<uint32_t>(ArchiveOpcode::Extract), buf, nullptr);
        });
    arch_extract_btn->setFixedSize(70, 28);
    arch_extract_btn->setBackgroundColor({0.20f, 0.35f, 0.55f, 1.0f});
    ctx.arch_extract_row = std::make_shared<FlexRow>(
        std::vector<std::shared_ptr<Widget>>{
            arch_src_input, std::make_shared<Spacer>(6, 1),
            arch_dest_input, std::make_shared<Spacer>(6, 1),
            arch_extract_btn});
    ctx.arch_extract_row->setAlignment(Alignment::Center);

    // Create row
    auto arch_csrc_input  = std::make_shared<TextInputWidget>();
    arch_csrc_input->setPlaceholder("Source path\xe2\x80\xa6");
    arch_csrc_input->setFlexGrow(1.0f);
    auto arch_cout_input  = std::make_shared<TextInputWidget>();
    arch_cout_input->setPlaceholder("Output archive\xe2\x80\xa6");
    arch_cout_input->setFlexGrow(1.0f);
    auto arch_create_btn  = std::make_shared<ButtonWidget>("Create",
        [arch_csrc_input, arch_cout_input, arch_status_lbl = ctx.arch_status_lbl]() {
            ArchiveCreateRequest req{};
            std::strncpy(req.source_path_, arch_csrc_input->getText().c_str(),
                         sizeof(req.source_path_) - 1);
            std::strncpy(req.archive_path_, arch_cout_input->getText().c_str(),
                         sizeof(req.archive_path_) - 1);
            arch_status_lbl->setText("Creating\xe2\x80\xa6");
            arch_status_lbl->setColor({0.65f, 0.65f, 0.35f, 1.0f});
            std::vector<uint8_t> buf(sizeof(req));
            std::memcpy(buf.data(), &req, sizeof(req));
            g_shell_xbus->Call("com.freebsdx.Archive",
                static_cast<uint32_t>(ArchiveOpcode::Create), buf, nullptr);
        });
    arch_create_btn->setFixedSize(70, 28);
    arch_create_btn->setBackgroundColor({0.30f, 0.45f, 0.25f, 1.0f});
    ctx.arch_create_row  = std::make_shared<FlexRow>(
        std::vector<std::shared_ptr<Widget>>{
            arch_csrc_input, std::make_shared<Spacer>(6, 1),
            arch_cout_input, std::make_shared<Spacer>(6, 1),
            arch_create_btn});
    ctx.arch_create_row->setAlignment(Alignment::Center);
}

// Print Queue (Fetcher list). Header + result column + Refresh button;
// rebuild_print uses a local PrintFetcher struct (col + weak-self recursive fetch)
// to page com.freebsdx.Print ListPrinters (per-printer name (+★ default) + state
// label + location/description). The pagination guard uses `page_ + 1 < total_`.
// The Refresh button's onClick is wired after the rebuild body is assigned. The
// PrintFetcher struct stays inside the rebuild closure. Behaviour identical to the
// former inline block.
void buildPrintSpoolerPanel(ShellContext& ctx) {
    ctx.print_hdr_lbl_b = std::make_shared<TextWidget>("🖨 Printers", 14.0f);
    ctx.print_hdr_lbl_b->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.print_col_b = std::make_shared<FlexColumn>();
    ctx.print_col_b->setGap(6);

    ctx.rebuild_print = std::make_shared<std::function<void()>>();
    *ctx.rebuild_print = [print_col_b = ctx.print_col_b,
                      rp_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_print)]() {
        if (!g_shell_xbus) return;
        print_col_b->clearChildren();

        struct PrintFetcher {
            std::shared_ptr<FlexColumn> col;
            std::function<void(uint32_t)> fetch;
        };
        auto pf = std::make_shared<PrintFetcher>();
        pf->col = print_col_b;
        pf->fetch = [pf_weak = std::weak_ptr<PrintFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_idx = page;
            g_shell_xbus->Call("com.freebsdx.Print",
                static_cast<uint32_t>(PrintSpoolerOpcode::ListPrinters),
                {reinterpret_cast<const uint8_t*>(&pg_idx), sizeof(pg_idx)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    PrinterListPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No printers configured", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                        return;
                    }
                    if (pg.count_ > 0) {
                        const auto& pr = pg.printer_;
                        static constexpr const char* kStateStr[] = {"Idle", "Printing…", "Stopped"};
                        static const Color kStateCol[] = {
                            {0.4f, 0.80f, 0.50f, 1.0f},
                            {0.92f, 0.72f, 0.20f, 1.0f},
                            {0.80f, 0.30f, 0.30f, 1.0f},
                        };
                        uint32_t s = pr.state_ < 3u ? pr.state_ : 0u;

                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(pr.name_) + (pr.is_default_ ? "  ★" : ""), 12.0f);
                        name_lbl->setColor({0.88f, 0.88f, 0.92f, 1.0f});

                        auto state_lbl = std::make_shared<TextWidget>(kStateStr[s], 11.0f);
                        state_lbl->setColor(kStateCol[s]);

                        const char* sub = pr.location_[0] ? pr.location_ : pr.description_;
                        auto sub_lbl = std::make_shared<TextWidget>(std::string(sub), 10.0f);
                        sub_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        auto hdr_row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl, std::make_shared<Spacer>(8, 1), state_lbl});

                        auto entry = std::make_shared<FlexColumn>(
                            std::vector<std::shared_ptr<Widget>>{hdr_row, sub_lbl});
                        entry->setGap(2);
                        pf->col->addChild(entry);
                    }
                    if (pg.page_ + 1 < pg.total_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };

    ctx.print_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh", [rebuild_print = ctx.rebuild_print]() {
        if (rebuild_print && *rebuild_print) (*rebuild_print)();
    });
    ctx.print_refresh_btn->setFixedSize(84, 26);
}

// USB Device Monitor (paginated Fetcher list, LATENT). rebuild_usb_b uses a local
// UsbFetcher struct to page com.freebsdx.Usb ListDevices (per-device type icon +
// name, with capacity + Eject for storage devices, and a mount line). Only
// rebuild_usb_b escapes (broadcast/timer); the header, result column and Refresh
// button are never added to a card, so they stay builder-local (the column is kept
// alive by the rebuild closure). Behaviour identical to the former inline block.
void buildUsbMonitorPanel(ShellContext& ctx) {
    auto usb_hdr_lbl_b = std::make_shared<TextWidget>("🔌 USB Devices", 14.0f);
    usb_hdr_lbl_b->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto usb_col_b = std::make_shared<FlexColumn>();
    usb_col_b->setGap(6);

    ctx.rebuild_usb_b = std::make_shared<std::function<void()>>();
    *ctx.rebuild_usb_b = [usb_col_b,
                    ru_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_usb_b)]() {
        if (!g_shell_xbus) return;
        usb_col_b->clearChildren();

        struct UsbFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::function<void(uint32_t)> fetch;
        };
        auto pf = std::make_shared<UsbFetcher>();
        pf->col = usb_col_b;
        pf->fetch = [pf_weak = std::weak_ptr<UsbFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_idx = page;
            g_shell_xbus->Call("com.freebsdx.Usb",
                static_cast<uint32_t>(UsbOpcode::ListDevices),
                {reinterpret_cast<const uint8_t*>(&pg_idx), sizeof(pg_idx)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    UsbDevicePage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No USB devices", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                        return;
                    }
                    if (pg.count_ > 0) {
                        const auto& dev = pg.device_;
                        const char* icon =
                            std::strcmp(dev.type_, "storage") == 0 ? "💾 " :
                            std::strcmp(dev.type_, "hid")     == 0 ? "🖱 " :
                            std::strcmp(dev.type_, "audio")   == 0 ? "🔊 " :
                            std::strcmp(dev.type_, "hub")     == 0 ? "🔗 " : "🔌 ";
                        std::string label = icon + std::string(dev.name_[0] ? dev.name_ : "Unknown Device");

                        auto name_lbl = std::make_shared<TextWidget>(label, 12.0f);
                        name_lbl->setColor({0.88f, 0.88f, 0.92f, 1.0f});

                        std::vector<std::shared_ptr<Widget>> row_ws{
                            name_lbl, std::make_shared<Spacer>(1, 1, true)};

                        bool is_storage = std::strcmp(dev.type_, "storage") == 0;
                        if (is_storage) {
                            std::string cap_s;
                            if (dev.capacity_mb_ >= 1024)
                                cap_s = std::format("{:.0f} GB", dev.capacity_mb_ / 1024.0f);
                            else if (dev.capacity_mb_ > 0)
                                cap_s = std::format("{} MB", dev.capacity_mb_);
                            auto cap_lbl = std::make_shared<TextWidget>(cap_s, 10.0f);
                            cap_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                            uint32_t addr = dev.addr_;
                            auto eject_btn = std::make_shared<ButtonWidget>("⏏ Eject",
                                [addr]() {
                                    if (!g_shell_xbus) return;
                                    UsbEjectRequest req{};
                                    req.addr_ = addr;
                                    g_shell_xbus->Call("com.freebsdx.Usb",
                                        static_cast<uint32_t>(UsbOpcode::Eject),
                                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                        nullptr);
                                });
                            eject_btn->setFixedSize(70, 26);
                            eject_btn->setBackgroundColor({0.35f, 0.15f, 0.15f, 1.0f});
                            row_ws.push_back(cap_lbl);
                            row_ws.push_back(std::make_shared<Spacer>(4, 1));
                            row_ws.push_back(eject_btn);
                        }

                        auto entry_row = std::make_shared<FlexRow>(row_ws);
                        entry_row->setGap(6);
                        entry_row->setAlignment(Alignment::Center);

                        if (is_storage && dev.mount_[0]) {
                            auto mount_lbl = std::make_shared<TextWidget>(
                                std::string("  ") + dev.mount_, 10.0f);
                            mount_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});
                            auto entry = std::make_shared<FlexColumn>(
                                std::vector<std::shared_ptr<Widget>>{entry_row, mount_lbl});
                            entry->setGap(2);
                            pf->col->addChild(entry);
                        } else {
                            pf->col->addChild(entry_row);
                        }
                    }
                    if (pg.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };

    auto usb_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh", [rebuild_usb_b = ctx.rebuild_usb_b]() {
        if (rebuild_usb_b && *rebuild_usb_b) (*rebuild_usb_b)();
    });
    usb_refresh_btn->setFixedSize(84, 26);
}

// Bookmarks (Fetcher list). Header row (label + count + Clear All) + result
// column; rebuild_bm uses a local BmFetcher struct (col + count + weak-self
// recursive fetch + total) to page com.freebsdx.Bookmarks List (per-bookmark
// title + truncated URL + a Remove button). Clear All wipes the service and
// resets the column/count. The BmFetcher struct stays inside the rebuild closure.
// Behaviour identical to the former inline block.
void buildBookmarksPanel(ShellContext& ctx) {
    ctx.bm_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x94\x96 Bookmarks", 14.0f);
    ctx.bm_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.bm_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.bm_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.bm_col = std::make_shared<FlexColumn>();
    ctx.bm_col->setGap(4);

    ctx.bm_clear_btn = std::make_shared<ButtonWidget>("\xf0\x9f\x97\x91 Clear All",
        [bm_col = ctx.bm_col, bm_count_lbl = ctx.bm_count_lbl]() {
            if (!g_shell_xbus) return;
            g_shell_xbus->Call("com.freebsdx.Bookmarks",
                static_cast<uint32_t>(BookmarkOpcode::Clear), {}, nullptr);
            bm_col->clearChildren();
            bm_count_lbl->setText("0");
        });
    ctx.bm_clear_btn->setFixedSize(90, 26);
    ctx.bm_clear_btn->setBackgroundColor({0.40f, 0.18f, 0.18f, 1.0f});

    ctx.rebuild_bm = std::make_shared<std::function<void()>>();
    *ctx.rebuild_bm = [bm_col = ctx.bm_col, bm_count_lbl = ctx.bm_count_lbl,
                   rbm_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_bm)]() {
        if (!g_shell_xbus) return;
        bm_col->clearChildren();

        struct BmFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<BmFetcher>();
        pf->col   = bm_col;
        pf->count = bm_count_lbl;
        pf->fetch = [pf_weak = std::weak_ptr<BmFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.Bookmarks",
                static_cast<uint32_t>(BookmarkOpcode::List),
                {reinterpret_cast<const uint8_t*>(&pg_num),
                 reinterpret_cast<const uint8_t*>(&pg_num) + sizeof(pg_num)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    BookmarkPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0) pf->total = pg.total_;
                    if (page == 0 && pg.total_ == 0) {
                        pf->count->setText("No bookmarks");
                        auto empty = std::make_shared<TextWidget>(
                            "No bookmarks saved", 11.0f);
                        empty->setColor({0.45f, 0.45f, 0.50f, 1.0f});
                        pf->col->addChild(empty);
                        return;
                    }
                    if (page == 0)
                        pf->count->setText(std::format("{}", pf->total));
                    if (pg.valid_) {
                        auto title_lbl = std::make_shared<TextWidget>(
                            std::string(pg.title_), 11.0f);
                        title_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});
                        title_lbl->setFlexGrow(1.0f);

                        // Truncate URL for display
                        std::string url_str = std::string(pg.url_);
                        std::string url_disp = url_str.size() > 36
                            ? url_str.substr(0, 33) + "\xe2\x80\xa6"
                            : url_str;
                        auto url_lbl = std::make_shared<TextWidget>(url_disp, 10.0f);
                        url_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        std::string url_key = url_str;
                        auto rm_btn = std::make_shared<ButtonWidget>("\xe2\x9c\x95",
                            [url_key]() {
                                if (!g_shell_xbus) return;
                                BookmarkRequest req{};
                                std::strncpy(req.url_, url_key.c_str(),
                                             sizeof(req.url_) - 1);
                                g_shell_xbus->Call("com.freebsdx.Bookmarks",
                                    static_cast<uint32_t>(BookmarkOpcode::Remove),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    nullptr);
                            });
                        rm_btn->setFixedSize(26, 22);
                        rm_btn->setBackgroundColor({0.40f, 0.18f, 0.18f, 1.0f});

                        auto entry_col = std::make_shared<FlexColumn>(
                            std::vector<std::shared_ptr<Widget>>{title_lbl, url_lbl});
                        entry_col->setGap(1);
                        entry_col->setFlexGrow(1.0f);

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                entry_col,
                                std::make_shared<Spacer>(6, 1), rm_btn});
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

// Active Transfers (outer-scope-fetcher list). Header + count label + result
// column; unlike the in-rebuild fetchers, the XfrFetcher struct + xfrf shared_ptr
// live at builder scope and are captured by rebuild_xfr (so they stay alive via
// the closure). fetch pages com.freebsdx.Transfer ListActive (per-transfer
// direction icon + filename + %/speed/size + a ProgressBarWidget + Cancel button
// that re-triggers the rebuild). Behaviour identical to the former inline block.
void buildActiveTransfersPanel(ShellContext& ctx) {
    ctx.xfr_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x93\xa4 Active Transfers", 14.0f);
    ctx.xfr_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.xfr_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.xfr_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.xfr_col       = std::make_shared<FlexColumn>();
    ctx.xfr_col->setGap(8);

    ctx.rebuild_xfr = std::make_shared<std::function<void()>>();
    struct XfrFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto xfrf = std::make_shared<XfrFetcher>();
    auto xfrf_weak = std::weak_ptr<XfrFetcher>(xfrf);

    xfrf->col       = ctx.xfr_col;
    xfrf->count_lbl = ctx.xfr_count_lbl;
    xfrf->rw        = ctx.rebuild_xfr;
    xfrf->fetch = [xfrf_weak](uint32_t page) {
        auto xfrf = xfrf_weak.lock();
        if (!xfrf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.Transfer",
            static_cast<uint32_t>(TransferOpcode::ListActive),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [xfrf_weak, page](const XBusMessage& r) {
                auto xfrf = xfrf_weak.lock();
                if (!xfrf) return;
                TransferPage tp{};
                std::memcpy(&tp, r.payload_, sizeof(tp));
                if (!tp.valid_) return;
                if (page == 0) xfrf->col->clearChildren();

                // Progress 0.0–1.0
                float pct = (tp.bytes_total_ > 0)
                    ? static_cast<float>(tp.bytes_done_) / static_cast<float>(tp.bytes_total_)
                    : 0.0f;
                pct = std::clamp(pct, 0.0f, 1.0f);

                // Format sizes
                auto fmt_bytes = [](uint64_t b) -> std::string {
                    if (b >= 1024*1024*1024ull)
                        return std::format("{:.1f} GB", b / (1024.0*1024*1024));
                    if (b >= 1024*1024)
                        return std::format("{:.1f} MB", b / (1024.0*1024));
                    if (b >= 1024)
                        return std::format("{:.1f} KB", b / 1024.0);
                    return std::format("{} B", b);
                };

                // Direction icon
                const char* dir_icon = tp.direction_ ? "\xe2\x86\x91" : "\xe2\x86\x93"; // ↑ / ↓

                auto name_lbl = std::make_shared<TextWidget>(
                    std::string(dir_icon) + " " + (tp.filename_[0] ? tp.filename_ : "Unknown"),
                    11.0f);
                name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                auto pct_lbl = std::make_shared<TextWidget>(
                    std::format("{:.0f}%", pct * 100.0f), 10.0f);
                pct_lbl->setColor({0.60f, 0.75f, 0.90f, 1.0f});

                auto speed_lbl = std::make_shared<TextWidget>(
                    std::format("{} KB/s", tp.speed_kbps_), 10.0f);
                speed_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                auto size_lbl = std::make_shared<TextWidget>(
                    fmt_bytes(tp.bytes_done_) + " / " + fmt_bytes(tp.bytes_total_), 10.0f);
                size_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                // Progress bar: xkit ProgressBarWidget takes a 0..1 fraction
                auto bar = std::make_shared<ProgressBarWidget>(pct);
                bar->setFixedSize(200, 6);

                auto id_str = std::make_shared<std::string>(tp.id_);
                auto cancel_btn = std::make_shared<ButtonWidget>("Cancel");
                cancel_btn->setFixedSize(60, 22);
                cancel_btn->setBackgroundColor({0.55f, 0.20f, 0.20f, 0.90f});
                cancel_btn->onClick([id_str, xfrf_weak]() {
                    if (!g_shell_xbus) return;
                    TransferActionRequest req{};
                    std::strncpy(req.id_, id_str->c_str(), sizeof(req.id_)-1);
                    g_shell_xbus->Call("com.freebsdx.Transfer",
                        static_cast<uint32_t>(TransferOpcode::CancelTransfer),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [xfrf_weak](const XBusMessage&) {
                            if (auto xfrf = xfrf_weak.lock(); xfrf && xfrf->rw.lock())
                                (*xfrf->rw.lock())();
                        });
                });

                auto name_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    name_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    pct_lbl,
                });
                auto bar_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    bar,
                });
                auto info_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    size_lbl,
                    std::make_shared<Spacer>(8, 1),
                    speed_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    cancel_btn,
                });
                auto entry = std::make_shared<FlexColumn>();
                entry->addChild(name_row);
                entry->addChild(std::make_shared<Spacer>(1, 3));
                entry->addChild(bar_row);
                entry->addChild(std::make_shared<Spacer>(1, 3));
                entry->addChild(info_row);
                xfrf->col->addChild(entry);

                if (xfrf->count_lbl)
                    xfrf->count_lbl->setText(std::format("{}", tp.total_));
                if (tp.has_more_) xfrf->fetch(page + 1);
            });
    };

    *ctx.rebuild_xfr = [xfr_col = ctx.xfr_col, xfrf]() {
        xfr_col->clearChildren();
        if (xfrf->fetch) xfrf->fetch(0);
    };
}

// Game Controllers (outer-scope-fetcher list). Header + count label + result
// column; the GcFetcher struct + gcf shared_ptr live at builder scope and are
// captured by rebuild_gc. fetch pages com.freebsdx.GameController ListControllers
// (per-controller BT/USB icon + name + battery + Calibrate/Forget buttons; Forget
// re-triggers the rebuild). Behaviour identical to the former inline block.
void buildGameControllersPanel(ShellContext& ctx) {
    ctx.gc_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x8e\xae Game Controllers", 14.0f);
    ctx.gc_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.gc_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.gc_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.gc_col       = std::make_shared<FlexColumn>();
    ctx.gc_col->setGap(8);

    ctx.rebuild_gc = std::make_shared<std::function<void()>>();
    struct GcFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto gcf = std::make_shared<GcFetcher>();
    auto gcf_weak = std::weak_ptr<GcFetcher>(gcf);

    gcf->col       = ctx.gc_col;
    gcf->count_lbl = ctx.gc_count_lbl;
    gcf->rw        = ctx.rebuild_gc;
    gcf->fetch = [gcf_weak](uint32_t page) {
        auto gcf = gcf_weak.lock();
        if (!gcf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.GameController",
            static_cast<uint32_t>(GameControllerOpcode::ListControllers),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [gcf_weak, page](const XBusMessage& r) {
                auto gcf = gcf_weak.lock();
                if (!gcf) return;
                GameControllerPage gp{};
                std::memcpy(&gp, r.payload_, sizeof(gp));
                if (!gp.valid_) return;
                if (page == 0) gcf->col->clearChildren();

                const char* conn_icon = gp.connection_ ? "\xf0\x9f\x93\xb6" : "\xf0\x9f\x94\x8c"; // BT / USB
                auto name_lbl = std::make_shared<TextWidget>(
                    std::string(conn_icon) + " " + (gp.name_[0] ? gp.name_ : "Unknown Controller"),
                    11.0f);
                name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                auto batt_lbl = std::make_shared<TextWidget>(
                    gp.battery_pct_ <= 100 ? std::format("{}% battery", gp.battery_pct_)
                                            : std::string("Wired"),
                    10.0f);
                batt_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                uint32_t id_val = gp.controller_id_;
                auto calib_btn = std::make_shared<ButtonWidget>("Calibrate");
                calib_btn->setFixedSize(70, 22);
                calib_btn->onClick([id_val]() {
                    if (!g_shell_xbus) return;
                    GameControllerRequest req{};
                    req.controller_id_ = id_val;
                    g_shell_xbus->Call("com.freebsdx.GameController",
                        static_cast<uint32_t>(GameControllerOpcode::CalibrateController),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                });

                auto forget_btn = std::make_shared<ButtonWidget>("Forget");
                forget_btn->setFixedSize(60, 22);
                forget_btn->setBackgroundColor({0.55f, 0.20f, 0.20f, 0.90f});
                forget_btn->onClick([id_val, gcf_weak]() {
                    if (!g_shell_xbus) return;
                    GameControllerRequest req{};
                    req.controller_id_ = id_val;
                    g_shell_xbus->Call("com.freebsdx.GameController",
                        static_cast<uint32_t>(GameControllerOpcode::ForgetController),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [gcf_weak](const XBusMessage&) {
                            if (auto gcf = gcf_weak.lock(); gcf && gcf->rw.lock())
                                (*gcf->rw.lock())();
                        });
                });

                auto name_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    name_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    batt_lbl,
                });
                auto btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    std::make_shared<Spacer>(1, 1, true),
                    calib_btn,
                    std::make_shared<Spacer>(4, 1),
                    forget_btn,
                });
                auto entry = std::make_shared<FlexColumn>();
                entry->addChild(name_row);
                entry->addChild(std::make_shared<Spacer>(1, 3));
                entry->addChild(btn_row);
                gcf->col->addChild(entry);

                if (gcf->count_lbl)
                    gcf->count_lbl->setText(std::format("{}", gp.total_));
                if (gp.has_more_) gcf->fetch(page + 1);
            });
    };

    *ctx.rebuild_gc = [gc_col = ctx.gc_col, gcf]() {
        gc_col->clearChildren();
        if (gcf->fetch) gcf->fetch(0);
    };
}

// Saved Wi-Fi Networks (outer-scope-fetcher list). Header + count label + result
// column; the SwifiFetcher struct + swf shared_ptr live at builder scope and are
// captured by rebuild_swifi. fetch pages com.freebsdx.SavedWifi ListSaved
// (per-network SSID + security label + auto-connect switch + Forget button;
// Forget re-triggers the rebuild). Behaviour identical to the former inline block.
void buildSavedWifiPanel(ShellContext& ctx) {
    ctx.swifi_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x93\xb6 Saved Wi-Fi Networks", 14.0f);
    ctx.swifi_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.swifi_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.swifi_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.swifi_col       = std::make_shared<FlexColumn>();
    ctx.swifi_col->setGap(8);

    ctx.rebuild_swifi = std::make_shared<std::function<void()>>();
    struct SwifiFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto swf = std::make_shared<SwifiFetcher>();
    auto swf_weak = std::weak_ptr<SwifiFetcher>(swf);

    swf->col       = ctx.swifi_col;
    swf->count_lbl = ctx.swifi_count_lbl;
    swf->rw        = ctx.rebuild_swifi;
    swf->fetch = [swf_weak](uint32_t page) {
        auto swf = swf_weak.lock();
        if (!swf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.SavedWifi",
            static_cast<uint32_t>(SavedWifiOpcode::ListSaved),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [swf_weak, page](const XBusMessage& r) {
                auto swf = swf_weak.lock();
                if (!swf) return;
                SavedWifiPage sp{};
                std::memcpy(&sp, r.payload_, sizeof(sp));
                if (!sp.valid_) return;
                if (page == 0) swf->col->clearChildren();

                static const char* kSecLabels[] = {"Open", "WPA2", "WPA3"};
                const char* sec = sp.security_type_ < 3 ? kSecLabels[sp.security_type_] : "Open";

                auto ssid_lbl = std::make_shared<TextWidget>(
                    std::string("\xf0\x9f\x93\xb6 ") + (sp.ssid_[0] ? sp.ssid_ : "Unknown"), 11.0f);
                ssid_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                auto sec_lbl = std::make_shared<TextWidget>(sec, 10.0f);
                sec_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                auto auto_sw = std::make_shared<SwitchWidget>(sp.auto_connect_ != 0);
                auto ssid_val = std::make_shared<std::string>(sp.ssid_);
                auto_sw->setOnChanged([ssid_val](bool on) {
                    if (!g_shell_xbus) return;
                    SavedWifiAutoConnectRequest req{};
                    std::strncpy(req.ssid_, ssid_val->c_str(), sizeof(req.ssid_) - 1);
                    req.auto_connect_ = on ? 1u : 0u;
                    g_shell_xbus->Call("com.freebsdx.SavedWifi",
                        static_cast<uint32_t>(SavedWifiOpcode::SetAutoConnect),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                });

                auto forget_btn = std::make_shared<ButtonWidget>("Forget");
                forget_btn->setFixedSize(60, 22);
                forget_btn->setBackgroundColor({0.55f, 0.20f, 0.20f, 0.90f});
                forget_btn->onClick([ssid_val, swf_weak]() {
                    if (!g_shell_xbus) return;
                    SavedWifiForgetRequest req{};
                    std::strncpy(req.ssid_, ssid_val->c_str(), sizeof(req.ssid_) - 1);
                    g_shell_xbus->Call("com.freebsdx.SavedWifi",
                        static_cast<uint32_t>(SavedWifiOpcode::ForgetNetwork),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [swf_weak](const XBusMessage&) {
                            if (auto swf = swf_weak.lock(); swf && swf->rw.lock())
                                (*swf->rw.lock())();
                        });
                });

                auto name_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    ssid_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    sec_lbl,
                });
                auto ctrl_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    std::make_shared<TextWidget>("Auto-connect", 10.0f),
                    std::make_shared<Spacer>(6, 1),
                    auto_sw,
                    std::make_shared<Spacer>(1, 1, true),
                    forget_btn,
                });
                auto entry = std::make_shared<FlexColumn>();
                entry->addChild(name_row);
                entry->addChild(std::make_shared<Spacer>(1, 3));
                entry->addChild(ctrl_row);
                swf->col->addChild(entry);

                if (swf->count_lbl)
                    swf->count_lbl->setText(std::format("{}", sp.total_));
                if (sp.has_more_) swf->fetch(page + 1);
            });
    };

    *ctx.rebuild_swifi = [swifi_col = ctx.swifi_col, swf]() {
        swifi_col->clearChildren();
        if (swf->fetch) swf->fetch(0);
    };
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

// Text Replacement (outer-scope-fetcher list + add form). Header + count label +
// result column + an add form (shortcut + phrase inputs + status label + Add
// button); the TxtrepFetcher struct + trf shared_ptr live at builder scope and are
// captured by rebuild_txtrep. fetch pages com.freebsdx.TextReplacement
// ListShortcuts (per-entry "shortcut → phrase" + Delete). Add posts AddShortcut,
// clears the inputs, and refreshes. The add-form widgets are added to the AC card
// so they escape to ctx. Behaviour identical to the former inline block.
void buildTextReplacementPanel(ShellContext& ctx) {
    ctx.txtrep_hdr_lbl   = std::make_shared<TextWidget>("\xe2\x8c\xa8 Text Replacement", 14.0f);
    ctx.txtrep_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.txtrep_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.txtrep_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.txtrep_col       = std::make_shared<FlexColumn>();
    ctx.txtrep_col->setGap(8);

    ctx.rebuild_txtrep = std::make_shared<std::function<void()>>();
    struct TxtrepFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto trf = std::make_shared<TxtrepFetcher>();
    auto trf_weak = std::weak_ptr<TxtrepFetcher>(trf);

    trf->col       = ctx.txtrep_col;
    trf->count_lbl = ctx.txtrep_count_lbl;
    trf->rw        = ctx.rebuild_txtrep;
    trf->fetch = [trf_weak](uint32_t page) {
        auto trf = trf_weak.lock();
        if (!trf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.TextReplacement",
            static_cast<uint32_t>(TextReplacementOpcode::ListShortcuts),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [trf_weak, page](const XBusMessage& r) {
                auto trf = trf_weak.lock();
                if (!trf) return;
                TextReplacementPage tp{};
                std::memcpy(&tp, r.payload_, sizeof(tp));
                if (!tp.valid_) return;
                if (page == 0) trf->col->clearChildren();

                auto shortcut_lbl = std::make_shared<TextWidget>(
                    std::string(tp.shortcut_) + " \xe2\x86\x92 " + tp.phrase_, 11.0f);
                shortcut_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                uint32_t id_val = tp.entry_id_;
                auto delete_btn = std::make_shared<ButtonWidget>("Delete");
                delete_btn->setFixedSize(60, 22);
                delete_btn->setBackgroundColor({0.55f, 0.20f, 0.20f, 0.90f});
                delete_btn->onClick([id_val, trf_weak]() {
                    if (!g_shell_xbus) return;
                    TextReplacementDeleteRequest req{};
                    req.entry_id_ = id_val;
                    g_shell_xbus->Call("com.freebsdx.TextReplacement",
                        static_cast<uint32_t>(TextReplacementOpcode::DeleteShortcut),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [trf_weak](const XBusMessage&) {
                            if (auto trf = trf_weak.lock(); trf && trf->rw.lock())
                                (*trf->rw.lock())();
                        });
                });

                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    shortcut_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    delete_btn,
                });
                trf->col->addChild(row);

                if (trf->count_lbl)
                    trf->count_lbl->setText(std::format("{}", tp.total_));
                if (tp.has_more_) trf->fetch(page + 1);
            });
    };

    *ctx.rebuild_txtrep = [txtrep_col = ctx.txtrep_col, trf]() {
        txtrep_col->clearChildren();
        if (trf->fetch) trf->fetch(0);
    };

    ctx.txtrep_shortcut_input = std::make_shared<TextInputWidget>();
    ctx.txtrep_shortcut_input->setPlaceholder("Shortcut (e.g. omw)");
    ctx.txtrep_phrase_input = std::make_shared<TextInputWidget>();
    ctx.txtrep_phrase_input->setPlaceholder("Phrase (e.g. On my way!)");
    ctx.txtrep_add_status_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.txtrep_add_status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});

    ctx.txtrep_add_btn = std::make_shared<ButtonWidget>("+ Add");
    ctx.txtrep_add_btn->setFixedSize(70, 24);
    ctx.txtrep_add_btn->onClick([txtrep_shortcut_input = ctx.txtrep_shortcut_input,
                             txtrep_phrase_input = ctx.txtrep_phrase_input,
                             txtrep_add_status_lbl = ctx.txtrep_add_status_lbl,
                             rebuild_txtrep = ctx.rebuild_txtrep]() {
        if (!g_shell_xbus) return;
        if (txtrep_shortcut_input->text().empty() || txtrep_phrase_input->text().empty()) return;
        TextReplacementAddRequest req{};
        std::strncpy(req.shortcut_, txtrep_shortcut_input->text().c_str(), sizeof(req.shortcut_) - 1);
        std::strncpy(req.phrase_, txtrep_phrase_input->text().c_str(), sizeof(req.phrase_) - 1);
        g_shell_xbus->Call("com.freebsdx.TextReplacement",
            static_cast<uint32_t>(TextReplacementOpcode::AddShortcut),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [txtrep_shortcut_input, txtrep_phrase_input, txtrep_add_status_lbl,
             rebuild_txtrep](const XBusMessage&) {
                txtrep_shortcut_input->setText("");
                txtrep_phrase_input->setText("");
                txtrep_add_status_lbl->setText("Added \xe2\x9c\x93");
                if (rebuild_txtrep && *rebuild_txtrep) (*rebuild_txtrep)();
            });
    });
}

// App Time Limits (outer-scope-fetcher list + add form). Header + count label +
// result column + an add form (app-name + minutes inputs + status label + Add
// button); the AppLimitFetcher struct + alf shared_ptr live at builder scope and
// are captured by rebuild_applimit. fetch pages com.freebsdx.AppTimeLimit
// ListLimits (per-app used/limit minutes, over-limit-coloured + Remove). Add
// parses the minutes, posts SetLimit, clears the inputs, and refreshes. The
// add-form widgets are added to the AC card so they escape to ctx. Behaviour
// identical to the former inline block.
void buildAppTimeLimitsPanel(ShellContext& ctx) {
    ctx.applimit_hdr_lbl   = std::make_shared<TextWidget>("\xe2\x8f\xb3 App Time Limits", 14.0f);
    ctx.applimit_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.applimit_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.applimit_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.applimit_col       = std::make_shared<FlexColumn>();
    ctx.applimit_col->setGap(8);

    ctx.rebuild_applimit = std::make_shared<std::function<void()>>();
    struct AppLimitFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto alf = std::make_shared<AppLimitFetcher>();
    auto alf_weak = std::weak_ptr<AppLimitFetcher>(alf);

    alf->col       = ctx.applimit_col;
    alf->count_lbl = ctx.applimit_count_lbl;
    alf->rw        = ctx.rebuild_applimit;
    alf->fetch = [alf_weak](uint32_t page) {
        auto alf = alf_weak.lock();
        if (!alf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.AppTimeLimit",
            static_cast<uint32_t>(AppTimeLimitOpcode::ListLimits),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [alf_weak, page](const XBusMessage& r) {
                auto alf = alf_weak.lock();
                if (!alf) return;
                AppTimeLimitPage lp{};
                std::memcpy(&lp, r.payload_, sizeof(lp));
                if (!lp.valid_) return;
                if (page == 0) alf->col->clearChildren();

                auto name_lbl = std::make_shared<TextWidget>(
                    lp.app_name_[0] ? lp.app_name_ : "Unknown App", 11.0f);
                name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                auto usage_lbl = std::make_shared<TextWidget>(
                    std::format("{} / {} min today", lp.used_minutes_today_, lp.limit_minutes_),
                    10.0f);
                usage_lbl->setColor(lp.used_minutes_today_ >= lp.limit_minutes_
                    ? Color{0.90f, 0.45f, 0.35f, 1.0f} : Color{0.55f, 0.55f, 0.60f, 1.0f});

                auto app_name_val = std::make_shared<std::string>(lp.app_name_);
                auto remove_btn = std::make_shared<ButtonWidget>("Remove");
                remove_btn->setFixedSize(60, 22);
                remove_btn->setBackgroundColor({0.55f, 0.20f, 0.20f, 0.90f});
                remove_btn->onClick([app_name_val, alf_weak]() {
                    if (!g_shell_xbus) return;
                    AppTimeLimitRemoveRequest req{};
                    std::strncpy(req.app_name_, app_name_val->c_str(), sizeof(req.app_name_) - 1);
                    g_shell_xbus->Call("com.freebsdx.AppTimeLimit",
                        static_cast<uint32_t>(AppTimeLimitOpcode::RemoveLimit),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [alf_weak](const XBusMessage&) {
                            if (auto alf = alf_weak.lock(); alf && alf->rw.lock())
                                (*alf->rw.lock())();
                        });
                });

                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    name_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    usage_lbl,
                    std::make_shared<Spacer>(8, 1),
                    remove_btn,
                });
                alf->col->addChild(row);

                if (alf->count_lbl)
                    alf->count_lbl->setText(std::format("{}", lp.total_));
                if (lp.has_more_) alf->fetch(page + 1);
            });
    };

    *ctx.rebuild_applimit = [applimit_col = ctx.applimit_col, alf]() {
        applimit_col->clearChildren();
        if (alf->fetch) alf->fetch(0);
    };

    ctx.applimit_name_input = std::make_shared<TextInputWidget>();
    ctx.applimit_name_input->setPlaceholder("App name");
    ctx.applimit_min_input = std::make_shared<TextInputWidget>();
    ctx.applimit_min_input->setPlaceholder("Minutes/day");
    ctx.applimit_add_status_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.applimit_add_status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});

    ctx.applimit_add_btn = std::make_shared<ButtonWidget>("+ Add");
    ctx.applimit_add_btn->setFixedSize(70, 24);
    ctx.applimit_add_btn->onClick([applimit_name_input = ctx.applimit_name_input,
                               applimit_min_input = ctx.applimit_min_input,
                               applimit_add_status_lbl = ctx.applimit_add_status_lbl,
                               rebuild_applimit = ctx.rebuild_applimit]() {
        if (!g_shell_xbus) return;
        if (applimit_name_input->text().empty() || applimit_min_input->text().empty()) return;
        uint32_t mins = 0;
        try { mins = static_cast<uint32_t>(std::stoul(applimit_min_input->text())); }
        catch (...) { return; }
        AppTimeLimitSetRequest req{};
        std::strncpy(req.app_name_, applimit_name_input->text().c_str(), sizeof(req.app_name_) - 1);
        req.limit_minutes_ = mins;
        g_shell_xbus->Call("com.freebsdx.AppTimeLimit",
            static_cast<uint32_t>(AppTimeLimitOpcode::SetLimit),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [applimit_name_input, applimit_min_input, applimit_add_status_lbl,
             rebuild_applimit](const XBusMessage&) {
                applimit_name_input->setText("");
                applimit_min_input->setText("");
                applimit_add_status_lbl->setText("Saved \xe2\x9c\x93");
                if (rebuild_applimit && *rebuild_applimit) (*rebuild_applimit)();
            });
    });
}

// Notification Sounds (outer-scope-fetcher list). Header + count label + result
// column; the NotifSoundFetcher struct + nsf shared_ptr live at builder scope and
// are captured by rebuild_notifsnd. fetch pages com.freebsdx.NotifSound ListSounds
// (per-app name + sound-name label + Cycle button that advances the sound id mod 4
// and posts SetSound). Behaviour identical to the former inline block.
void buildNotifSoundsPanel(ShellContext& ctx) {
    ctx.notifsnd_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x94\x94 Notification Sounds", 14.0f);
    ctx.notifsnd_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.notifsnd_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.notifsnd_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.notifsnd_col       = std::make_shared<FlexColumn>();
    ctx.notifsnd_col->setGap(8);

    ctx.rebuild_notifsnd = std::make_shared<std::function<void()>>();
    struct NotifSoundFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto nsf = std::make_shared<NotifSoundFetcher>();
    auto nsf_weak = std::weak_ptr<NotifSoundFetcher>(nsf);

    nsf->col       = ctx.notifsnd_col;
    nsf->count_lbl = ctx.notifsnd_count_lbl;
    nsf->rw        = ctx.rebuild_notifsnd;
    nsf->fetch = [nsf_weak](uint32_t page) {
        auto nsf = nsf_weak.lock();
        if (!nsf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.NotifSound",
            static_cast<uint32_t>(NotifSoundOpcode::ListSounds),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [nsf_weak, page](const XBusMessage& r) {
                auto nsf = nsf_weak.lock();
                if (!nsf) return;
                NotifSoundPage sp{};
                std::memcpy(&sp, r.payload_, sizeof(sp));
                if (!sp.valid_) return;
                if (page == 0) nsf->col->clearChildren();

                static const char* kSoundNames[] = {"Default", "Chime", "Ping", "Silent"};
                auto sound_id = std::make_shared<uint32_t>(
                    sp.sound_id_ < 4 ? sp.sound_id_ : 0u);

                auto name_lbl = std::make_shared<TextWidget>(
                    sp.app_name_[0] ? sp.app_name_ : "Unknown App", 11.0f);
                name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                auto sound_lbl = std::make_shared<TextWidget>(
                    kSoundNames[*sound_id], 10.0f);
                sound_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                auto app_name_val = std::make_shared<std::string>(sp.app_name_);
                auto cycle_btn = std::make_shared<ButtonWidget>("Cycle");
                cycle_btn->setFixedSize(60, 22);
                cycle_btn->onClick([app_name_val, sound_id, sound_lbl, nsf_weak]() {
                    if (!g_shell_xbus) return;
                    *sound_id = (*sound_id + 1) % 4;
                    sound_lbl->setText(kSoundNames[*sound_id]);
                    NotifSoundRequest req{};
                    std::strncpy(req.app_name_, app_name_val->c_str(), sizeof(req.app_name_) - 1);
                    req.sound_id_ = *sound_id;
                    g_shell_xbus->Call("com.freebsdx.NotifSound",
                        static_cast<uint32_t>(NotifSoundOpcode::SetSound),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                });

                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    name_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    sound_lbl,
                    std::make_shared<Spacer>(8, 1),
                    cycle_btn,
                });
                nsf->col->addChild(row);

                if (nsf->count_lbl)
                    nsf->count_lbl->setText(std::format("{}", sp.total_));
                if (sp.has_more_) nsf->fetch(page + 1);
            });
    };

    *ctx.rebuild_notifsnd = [notifsnd_col = ctx.notifsnd_col, nsf]() {
        notifsnd_col->clearChildren();
        if (nsf->fetch) nsf->fetch(0);
    };
}

// Login Items (outer-scope-fetcher list). Header + count label + result column;
// the LoginItemFetcher struct + lif shared_ptr live at builder scope and are
// captured by rebuild_litem. fetch pages com.freebsdx.LoginItems ListItems
// (per-app name + On/Off state + Toggle button that flips the enabled flag and
// posts SetItem). Behaviour identical to the former inline block.
void buildLoginItemsPanel(ShellContext& ctx) {
    ctx.litem_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x9a\x80 Login Items", 14.0f);
    ctx.litem_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.litem_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.litem_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.litem_col       = std::make_shared<FlexColumn>();
    ctx.litem_col->setGap(8);

    ctx.rebuild_litem = std::make_shared<std::function<void()>>();
    struct LoginItemFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto lif = std::make_shared<LoginItemFetcher>();
    auto lif_weak = std::weak_ptr<LoginItemFetcher>(lif);

    lif->col       = ctx.litem_col;
    lif->count_lbl = ctx.litem_count_lbl;
    lif->rw        = ctx.rebuild_litem;
    lif->fetch = [lif_weak](uint32_t page) {
        auto lif = lif_weak.lock();
        if (!lif || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.LoginItems",
            static_cast<uint32_t>(LoginItemOpcode::ListItems),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [lif_weak, page](const XBusMessage& r) {
                auto lif = lif_weak.lock();
                if (!lif) return;
                LoginItemPage lp{};
                std::memcpy(&lp, r.payload_, sizeof(lp));
                if (!lp.valid_) return;
                if (page == 0) lif->col->clearChildren();

                auto enabled = std::make_shared<uint32_t>(lp.enabled_ ? 1u : 0u);

                auto name_lbl = std::make_shared<TextWidget>(
                    lp.app_name_[0] ? lp.app_name_ : "Unknown App", 11.0f);
                name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                auto state_lbl = std::make_shared<TextWidget>(
                    *enabled ? "On" : "Off", 10.0f);
                state_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                auto app_name_val = std::make_shared<std::string>(lp.app_name_);
                auto toggle_btn = std::make_shared<ButtonWidget>("Toggle");
                toggle_btn->setFixedSize(64, 22);
                toggle_btn->onClick([app_name_val, enabled, state_lbl]() {
                    if (!g_shell_xbus) return;
                    *enabled = *enabled ? 0u : 1u;
                    state_lbl->setText(*enabled ? "On" : "Off");
                    LoginItemRequest req{};
                    std::strncpy(req.app_name_, app_name_val->c_str(), sizeof(req.app_name_) - 1);
                    req.enabled_ = *enabled;
                    g_shell_xbus->Call("com.freebsdx.LoginItems",
                        static_cast<uint32_t>(LoginItemOpcode::SetItem),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                });

                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    name_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    state_lbl,
                    std::make_shared<Spacer>(8, 1),
                    toggle_btn,
                });
                lif->col->addChild(row);

                if (lif->count_lbl)
                    lif->count_lbl->setText(std::format("{}", lp.total_));
                if (lp.has_more_) lif->fetch(page + 1);
            });
    };

    *ctx.rebuild_litem = [litem_col = ctx.litem_col, lif]() {
        litem_col->clearChildren();
        if (lif->fetch) lif->fetch(0);
    };
}

// Trusted Devices (outer-scope-fetcher list). Header + count label + result
// column; the TrustedDevFetcher struct + tdf shared_ptr live at builder scope and
// are captured by rebuild_tdev. fetch pages com.freebsdx.TrustedDevices
// ListDevices (per-device name + Trusted/Blocked state + Toggle button that flips
// the trust flag and posts SetTrust). Behaviour identical to the former inline
// block.
void buildTrustedDevicesPanel(ShellContext& ctx) {
    ctx.tdev_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x94\x90 Trusted Devices", 14.0f);
    ctx.tdev_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.tdev_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.tdev_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.tdev_col       = std::make_shared<FlexColumn>();
    ctx.tdev_col->setGap(8);

    ctx.rebuild_tdev = std::make_shared<std::function<void()>>();
    struct TrustedDevFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto tdf = std::make_shared<TrustedDevFetcher>();
    auto tdf_weak = std::weak_ptr<TrustedDevFetcher>(tdf);

    tdf->col       = ctx.tdev_col;
    tdf->count_lbl = ctx.tdev_count_lbl;
    tdf->rw        = ctx.rebuild_tdev;
    tdf->fetch = [tdf_weak](uint32_t page) {
        auto tdf = tdf_weak.lock();
        if (!tdf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.TrustedDevices",
            static_cast<uint32_t>(TrustedDeviceOpcode::ListDevices),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [tdf_weak, page](const XBusMessage& r) {
                auto tdf = tdf_weak.lock();
                if (!tdf) return;
                TrustedDevicePage dp{};
                std::memcpy(&dp, r.payload_, sizeof(dp));
                if (!dp.valid_) return;
                if (page == 0) tdf->col->clearChildren();

                auto trusted = std::make_shared<uint32_t>(dp.trusted_ ? 1u : 0u);

                auto name_lbl = std::make_shared<TextWidget>(
                    dp.device_name_[0] ? dp.device_name_ : "Unknown Device", 11.0f);
                name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                auto state_lbl = std::make_shared<TextWidget>(
                    *trusted ? "Trusted" : "Blocked", 10.0f);
                state_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                auto dev_name_val = std::make_shared<std::string>(dp.device_name_);
                auto toggle_btn = std::make_shared<ButtonWidget>("Toggle");
                toggle_btn->setFixedSize(64, 22);
                toggle_btn->onClick([dev_name_val, trusted, state_lbl]() {
                    if (!g_shell_xbus) return;
                    *trusted = *trusted ? 0u : 1u;
                    state_lbl->setText(*trusted ? "Trusted" : "Blocked");
                    TrustedDeviceRequest req{};
                    std::strncpy(req.device_name_, dev_name_val->c_str(), sizeof(req.device_name_) - 1);
                    req.trusted_ = *trusted;
                    g_shell_xbus->Call("com.freebsdx.TrustedDevices",
                        static_cast<uint32_t>(TrustedDeviceOpcode::SetTrust),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                });

                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    name_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    state_lbl,
                    std::make_shared<Spacer>(8, 1),
                    toggle_btn,
                });
                tdf->col->addChild(row);

                if (tdf->count_lbl)
                    tdf->count_lbl->setText(std::format("{}", dp.total_));
                if (dp.has_more_) tdf->fetch(page + 1);
            });
    };

    *ctx.rebuild_tdev = [tdev_col = ctx.tdev_col, tdf]() {
        tdev_col->clearChildren();
        if (tdf->fetch) tdf->fetch(0);
    };
}

// Keyboard Shortcuts (outer-scope-fetcher list). Header + count label + result
// column; the KeyShortcutFetcher struct + ksf shared_ptr live at builder scope and
// are captured by rebuild_kbsc. fetch pages com.freebsdx.KeyboardShortcuts
// ListShortcuts (per-shortcut action + binding + On/Off toggle button that flips
// the enabled flag and posts SetShortcut; the button's own label reflects state).
// Behaviour identical to the former inline block.
void buildKeyboardShortcutsPanel(ShellContext& ctx) {
    ctx.kbsc_hdr_lbl   = std::make_shared<TextWidget>("\xe2\x8c\xa8 Keyboard Shortcuts", 14.0f);
    ctx.kbsc_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.kbsc_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.kbsc_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.kbsc_col       = std::make_shared<FlexColumn>();
    ctx.kbsc_col->setGap(8);

    ctx.rebuild_kbsc = std::make_shared<std::function<void()>>();
    struct KeyShortcutFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto ksf = std::make_shared<KeyShortcutFetcher>();
    auto ksf_weak = std::weak_ptr<KeyShortcutFetcher>(ksf);

    ksf->col       = ctx.kbsc_col;
    ksf->count_lbl = ctx.kbsc_count_lbl;
    ksf->rw        = ctx.rebuild_kbsc;
    ksf->fetch = [ksf_weak](uint32_t page) {
        auto ksf = ksf_weak.lock();
        if (!ksf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.KeyboardShortcuts",
            static_cast<uint32_t>(KeyboardShortcutOpcode::ListShortcuts),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [ksf_weak, page](const XBusMessage& r) {
                auto ksf = ksf_weak.lock();
                if (!ksf) return;
                KeyShortcutPage kp{};
                std::memcpy(&kp, r.payload_, sizeof(kp));
                if (!kp.valid_) return;
                if (page == 0) ksf->col->clearChildren();

                auto enabled = std::make_shared<uint32_t>(kp.enabled_ ? 1u : 0u);

                auto action_lbl = std::make_shared<TextWidget>(
                    kp.action_[0] ? kp.action_ : "Unknown", 11.0f);
                action_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                auto binding_lbl = std::make_shared<TextWidget>(
                    kp.binding_[0] ? kp.binding_ : "\xe2\x80\x94", 10.0f);
                binding_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                auto action_val = std::make_shared<std::string>(kp.action_);
                auto toggle_btn = std::make_shared<ButtonWidget>(*enabled ? "On" : "Off");
                toggle_btn->setFixedSize(52, 22);
                toggle_btn->onClick([action_val, enabled, toggle_btn]() {
                    if (!g_shell_xbus) return;
                    *enabled = *enabled ? 0u : 1u;
                    toggle_btn->setText(*enabled ? "On" : "Off");
                    KeyShortcutRequest req{};
                    std::strncpy(req.action_, action_val->c_str(), sizeof(req.action_) - 1);
                    req.enabled_ = *enabled;
                    g_shell_xbus->Call("com.freebsdx.KeyboardShortcuts",
                        static_cast<uint32_t>(KeyboardShortcutOpcode::SetShortcut),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                });

                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    action_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    binding_lbl,
                    std::make_shared<Spacer>(8, 1),
                    toggle_btn,
                });
                ksf->col->addChild(row);

                if (ksf->count_lbl)
                    ksf->count_lbl->setText(std::format("{}", kp.total_));
                if (kp.has_more_) ksf->fetch(page + 1);
            });
    };

    *ctx.rebuild_kbsc = [kbsc_col = ctx.kbsc_col, ksf]() {
        kbsc_col->clearChildren();
        if (ksf->fetch) ksf->fetch(0);
    };
}

// Default Apps (outer-scope-fetcher list). Header + count label + result column;
// the DefaultAppFetcher struct + daf shared_ptr live at builder scope and are
// captured by rebuild_dapp. fetch pages com.freebsdx.DefaultApps ListDefaults
// (per-category name + handler + Cycle button that posts CycleDefault). Behaviour
// identical to the former inline block.
void buildDefaultAppsPanel(ShellContext& ctx) {
    ctx.dapp_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x93\x82 Default Apps", 14.0f);
    ctx.dapp_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.dapp_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.dapp_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.dapp_col       = std::make_shared<FlexColumn>();
    ctx.dapp_col->setGap(8);

    ctx.rebuild_dapp = std::make_shared<std::function<void()>>();
    struct DefaultAppFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto daf = std::make_shared<DefaultAppFetcher>();
    auto daf_weak = std::weak_ptr<DefaultAppFetcher>(daf);

    daf->col       = ctx.dapp_col;
    daf->count_lbl = ctx.dapp_count_lbl;
    daf->rw        = ctx.rebuild_dapp;
    daf->fetch = [daf_weak](uint32_t page) {
        auto daf = daf_weak.lock();
        if (!daf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.DefaultApps",
            static_cast<uint32_t>(DefaultAppsOpcode::ListDefaults),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [daf_weak, page](const XBusMessage& r) {
                auto daf = daf_weak.lock();
                if (!daf) return;
                DefaultAppPage ap{};
                std::memcpy(&ap, r.payload_, sizeof(ap));
                if (!ap.valid_) return;
                if (page == 0) daf->col->clearChildren();

                auto cat_lbl = std::make_shared<TextWidget>(
                    ap.category_[0] ? ap.category_ : "Unknown", 11.0f);
                cat_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                auto handler_lbl = std::make_shared<TextWidget>(
                    ap.handler_[0] ? ap.handler_ : "\xe2\x80\x94", 10.0f);
                handler_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                auto category_val = std::make_shared<std::string>(ap.category_);
                auto cycle_btn = std::make_shared<ButtonWidget>("Cycle");
                cycle_btn->setFixedSize(60, 22);
                cycle_btn->onClick([category_val]() {
                    if (!g_shell_xbus) return;
                    DefaultAppRequest req{};
                    std::strncpy(req.category_, category_val->c_str(), sizeof(req.category_) - 1);
                    g_shell_xbus->Call("com.freebsdx.DefaultApps",
                        static_cast<uint32_t>(DefaultAppsOpcode::CycleDefault),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                });

                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    cat_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    handler_lbl,
                    std::make_shared<Spacer>(8, 1),
                    cycle_btn,
                });
                daf->col->addChild(row);

                if (daf->count_lbl)
                    daf->count_lbl->setText(std::format("{}", ap.total_));
                if (ap.has_more_) daf->fetch(page + 1);
            });
    };

    *ctx.rebuild_dapp = [dapp_col = ctx.dapp_col, daf]() {
        dapp_col->clearChildren();
        if (daf->fetch) daf->fetch(0);
    };
}

// Reminders (outer-scope-fetcher list). Header + count label + result column; the
// RemFetcher struct + remf shared_ptr live at builder scope and are captured by
// rebuild_rem. fetch pages com.freebsdx.Reminder ListReminders (per-reminder title
// + due (overdue-coloured) + Overdue badge + Done button that completes it and
// re-triggers the rebuild). Refreshed on loop_tick%60. Behaviour identical to the
// former inline block.
void buildRemindersPanel(ShellContext& ctx) {
    ctx.rem_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x94\x94 Reminders", 14.0f);
    ctx.rem_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.rem_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.rem_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.rem_col       = std::make_shared<FlexColumn>();
    ctx.rem_col->setGap(6);

    ctx.rebuild_rem = std::make_shared<std::function<void()>>();
    struct RemFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto remf = std::make_shared<RemFetcher>();
    auto remf_weak = std::weak_ptr<RemFetcher>(remf);

    remf->col       = ctx.rem_col;
    remf->count_lbl = ctx.rem_count_lbl;
    remf->rw        = ctx.rebuild_rem;
    remf->fetch = [remf_weak](uint32_t page) {
        auto remf = remf_weak.lock();
        if (!remf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.Reminder",
            static_cast<uint32_t>(ReminderOpcode::ListReminders),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [remf_weak, page](const XBusMessage& r) {
                auto remf = remf_weak.lock();
                if (!remf) return;
                ReminderPage rp{};
                std::memcpy(&rp, r.payload_, sizeof(rp));
                if (!rp.valid_) return;
                if (page == 0) remf->col->clearChildren();

                auto title_lbl = std::make_shared<TextWidget>(
                    rp.title_[0] ? rp.title_ : "(no title)", 11.0f);
                title_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                auto due_lbl = std::make_shared<TextWidget>(
                    rp.due_[0] ? rp.due_ : "", 10.0f);
                due_lbl->setColor(rp.overdue_
                    ? Color{0.85f, 0.35f, 0.35f, 1.0f}
                    : Color{0.55f, 0.55f, 0.60f, 1.0f});

                // Overdue badge
                auto overdue_badge = std::make_shared<TextWidget>(
                    rp.overdue_ ? "Overdue" : "", 10.0f);
                overdue_badge->setColor({0.85f, 0.30f, 0.30f, 1.0f});

                auto id_str = std::make_shared<std::string>(rp.id_);
                auto done_btn = std::make_shared<ButtonWidget>("\xe2\x9c\x93 Done");
                done_btn->setFixedSize(64, 22);
                done_btn->setBackgroundColor({0.22f, 0.48f, 0.28f, 0.90f});
                done_btn->onClick([id_str, remf_weak]() {
                    if (!g_shell_xbus) return;
                    ReminderActionRequest req{};
                    std::strncpy(req.id_, id_str->c_str(), sizeof(req.id_)-1);
                    g_shell_xbus->Call("com.freebsdx.Reminder",
                        static_cast<uint32_t>(ReminderOpcode::CompleteReminder),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [remf_weak](const XBusMessage&) {
                            if (auto remf = remf_weak.lock(); remf && remf->rw.lock())
                                (*remf->rw.lock())();
                        });
                });

                auto top_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    title_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    overdue_badge,
                });
                auto bot_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    due_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    done_btn,
                });
                auto entry = std::make_shared<FlexColumn>();
                entry->addChild(top_row);
                entry->addChild(std::make_shared<Spacer>(1, 2));
                entry->addChild(bot_row);
                remf->col->addChild(entry);

                if (remf->count_lbl)
                    remf->count_lbl->setText(std::format("{}", rp.total_));
                if (rp.has_more_) remf->fetch(page + 1);
            });
    };

    *ctx.rebuild_rem = [rem_col = ctx.rem_col, remf]() {
        rem_col->clearChildren();
        if (remf->fetch) remf->fetch(0);
    };
}

// Scheduled Backups (outer-scope-fetcher list). Header + count label + result
// column; the BkpFetcher struct + bkpf shared_ptr live at builder scope and are
// captured by rebuild_bkp. fetch pages com.freebsdx.Backup ListJobs (per-job name
// + status badge + last/next-run + Run Now button that posts RunJob, sets the
// status to Running, and re-triggers the rebuild). Refreshed on loop_tick%120.
// Behaviour identical to the former inline block.
void buildScheduledBackupsPanel(ShellContext& ctx) {
    ctx.bkp_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x92\xbe Scheduled Backups", 14.0f);
    ctx.bkp_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.bkp_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.bkp_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.bkp_col       = std::make_shared<FlexColumn>();
    ctx.bkp_col->setGap(8);

    ctx.rebuild_bkp = std::make_shared<std::function<void()>>();
    struct BkpFetcher {
        std::shared_ptr<FlexColumn>          col;
        std::shared_ptr<TextWidget>          count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)>        fetch;
    };
    auto bkpf = std::make_shared<BkpFetcher>();
    auto bkpf_weak = std::weak_ptr<BkpFetcher>(bkpf);

    bkpf->col       = ctx.bkp_col;
    bkpf->count_lbl = ctx.bkp_count_lbl;
    bkpf->rw        = ctx.rebuild_bkp;
    bkpf->fetch = [bkpf_weak](uint32_t page) {
        auto bkpf = bkpf_weak.lock();
        if (!bkpf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.Backup",
            static_cast<uint32_t>(BackupOpcode::ListJobs),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [bkpf_weak, page](const XBusMessage& r) {
                auto bkpf = bkpf_weak.lock();
                if (!bkpf) return;
                BackupJobPage jp{};
                std::memcpy(&jp, r.payload_, sizeof(jp));
                if (!jp.valid_) return;
                if (page == 0) bkpf->col->clearChildren();

                // Status badge: 0=OK(green), 1=Running(yellow), 2=Failed(red)
                static const char*  kStatusStr[]   = {"OK", "Running", "Failed"};
                static const Color  kStatusColor[] = {
                    {0.30f, 0.75f, 0.40f, 1.0f},
                    {0.80f, 0.70f, 0.25f, 1.0f},
                    {0.80f, 0.30f, 0.30f, 1.0f},
                };
                uint32_t st = jp.status_ < 3 ? jp.status_ : 0;

                auto name_lbl   = std::make_shared<TextWidget>(jp.name_[0] ? jp.name_ : "Unnamed", 11.0f);
                name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});
                auto status_lbl = std::make_shared<TextWidget>(kStatusStr[st], 10.0f);
                status_lbl->setColor(kStatusColor[st]);
                auto last_lbl   = std::make_shared<TextWidget>(
                    jp.last_run_[0] ? std::string("Last: ") + jp.last_run_ : "Never run", 10.0f);
                last_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
                auto next_lbl   = std::make_shared<TextWidget>(
                    jp.next_run_[0] ? std::string("Next: ") + jp.next_run_ : "", 10.0f);
                next_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                auto id_str = std::make_shared<std::string>(jp.id_);
                auto run_btn = std::make_shared<ButtonWidget>("Run Now");
                run_btn->setFixedSize(72, 24);
                run_btn->setBackgroundColor({0.20f, 0.45f, 0.70f, 0.90f});
                run_btn->onClick([id_str, status_lbl, bkpf_weak]() {
                    if (!g_shell_xbus) return;
                    BackupJobRequest req{};
                    std::strncpy(req.id_, id_str->c_str(), sizeof(req.id_)-1);
                    g_shell_xbus->Call("com.freebsdx.Backup",
                        static_cast<uint32_t>(BackupOpcode::RunJob),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [status_lbl, bkpf_weak](const XBusMessage&) {
                            status_lbl->setText("Running");
                            status_lbl->setColor({0.80f, 0.70f, 0.25f, 1.0f});
                            if (auto bkpf = bkpf_weak.lock(); bkpf && bkpf->rw.lock())
                                (*bkpf->rw.lock())();
                        });
                });

                auto name_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    name_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    status_lbl,
                });
                auto time_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    last_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    next_lbl,
                });
                auto btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    std::make_shared<Spacer>(1, 1, true),
                    run_btn,
                });
                auto entry = std::make_shared<FlexColumn>();
                entry->addChild(name_row);
                entry->addChild(std::make_shared<Spacer>(1, 2));
                entry->addChild(time_row);
                entry->addChild(std::make_shared<Spacer>(1, 4));
                entry->addChild(btn_row);
                bkpf->col->addChild(entry);

                if (bkpf->count_lbl)
                    bkpf->count_lbl->setText(std::format("{}", jp.total_));
                if (jp.has_more_) bkpf->fetch(page + 1);
            });
    };

    *ctx.rebuild_bkp = [bkp_col = ctx.bkp_col, bkpf]() {
        bkp_col->clearChildren();
        if (bkpf->fetch) bkpf->fetch(0);
    };
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

// Bluetooth Audio (outer-scope-fetcher list). Header + count label + result
// column; the BtAudioFetcher struct + baf shared_ptr live at builder scope and are
// captured by rebuild_btaudio. fetch pages com.freebsdx.BtAudio ListDevices
// (per-device status dot + name + codec badge + battery + Connect/Disconnect
// button that re-triggers the rebuild). Page 0 handles the empty/count line.
// Refreshed on loop_tick%30. Behaviour identical to the former inline block.
void buildBtAudioPanel(ShellContext& ctx) {
    ctx.btaudio_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x8e\xa7 BT Audio", 14.0f);
    ctx.btaudio_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.btaudio_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.btaudio_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.btaudio_col       = std::make_shared<FlexColumn>();
    ctx.btaudio_col->setGap(6);

    ctx.rebuild_btaudio = std::make_shared<std::function<void()>>();
    struct BtAudioFetcher {
        std::shared_ptr<FlexColumn>   col;
        std::shared_ptr<TextWidget>   count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)> fetch;
    };
    auto baf      = std::make_shared<BtAudioFetcher>();
    auto baf_weak = std::weak_ptr<BtAudioFetcher>(baf);
    baf->col       = ctx.btaudio_col;
    baf->count_lbl = ctx.btaudio_count_lbl;
    baf->rw        = ctx.rebuild_btaudio;
    baf->fetch = [baf_weak](uint32_t page) {
        if (!g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.BtAudio",
            static_cast<uint32_t>(BtAudioOpcode::ListDevices),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [baf_weak, page](const XBusMessage& r) {
                auto pf = baf_weak.lock();
                if (!pf) return;
                BtAudioPage bp{};
                std::memcpy(&bp, r.payload_, sizeof(bp));
                if (page == 0) {
                    if (bp.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No paired BT audio", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                        pf->count_lbl->setText("");
                        return;
                    }
                    pf->count_lbl->setText(
                        std::format("{} device{}", bp.total_, bp.total_ == 1 ? "" : "s"));
                }
                if (!bp.valid_) {
                    if (bp.has_more_)
                        if (auto p = baf_weak.lock()) p->fetch(page + 1);
                    return;
                }
                static const char* kCodecs[] = {"SBC","AAC","aptX","aptX-HD","LDAC"};
                std::string addr(bp.address_);
                std::string name(bp.name_[0] ? bp.name_ : "(unknown)");
                bool conn = bp.connected_ != 0;

                auto dev_lbl = std::make_shared<TextWidget>(name, 11.0f);
                dev_lbl->setColor({0.88f, 0.88f, 0.92f, 1.0f});

                // Codec badge
                const char* codec_str = bp.codec_ < 5 ? kCodecs[bp.codec_] : "?";
                auto codec_lbl = std::make_shared<TextWidget>(codec_str, 10.0f);
                codec_lbl->setColor({0.55f, 0.75f, 0.95f, 1.0f});

                // Battery badge
                std::string bat_str = bp.battery_pct_ <= 100
                    ? std::format("{}%", bp.battery_pct_) : "";
                auto bat_lbl = std::make_shared<TextWidget>(bat_str, 10.0f);
                Color bat_col = bp.battery_pct_ <= 20  ? Color{0.95f, 0.35f, 0.30f, 1.0f}
                              : bp.battery_pct_ <= 50  ? Color{0.95f, 0.75f, 0.25f, 1.0f}
                                                       : Color{0.35f, 0.85f, 0.45f, 1.0f};
                if (bp.battery_pct_ <= 100) bat_lbl->setColor(bat_col);

                // Connect / Disconnect button
                auto conn_btn = std::make_shared<ButtonWidget>(
                    conn ? "Disconnect" : "Connect",
                    [addr, conn, baf_weak]() {
                        if (!g_shell_xbus) return;
                        BtAudioRequest req{};
                        std::strncpy(req.address_, addr.c_str(), sizeof(req.address_) - 1);
                        uint32_t op = conn
                            ? static_cast<uint32_t>(BtAudioOpcode::Disconnect)
                            : static_cast<uint32_t>(BtAudioOpcode::Connect);
                        g_shell_xbus->Call("com.freebsdx.BtAudio", op,
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                            [baf_weak](const XBusMessage&) {
                                if (auto p = baf_weak.lock())
                                    if (auto rw = p->rw.lock())
                                        if (*rw) (*rw)();
                            });
                    });
                conn_btn->setFixedSize(conn ? 90 : 72, 24);
                conn_btn->setBackgroundColor(conn
                    ? Color{0.55f, 0.20f, 0.20f, 0.90f}
                    : Color{0.18f, 0.36f, 0.78f, 0.95f});

                // Status dot
                auto dot_lbl = std::make_shared<TextWidget>(
                    conn ? "\xf0\x9f\x9f\xa2" : "\xe2\x97\x8f", 10.0f);
                if (!conn) dot_lbl->setColor({0.35f, 0.35f, 0.40f, 1.0f});

                auto row = std::make_shared<FlexRow>(
                    std::vector<std::shared_ptr<Widget>>{
                        dot_lbl,
                        std::make_shared<Spacer>(6, 1),
                        dev_lbl,
                        std::make_shared<Spacer>(1, 1, true),
                        codec_lbl,
                        std::make_shared<Spacer>(6, 1),
                        bat_lbl,
                        std::make_shared<Spacer>(6, 1),
                        conn_btn,
                    });
                pf->col->addChild(row);
                if (bp.has_more_)
                    if (auto p = baf_weak.lock()) p->fetch(page + 1);
            });
    };
    *ctx.rebuild_btaudio = [btaudio_col = ctx.btaudio_col, baf]() {
        btaudio_col->clearChildren();
        if (baf->fetch) baf->fetch(0);
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

// USB Devices (detail, outer-scope-fetcher list). Header + count label + result
// column (+ a status label, both carded); the UsbDevFetcher struct + udf
// shared_ptr live at builder scope and are captured by rebuild_usbdev. fetch pages
// com.freebsdx.UsbDevice ListDevices (per-device class tag/colour + product + opt.
// manufacturer line + Eject for ejectable devices, which re-triggers the rebuild).
// Refreshed on loop_tick%30. Behaviour identical to the former inline block.
void buildUsbDeviceDetailPanel(ShellContext& ctx) {
    ctx.usbdev_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x94\x8c USB Devices", 14.0f);
    ctx.usbdev_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.usbdev_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.usbdev_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.usbdev_col       = std::make_shared<FlexColumn>();
    ctx.usbdev_col->setGap(4);
    ctx.usbdev_status_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.usbdev_status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});

    ctx.rebuild_usbdev = std::make_shared<std::function<void()>>();
    struct UsbDevFetcher {
        std::shared_ptr<FlexColumn> col;
        std::shared_ptr<TextWidget> count_lbl;
        std::weak_ptr<std::function<void()>> rw;
        std::function<void(uint32_t)> fetch;
    };
    auto udf      = std::make_shared<UsbDevFetcher>();
    auto udf_weak = std::weak_ptr<UsbDevFetcher>(udf);
    udf->col       = ctx.usbdev_col;
    udf->count_lbl = ctx.usbdev_count_lbl;
    udf->rw        = ctx.rebuild_usbdev;
    udf->fetch = [udf_weak](uint32_t page) {
        if (!g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.UsbDevice",
            static_cast<uint32_t>(UsbDeviceOpcode::ListDevices),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [udf_weak, page](const XBusMessage& r) {
                auto pf = udf_weak.lock();
                if (!pf) return;
                UsbDeviceDetailPage dp{};
                std::memcpy(&dp, r.payload_, sizeof(dp));
                if (page == 0) {
                    if (dp.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No USB devices", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                        pf->count_lbl->setText("");
                        return;
                    }
                    pf->count_lbl->setText(
                        std::format("{} device{}", dp.total_, dp.total_ == 1 ? "" : "s"));
                }
                if (!dp.valid_) {
                    if (dp.has_more_)
                        if (auto p = udf_weak.lock()) p->fetch(page + 1);
                    return;
                }
                Color cls_color =
                    dp.class_ == 0x08 ? Color{0.35f, 0.85f, 0.45f, 1.0f} :
                    dp.class_ == 0x03 ? Color{0.45f, 0.75f, 0.95f, 1.0f} :
                    dp.class_ == 0x01 ? Color{0.75f, 0.50f, 0.90f, 1.0f} :
                    dp.class_ == 0x09 ? Color{0.85f, 0.72f, 0.30f, 1.0f} :
                                        Color{0.55f, 0.55f, 0.60f, 1.0f};
                const char* cls_tag =
                    dp.class_ == 0x08 ? "MSC" :
                    dp.class_ == 0x03 ? "HID" :
                    dp.class_ == 0x01 ? "AUD" :
                    dp.class_ == 0x09 ? "HUB" : "USB";
                std::string prod(dp.product_[0] ? dp.product_ : "(unknown)");
                std::string mfr(dp.manufacturer_);
                auto ud_cls_lbl  = std::make_shared<TextWidget>(cls_tag, 10.0f);
                ud_cls_lbl->setColor(cls_color);
                auto ud_name_lbl = std::make_shared<TextWidget>(prod, 11.0f);
                ud_name_lbl->setColor({0.88f, 0.88f, 0.92f, 1.0f});
                std::vector<std::shared_ptr<Widget>> ud_row_ws{
                    ud_cls_lbl,
                    std::make_shared<Spacer>(6, 1),
                    ud_name_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                };
                if (dp.ejectable_) {
                    uint32_t dev_id = dp.device_id_;
                    auto eject_btn = std::make_shared<ButtonWidget>("\xe2\x8f\x8f Eject",
                        [dev_id, udf_weak]() {
                            if (!g_shell_xbus) return;
                            UsbDeviceRequest req{};
                            req.device_id_ = dev_id;
                            g_shell_xbus->Call("com.freebsdx.UsbDevice",
                                static_cast<uint32_t>(UsbDeviceOpcode::Eject),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                [udf_weak](const XBusMessage&) {
                                    if (auto p = udf_weak.lock())
                                        if (auto rw = p->rw.lock())
                                            if (*rw) (*rw)();
                                });
                        });
                    eject_btn->setFixedSize(72, 24);
                    eject_btn->setBackgroundColor({0.55f, 0.20f, 0.20f, 0.90f});
                    ud_row_ws.push_back(eject_btn);
                }
                pf->col->addChild(std::make_shared<FlexRow>(ud_row_ws));
                if (!mfr.empty()) {
                    auto ud_mfr_lbl = std::make_shared<TextWidget>(mfr, 10.0f);
                    ud_mfr_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});
                    pf->col->addChild(ud_mfr_lbl);
                }
                pf->col->addChild(std::make_shared<Spacer>(1, 4));
                if (dp.has_more_)
                    if (auto p = udf_weak.lock()) p->fetch(page + 1);
            });
    };
    *ctx.rebuild_usbdev = [usbdev_col = ctx.usbdev_col, udf]() {
        usbdev_col->clearChildren();
        if (udf->fetch) udf->fetch(0);
    };
}

// Network Shares (mount). Header + count label + result column + a collapsible
// mount form (host/share/mount/user/pass inputs + SMB/NFS protocol toggle + Mount
// button, all held by ns_form_col) + a toggle button that shows/hides the form.
// The paged NsFetcher (whose fetch is a shared_ptr<function>) lists configured
// shares (protocol badge + host/share path + mount dot + Mount/Unmount). nspf is
// kept alive by a STRONG capture in rebuild_ns (matching the original's
// program-lifetime local). Only header/count/col/form-col/status/toggle-btn +
// rebuild_ns escape; the form widgets stay builder-local (held by ns_form_col).
// Refreshed on loop_tick%30. Behaviour identical to the former inline block.
void buildNetworkSharesMountPanel(ShellContext& ctx) {
    ctx.nsm_hdr_lbl    = std::make_shared<TextWidget>("\xf0\x9f\x97\x82 Network Shares", 14.0f);
    ctx.nsm_hdr_lbl->setColor({0.85f, 0.85f, 0.92f, 1.0f});
    ctx.nsm_count_lbl  = std::make_shared<TextWidget>("", 11.0f);
    ctx.nsm_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.ns_col         = std::make_shared<FlexColumn>();
    ctx.ns_col->setGap(4);
    ctx.ns_form_col    = std::make_shared<FlexColumn>();
    ctx.ns_form_col->setGap(4);
    ctx.ns_form_col->setVisibility(false);
    ctx.ns_status_lbl  = std::make_shared<TextWidget>("", 10.0f);
    ctx.ns_status_lbl->setColor({0.55f, 0.55f, 0.58f, 1.0f});
    ctx.rebuild_ns     = std::make_shared<std::function<void()>>();

    // Mount form inputs
    auto ns_host_input  = std::make_shared<TextInputWidget>();
    auto ns_share_input = std::make_shared<TextInputWidget>();
    auto ns_mnt_input   = std::make_shared<TextInputWidget>();
    auto ns_user_input  = std::make_shared<TextInputWidget>();
    auto ns_pass_input  = std::make_shared<TextInputWidget>();
    ns_host_input->setPlaceholder("Host / IP");
    ns_share_input->setPlaceholder("Share path (e.g. /share)");
    ns_mnt_input->setPlaceholder("Mount point (e.g. /mnt/share)");
    ns_user_input->setPlaceholder("Username");
    ns_pass_input->setPlaceholder("Password");
    /* password mode unsupported */

    // Protocol toggle: SMB / NFS
    auto ns_proto_cur     = std::make_shared<int>(0);  // 0=SMB, 1=NFS
    auto ns_proto_smb_btn = std::make_shared<ButtonWidget>("SMB", []() {});
    auto ns_proto_nfs_btn = std::make_shared<ButtonWidget>("NFS", []() {});
    ns_proto_smb_btn->setFixedSize(44, 24);
    ns_proto_nfs_btn->setFixedSize(44, 24);
    auto ns_proto_hl = [ns_proto_cur, ns_proto_smb_btn, ns_proto_nfs_btn]() {
        bool smb = (*ns_proto_cur == 0);
        ns_proto_smb_btn->setBackgroundColor(smb
            ? Color{0.20f, 0.50f, 0.95f, 1.0f} : Color{0.22f, 0.22f, 0.25f, 1.0f});
        ns_proto_nfs_btn->setBackgroundColor(!smb
            ? Color{0.20f, 0.50f, 0.95f, 1.0f} : Color{0.22f, 0.22f, 0.25f, 1.0f});
    };
    ns_proto_hl();
    ns_proto_smb_btn->onClick([ns_proto_cur, ns_proto_hl]() { *ns_proto_cur = 0; ns_proto_hl(); });
    ns_proto_nfs_btn->onClick([ns_proto_cur, ns_proto_hl]() { *ns_proto_cur = 1; ns_proto_hl(); });

    auto ns_mount_btn = std::make_shared<ButtonWidget>("Mount",
        [ns_host_input, ns_share_input, ns_mnt_input, ns_user_input, ns_pass_input,
         ns_proto_cur, ns_status_lbl = ctx.ns_status_lbl, ns_form_col = ctx.ns_form_col,
         rebuild_ns = ctx.rebuild_ns]() {
            auto host  = ns_host_input->getText();
            auto share = ns_share_input->getText();
            if (host.empty() || share.empty()) {
                ns_status_lbl->setText("Host and share are required");
                ns_status_lbl->setColor({0.92f, 0.35f, 0.30f, 1.0f});
                return;
            }
            NetworkShareMountRequest req{};
            std::strncpy(req.host_,       host.c_str(),
                         sizeof(req.host_) - 1);
            std::strncpy(req.share_,      share.c_str(),
                         sizeof(req.share_) - 1);
            auto mnt = ns_mnt_input->getText();
            std::strncpy(req.mountpoint_, mnt.c_str(),
                         sizeof(req.mountpoint_) - 1);
            auto user = ns_user_input->getText();
            std::strncpy(req.user_,       user.c_str(),
                         sizeof(req.user_) - 1);
            auto pass = ns_pass_input->getText();
            std::strncpy(req.password_,   pass.c_str(),
                         sizeof(req.password_) - 1);
            req.protocol_ = static_cast<uint32_t>(*ns_proto_cur);
            std::vector<uint8_t> payload(sizeof(req));
            std::memcpy(payload.data(), &req, sizeof(req));
            ns_status_lbl->setText("Mounting\xe2\x80\xa6");
            ns_status_lbl->setColor({0.85f, 0.85f, 0.30f, 1.0f});
            if (!g_shell_xbus) return;
            g_shell_xbus->Call("com.freebsdx.NetworkShare",
                static_cast<uint32_t>(NetworkShareMountOpcode::Mount), payload,
                [ns_status_lbl, ns_form_col, rebuild_ns](const XBusMessage& r) {
                    // The ack carries a status: 0 = mount started (the list
                    // refreshes on ShareChanged when it finishes), else errno.
                    uint32_t st = 0;
                    std::memcpy(&st, r.payload_, sizeof(st));
                    if (st != 0) {
                        ns_status_lbl->setText(
                            st == EPERM   ? "Needs sysctl vfs.usermount=1" :
                            st == ENOTDIR ? "Mount point must be your own folder" :
                            st == ENOTSUP ? "Put SMB passwords in ~/.nsmbrc" :
                                            "Check host / share / user");
                        ns_status_lbl->setColor({0.95f, 0.40f, 0.40f, 1.0f});
                        return;
                    }
                    ns_status_lbl->setText("Mounting\xe2\x80\xa6");
                    ns_status_lbl->setColor({0.30f, 0.85f, 0.45f, 1.0f});
                    ns_form_col->setVisibility(false);
                    if (rebuild_ns && *rebuild_ns) (*rebuild_ns)();
                });
        });
    ns_mount_btn->setBackgroundColor({0.20f, 0.50f, 0.95f, 1.0f});
    ns_mount_btn->setFixedSize(72, 28);

    // Toggle button for mount form
    auto ns_form_visible = std::make_shared<bool>(false);
    ctx.ns_toggle_btn = std::make_shared<ButtonWidget>("\xe2\x9e\x95 Mount",
        [ns_form_col = ctx.ns_form_col, ns_status_lbl = ctx.ns_status_lbl, ns_form_visible]() {
            *ns_form_visible = !*ns_form_visible;
            ns_form_col->setVisibility(*ns_form_visible);
            if (!*ns_form_visible) ns_status_lbl->setText("");
        });
    ctx.ns_toggle_btn->setFixedSize(76, 26);

    // Populate mount form layout
    ctx.ns_form_col->addChild(ns_host_input);
    ctx.ns_form_col->addChild(ns_share_input);
    ctx.ns_form_col->addChild(ns_mnt_input);
    ctx.ns_form_col->addChild(ns_user_input);
    ctx.ns_form_col->addChild(ns_pass_input);
    ctx.ns_form_col->addChild(std::make_shared<Spacer>(1, 4));
    ctx.ns_form_col->addChild(std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        ns_proto_smb_btn, std::make_shared<Spacer>(6, 1), ns_proto_nfs_btn,
        std::make_shared<Spacer>(1, 1, true), ns_mount_btn,
    }));

    // Paged share list fetcher
    struct NsFetcher {
        std::shared_ptr<FlexColumn>  col;
        std::shared_ptr<TextWidget>  count_lbl;
        std::weak_ptr<std::function<void()>> rns_weak;
        std::shared_ptr<std::function<void(uint32_t)>> fetch;
    };
    auto nspf      = std::make_shared<NsFetcher>();
    nspf->col      = ctx.ns_col;
    nspf->count_lbl = ctx.nsm_count_lbl;
    nspf->rns_weak  = ctx.rebuild_ns;
    auto nspf_weak  = std::weak_ptr<NsFetcher>(nspf);

    nspf->fetch = std::make_shared<std::function<void(uint32_t)>>(
        [nspf_weak](uint32_t pg) {
            auto pf = nspf_weak.lock(); if (!pf || !g_shell_xbus) return;
            std::vector<uint8_t> payload(4);
            std::memcpy(payload.data(), &pg, 4);
            g_shell_xbus->Call("com.freebsdx.NetworkShare",
                static_cast<uint32_t>(NetworkShareMountOpcode::ListShares), payload,
                [nspf_weak](const XBusMessage& resp) {
                    auto pf = nspf_weak.lock(); if (!pf) return;
                    NetworkShareMountPage p{};
                    std::memcpy(&p, resp.payload_, sizeof(p));
                    if (p.page_ == 0) {
                        pf->col->clearChildren();
                        if (p.total_ == 0) {
                            auto e = std::make_shared<TextWidget>("No shares configured", 11.0f);
                            e->setColor({0.55f, 0.55f, 0.58f, 1.0f});
                            pf->col->addChild(e);
                        }
                        pf->count_lbl->setText(p.total_ > 0
                            ? std::format("{} share{}", p.total_, p.total_ == 1 ? "" : "s")
                            : "");
                    }
                    if (p.valid_ == 0) return;
                    // Protocol badge
                    auto proto_lbl = std::make_shared<TextWidget>(
                        p.protocol_ == 0 ? "SMB" : "NFS", 10.0f);
                    proto_lbl->setColor(p.protocol_ == 0
                        ? Color{0.30f, 0.70f, 1.00f, 1.0f}
                        : Color{0.70f, 0.50f, 1.00f, 1.0f});
                    proto_lbl->setFixedSize(34, 0);
                    // Path: host/share (truncated)
                    std::string path = std::string(p.host_) + std::string(p.share_);
                    if (path.size() > 28) path = path.substr(0, 27) + "\xe2\x80\xa6";
                    auto path_lbl = std::make_shared<TextWidget>(path, 11.0f);
                    path_lbl->setFlexGrow(true);
                    // Mount state dot
                    auto dot_lbl = std::make_shared<TextWidget>(
                        p.mounted_ ? "\xf0\x9f\x9f\xa2" : "\xe2\x9a\xab", 11.0f);
                    // Action button
                    uint32_t sid = p.share_id_;
                    uint32_t mounted = p.mounted_;
                    auto act_btn = mounted
                        ? std::make_shared<ButtonWidget>("Unmount",
                            [sid, nspf_weak]() {
                                auto pf = nspf_weak.lock(); if (!pf || !g_shell_xbus) return;
                                NetworkShareRequest req{}; req.share_id_ = sid;
                                std::vector<uint8_t> pl(sizeof(req));
                                std::memcpy(pl.data(), &req, sizeof(req));
                                g_shell_xbus->Call("com.freebsdx.NetworkShare",
                                    static_cast<uint32_t>(NetworkShareMountOpcode::Unmount), pl,
                                    [nspf_weak](const XBusMessage&) {
                                        auto p2 = nspf_weak.lock(); if (!p2) return;
                                        auto rns = p2->rns_weak.lock();
                                        if (rns && *rns) (*rns)();
                                    });
                            })
                        : std::make_shared<ButtonWidget>("Mount",
                            [nspf_weak]() {
                                auto p2 = nspf_weak.lock(); if (!p2) return;
                                auto rns = p2->rns_weak.lock();
                                if (rns && *rns) (*rns)();
                            });
                    act_btn->setBackgroundColor(mounted
                        ? Color{0.85f, 0.25f, 0.25f, 1.0f}
                        : Color{0.20f, 0.50f, 0.95f, 1.0f});
                    act_btn->setFixedSize(72, 22);

                    auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                        proto_lbl, std::make_shared<Spacer>(4, 1),
                        path_lbl,
                        dot_lbl, std::make_shared<Spacer>(4, 1),
                        act_btn,
                    });
                    pf->col->addChild(row);
                    if (p.has_more_ && pf->fetch) (*pf->fetch)(p.page_ + 1);
                });
        });

    // nspf is captured STRONG here so it lives as long as ctx.rebuild_ns (program
    // lifetime), matching the original block-scope local.
    *ctx.rebuild_ns = [ns_col = ctx.ns_col, nspf]() {
        ns_col->clearChildren();
        if (nspf && nspf->fetch) (*nspf->fetch)(0);
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

// Remote Desktop (in-rebuild fetcher + connect form). Header + count label +
// result column + a collapsible connect form (host/user/pass inputs + RDP/VNC
// protocol toggle + Connect button, all held by rdp_form_col) + a toggle button.
// rebuild_rdp builds a local RdpFetcher that pages com.freebsdx.RemoteDesktop
// ListSessions (per-session protocol/direction badge + host/user + state dot +
// Disconnect). Only header/count/col/form-col/status/toggle-btn + rebuild_rdp
// escape; the form widgets stay builder-local. rdp_status_lbl also escapes (it is
// captured by broadcast/reply/timer). Refreshed on loop_tick%30. Behaviour
// identical to the former inline block.
void buildRemoteDesktopPanel(ShellContext& ctx) {
    ctx.rdp_hdr_lbl    = std::make_shared<TextWidget>("\xf0\x9f\x96\xa5 Remote Desktop", 14.0f);
    ctx.rdp_count_lbl  = std::make_shared<TextWidget>("", 11.0f);
    ctx.rdp_count_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
    ctx.rdp_col        = std::make_shared<FlexColumn>();
    ctx.rdp_col->setGap(5);

    // Connect form (collapsed by default)
    ctx.rdp_form_col   = std::make_shared<FlexColumn>();
    ctx.rdp_form_col->setGap(5);
    ctx.rdp_form_col->setVisibility(false);
    auto rdp_host_input = std::make_shared<TextInputWidget>();
    rdp_host_input->setPlaceholder("Host or IP\xe2\x80\xa6");
    rdp_host_input->setFlexGrow(1.0f);
    auto rdp_user_input = std::make_shared<TextInputWidget>();
    rdp_user_input->setPlaceholder("Username\xe2\x80\xa6");
    rdp_user_input->setFlexGrow(1.0f);
    auto rdp_pass_input = std::make_shared<TextInputWidget>();
    rdp_pass_input->setPlaceholder("Password\xe2\x80\xa6");
    /* password mode unsupported */
    rdp_pass_input->setFlexGrow(1.0f);
    auto rdp_proto_cur  = std::make_shared<uint32_t>(0u); // 0=RDP, 1=VNC
    auto rdp_rdp_btn    = std::make_shared<ButtonWidget>("RDP", [](){});
    auto rdp_vnc_btn    = std::make_shared<ButtonWidget>("VNC", [](){});
    rdp_rdp_btn->setFixedSize(50, 24);
    rdp_vnc_btn->setFixedSize(50, 24);
    rdp_rdp_btn->setBackgroundColor({0.25f, 0.45f, 0.70f, 1.0f});
    rdp_vnc_btn->setBackgroundColor({0.18f, 0.20f, 0.28f, 1.0f});

    auto rdp_proto_hl = [rdp_rdp_btn, rdp_vnc_btn, rdp_proto_cur]() {
        rdp_rdp_btn->setBackgroundColor(*rdp_proto_cur == 0
            ? Color{0.25f, 0.45f, 0.70f, 1.0f}
            : Color{0.18f, 0.20f, 0.28f, 1.0f});
        rdp_vnc_btn->setBackgroundColor(*rdp_proto_cur == 1
            ? Color{0.25f, 0.45f, 0.70f, 1.0f}
            : Color{0.18f, 0.20f, 0.28f, 1.0f});
    };
    rdp_rdp_btn->onClick([rdp_proto_cur, rdp_proto_hl]() {
        *rdp_proto_cur = 0; rdp_proto_hl();
    });
    rdp_vnc_btn->onClick([rdp_proto_cur, rdp_proto_hl]() {
        *rdp_proto_cur = 1; rdp_proto_hl();
    });

    ctx.rdp_status_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.rdp_status_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

    auto rdp_connect_btn = std::make_shared<ButtonWidget>("Connect",
        [rdp_host_input, rdp_user_input, rdp_pass_input,
         rdp_proto_cur, rdp_status_lbl = ctx.rdp_status_lbl]() {
            if (!g_shell_xbus) return;
            RemoteDesktopConnectRequest req{};
            std::strncpy(req.host_,     rdp_host_input->getText().c_str(),
                         sizeof(req.host_)     - 1);
            std::strncpy(req.user_,     rdp_user_input->getText().c_str(),
                         sizeof(req.user_)     - 1);
            std::strncpy(req.password_, rdp_pass_input->getText().c_str(),
                         sizeof(req.password_) - 1);
            req.protocol_ = *rdp_proto_cur;
            req.port_     = 0u;
            rdp_status_lbl->setText("Connecting\xe2\x80\xa6");
            rdp_status_lbl->setColor({0.65f, 0.65f, 0.35f, 1.0f});
            std::vector<uint8_t> buf(sizeof(req));
            std::memcpy(buf.data(), &req, sizeof(req));
            g_shell_xbus->Call("com.freebsdx.RemoteDesktop",
                static_cast<uint32_t>(RemoteDesktopSessionOpcode::Connect), buf, nullptr);
            rdp_pass_input->setText("");
        });
    rdp_connect_btn->setFixedSize(76, 28);
    rdp_connect_btn->setBackgroundColor({0.20f, 0.38f, 0.55f, 1.0f});

    // Toggle button for connect form
    ctx.rdp_toggle_btn = std::make_shared<ButtonWidget>("\xe2\x9e\x95 Connect",
        [rdp_form_col = ctx.rdp_form_col]() {
            rdp_form_col->setVisibility(!rdp_form_col->isVisible());
        });
    ctx.rdp_toggle_btn->setFixedSize(88, 26);
    ctx.rdp_toggle_btn->setBackgroundColor({0.20f, 0.35f, 0.22f, 1.0f});

    ctx.rdp_form_col->addChild(std::make_shared<FlexRow>(
        std::vector<std::shared_ptr<Widget>>{rdp_host_input}));
    ctx.rdp_form_col->addChild(std::make_shared<FlexRow>(
        std::vector<std::shared_ptr<Widget>>{
            rdp_user_input, std::make_shared<Spacer>(6, 1), rdp_pass_input}));
    ctx.rdp_form_col->addChild(std::make_shared<FlexRow>(
        std::vector<std::shared_ptr<Widget>>{
            rdp_rdp_btn, std::make_shared<Spacer>(6, 1), rdp_vnc_btn,
            std::make_shared<Spacer>(1, 1, true), rdp_connect_btn}));
    ctx.rdp_form_col->addChild(ctx.rdp_status_lbl);

    ctx.rebuild_rdp = std::make_shared<std::function<void()>>();
    *ctx.rebuild_rdp = [rdp_col = ctx.rdp_col, rdp_count_lbl = ctx.rdp_count_lbl,
                    rrp_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_rdp)]() {
        if (!g_shell_xbus) return;
        rdp_col->clearChildren();
        struct RdpFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<RdpFetcher>();
        pf->col   = rdp_col;
        pf->count = rdp_count_lbl;
        pf->fetch = [pf_weak = std::weak_ptr<RdpFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg = page;
            g_shell_xbus->Call("com.freebsdx.RemoteDesktop",
                static_cast<uint32_t>(RemoteDesktopSessionOpcode::ListSessions),
                {reinterpret_cast<const uint8_t*>(&pg),
                 reinterpret_cast<const uint8_t*>(&pg) + sizeof(pg)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    RemoteDesktopPage dp{};
                    std::memcpy(&dp, resp.payload_, sizeof(dp));
                    if (page == 0) {
                        pf->total = dp.total_;
                        pf->count->setText(dp.total_ == 0
                            ? "No sessions"
                            : std::format("{} session{}", dp.total_,
                                          dp.total_ == 1 ? "" : "s"));
                    }
                    if (dp.valid_) {
                        // Protocol + direction badge
                        const char* proto = dp.protocol_ == 0 ? "RDP" : "VNC";
                        const char* arrow = dp.direction_ == 0
                            ? "\xe2\x86\x99"  // ↙ inbound
                            : "\xe2\x86\x97"; // ↗ outbound
                        auto badge_lbl = std::make_shared<TextWidget>(
                            std::format("{}{}", proto, arrow), 10.0f);
                        badge_lbl->setColor({0.45f, 0.65f, 0.90f, 1.0f});
                        badge_lbl->setFixedSize(40, 16);

                        auto host_lbl = std::make_shared<TextWidget>(
                            std::string(dp.host_), 11.0f);
                        host_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});
                        host_lbl->setFlexGrow(1.0f);

                        auto user_lbl = std::make_shared<TextWidget>(
                            std::string(dp.user_), 10.0f);
                        user_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        // State dot
                        const char* dot = dp.state_ == 0 ? "\xf0\x9f\x9f\xa1"  // 🟡
                                        : dp.state_ == 1 ? "\xf0\x9f\x9f\xa2"  // 🟢
                                        :                  "\xe2\x97\x8f";      // ⚫
                        auto state_lbl = std::make_shared<TextWidget>(dot, 10.0f);

                        uint32_t sid = dp.session_id_;
                        auto disc_btn = std::make_shared<ButtonWidget>("Disconnect",
                            [sid]() {
                                if (!g_shell_xbus) return;
                                RemoteDesktopSessionRequest req{};
                                req.session_id_ = sid;
                                std::vector<uint8_t> buf(sizeof(req));
                                std::memcpy(buf.data(), &req, sizeof(req));
                                g_shell_xbus->Call("com.freebsdx.RemoteDesktop",
                                    static_cast<uint32_t>(
                                        RemoteDesktopSessionOpcode::Disconnect),
                                    buf, nullptr);
                            });
                        disc_btn->setFixedSize(84, 22);
                        disc_btn->setBackgroundColor({0.45f, 0.18f, 0.18f, 1.0f});

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                badge_lbl, std::make_shared<Spacer>(4, 1),
                                host_lbl,  std::make_shared<Spacer>(4, 1),
                                user_lbl,  std::make_shared<Spacer>(6, 1),
                                state_lbl, std::make_shared<Spacer>(6, 1),
                                disc_btn});
                        row->setAlignment(Alignment::Center);
                        pf->col->addChild(row);
                    }
                    if (!dp.valid_ && page == 0) {
                        auto empty = std::make_shared<TextWidget>("No active sessions", 10.0f);
                        empty->setColor({0.40f, 0.40f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                    } else if (dp.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };
}


// Input Devices (in-rebuild fetcher). Header + count label + result column;
// rebuild_indev builds a local IndevFetcher that pages com.freebsdx.InputDevice
// ListDevices (per-device type icon + name/vendor, and for pointer/touch devices a
// sensitivity slider that posts SetSensitivity). The IndevFetcher struct + the
// kTypeIcons table stay builder-local. Refreshed on loop_tick%30. Behaviour
// identical to the former inline block.
void buildInputDevicesPanel(ShellContext& ctx) {
    ctx.indev_hdr_lbl   = std::make_shared<TextWidget>("\xe2\x8c\xa8 Input Devices", 14.0f);
    ctx.indev_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.indev_count_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
    ctx.indev_col       = std::make_shared<FlexColumn>();
    ctx.indev_col->setGap(6);

    ctx.rebuild_indev = std::make_shared<std::function<void()>>();
    *ctx.rebuild_indev = [indev_col = ctx.indev_col, indev_count_lbl = ctx.indev_count_lbl,
                      rid_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_indev)]() {
        if (!g_shell_xbus) return;
        indev_col->clearChildren();
        struct IndevFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<IndevFetcher>();
        pf->col   = indev_col;
        pf->count = indev_count_lbl;
        pf->fetch = [pf_weak = std::weak_ptr<IndevFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg = page;
            g_shell_xbus->Call("com.freebsdx.InputDevice",
                static_cast<uint32_t>(InputDeviceOpcode::ListDevices),
                {reinterpret_cast<const uint8_t*>(&pg),
                 reinterpret_cast<const uint8_t*>(&pg) + sizeof(pg)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    InputDevicePage dp{};
                    std::memcpy(&dp, resp.payload_, sizeof(dp));
                    if (page == 0) {
                        pf->total = dp.total_;
                        pf->count->setText(std::format("{} device{}",
                            dp.total_, dp.total_ == 1 ? "" : "s"));
                    }
                    if (dp.valid_) {
                        static constexpr const char* kTypeIcons[] =
                            {"\xe2\x8c\xa8", "\xf0\x9f\x96\xb1",
                             "\xf0\x9f\x91\x86", "\xe2\x9c\x8f"};
                        const char* icon = (dp.type_ < 4) ? kTypeIcons[dp.type_] : "?";

                        auto icon_lbl = std::make_shared<TextWidget>(icon, 12.0f);
                        icon_lbl->setFixedSize(20, 16);
                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(dp.name_), 11.0f);
                        name_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});
                        name_lbl->setFlexGrow(1.0f);
                        auto vendor_lbl = std::make_shared<TextWidget>(
                            std::string(dp.vendor_), 10.0f);
                        vendor_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        auto name_row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                icon_lbl, std::make_shared<Spacer>(6, 1),
                                name_lbl, vendor_lbl});
                        name_row->setAlignment(Alignment::Center);
                        pf->col->addChild(name_row);

                        // Pointer and touch devices get a sensitivity slider
                        if (dp.type_ == 1 || dp.type_ == 2) {
                            uint32_t did = dp.device_id_;
                            auto sens_sl = std::make_shared<SliderWidget>(
                                static_cast<float>(dp.sensitivity_),
                                0.0f, 100.0f);
                            sens_sl->setOnChanged([did](float v) {
                                if (!g_shell_xbus) return;
                                InputSensitivityRequest req{};
                                req.device_id_    = did;
                                req.sensitivity_  = static_cast<uint32_t>(v);
                                req.accel_profile_ = 1u; // adaptive
                                std::vector<uint8_t> buf(sizeof(req));
                                std::memcpy(buf.data(), &req, sizeof(req));
                                g_shell_xbus->Call("com.freebsdx.InputDevice",
                                    static_cast<uint32_t>(
                                        InputDeviceOpcode::SetSensitivity),
                                    buf, nullptr);
                            });
                            auto sens_row = std::make_shared<FlexRow>(
                                std::vector<std::shared_ptr<Widget>>{
                                    std::make_shared<TextWidget>("Speed", 10.0f),
                                    std::make_shared<Spacer>(8, 1),
                                    sens_sl});
                            sens_row->setAlignment(Alignment::Center);
                            pf->col->addChild(sens_row);
                        }
                    }
                    if (!dp.valid_ && page == 0) {
                        auto empty = std::make_shared<TextWidget>("No devices found", 10.0f);
                        empty->setColor({0.40f, 0.40f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                    } else if (dp.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };
}

// Clipboard History (in-rebuild fetcher). Header + count label + result column + a
// Clear button; rebuild_clip builds a local ClipFetcher that pages
// com.freebsdx.ClipHistory ListEntries (per-entry truncated preview + relative
// timestamp + Paste button that posts PasteEntry). Clear wipes the service and
// resets the column/count. The ClipFetcher struct stays builder-local. Refreshed
// on loop_tick%30. Behaviour identical to the former inline block.
void buildClipboardHistoryPanel(ShellContext& ctx) {
    ctx.clip_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x93\x8b Clipboard History", 14.0f);
    ctx.clip_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.clip_count_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
    ctx.clip_col       = std::make_shared<FlexColumn>();
    ctx.clip_col->setGap(4);

    ctx.clip_clear_btn = std::make_shared<ButtonWidget>("\xf0\x9f\x97\x91 Clear",
        [clip_col = ctx.clip_col, clip_count_lbl = ctx.clip_count_lbl]() {
            if (!g_shell_xbus) return;
            g_shell_xbus->Call("com.freebsdx.ClipboardHistory",
                static_cast<uint32_t>(ClipHistoryOpcode::Clear), {}, nullptr);
            clip_col->clearChildren();
            clip_count_lbl->setText("0 entries");
        });
    ctx.clip_clear_btn->setFixedSize(72, 26);
    ctx.clip_clear_btn->setBackgroundColor({0.40f, 0.18f, 0.18f, 1.0f});

    ctx.rebuild_clip = std::make_shared<std::function<void()>>();
    *ctx.rebuild_clip = [clip_col = ctx.clip_col, clip_count_lbl = ctx.clip_count_lbl,
                     rcp_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_clip)]() {
        if (!g_shell_xbus) return;
        clip_col->clearChildren();
        struct ClipFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<ClipFetcher>();
        pf->col   = clip_col;
        pf->count = clip_count_lbl;
        pf->fetch = [pf_weak = std::weak_ptr<ClipFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg = page;
            g_shell_xbus->Call("com.freebsdx.ClipboardHistory",
                static_cast<uint32_t>(ClipHistoryOpcode::ListEntries),
                {reinterpret_cast<const uint8_t*>(&pg),
                 reinterpret_cast<const uint8_t*>(&pg) + sizeof(pg)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    ClipHistoryPageFlat pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0) {
                        pf->total = pg.total_;
                        pf->count->setText(std::format("{} entr{}",
                            pg.total_, pg.total_ == 1 ? "y" : "ies"));
                    }
                    if (pg.valid_) {
                        std::string preview(pg.preview_);
                        if (preview.size() > 36)
                            preview = preview.substr(0, 35) + "\xe2\x80\xa6";
                        auto prev_lbl = std::make_shared<TextWidget>(preview, 10.0f);
                        prev_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});
                        prev_lbl->setFlexGrow(1.0f);

                        // Timestamp: relative label
                        uint32_t age = static_cast<uint32_t>(std::time(nullptr))
                                       - pg.timestamp_;
                        std::string ts = age < 60    ? "just now"
                                       : age < 3600  ? std::format("{}m ago", age / 60)
                                       : age < 86400 ? std::format("{}h ago", age / 3600)
                                       :               std::format("{}d ago", age / 86400);
                        auto ts_lbl = std::make_shared<TextWidget>(ts, 10.0f);
                        ts_lbl->setColor({0.40f, 0.40f, 0.45f, 1.0f});

                        uint32_t idx = pg.index_;
                        auto paste_btn = std::make_shared<ButtonWidget>("Paste",
                            [idx]() {
                                if (!g_shell_xbus) return;
                                ClipHistorySelectRequest req{};
                                req.index_ = idx;
                                std::vector<uint8_t> buf(sizeof(req));
                                std::memcpy(buf.data(), &req, sizeof(req));
                                g_shell_xbus->Call("com.freebsdx.ClipboardHistory",
                                    static_cast<uint32_t>(ClipHistoryOpcode::PasteEntry),
                                    buf, nullptr);
                            });
                        paste_btn->setFixedSize(50, 22);
                        paste_btn->setBackgroundColor({0.20f, 0.35f, 0.55f, 1.0f});

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                prev_lbl, std::make_shared<Spacer>(6, 1),
                                ts_lbl,   std::make_shared<Spacer>(6, 1),
                                paste_btn});
                        row->setAlignment(Alignment::Center);
                        pf->col->addChild(row);
                    }
                    if (!pg.valid_ && page == 0) {
                        auto empty = std::make_shared<TextWidget>("No history yet", 10.0f);
                        empty->setColor({0.40f, 0.40f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                    } else if (pg.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };
}

// Screenshot Gallery (in-rebuild fetcher). Header + count label + result column +
// a Capture button (full-screen immediate); rebuild_scap builds a local
// ScapFetcher that pages com.freebsdx.ScreenCapture ListCaptures (per-capture
// filename + dimensions + Delete, which re-pages from 0). The ScapFetcher struct
// stays builder-local. Refreshed on loop_tick%30. Behaviour identical to the
// former inline block.
void buildScreenshotGalleryPanel(ShellContext& ctx) {
    ctx.scap_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x93\xb8 Screenshots", 14.0f);
    ctx.scap_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.scap_count_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
    ctx.scap_col       = std::make_shared<FlexColumn>();
    ctx.scap_col->setGap(4);

    // Capture button: full-screen immediate
    ctx.scap_btn = std::make_shared<ButtonWidget>("\xf0\x9f\x93\xb8 Capture",
        [](){
            if (!g_shell_xbus) return;
            ScreenCaptureRequest req{};
            req.full_screen_ = 1; req.with_cursor_ = 1; req.delay_secs_ = 0;
            std::vector<uint8_t> buf(sizeof(req));
            std::memcpy(buf.data(), &req, sizeof(req));
            g_shell_xbus->Call("com.freebsdx.ScreenCapture",
                static_cast<uint32_t>(ScreenCaptureOpcode::Capture), buf, nullptr);
        });
    ctx.scap_btn->setFixedSize(88, 26);
    ctx.scap_btn->setBackgroundColor({0.20f, 0.38f, 0.55f, 1.0f});

    ctx.rebuild_scap = std::make_shared<std::function<void()>>();
    *ctx.rebuild_scap = [scap_col = ctx.scap_col, scap_count_lbl = ctx.scap_count_lbl,
                     rsc_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_scap)]() {
        if (!g_shell_xbus) return;
        scap_col->clearChildren();
        struct ScapFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<ScapFetcher>();
        pf->col   = scap_col;
        pf->count = scap_count_lbl;
        pf->fetch = [pf_weak = std::weak_ptr<ScapFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg = page;
            g_shell_xbus->Call("com.freebsdx.ScreenCapture",
                static_cast<uint32_t>(ScreenCaptureOpcode::ListCaptures),
                {reinterpret_cast<const uint8_t*>(&pg),
                 reinterpret_cast<const uint8_t*>(&pg) + sizeof(pg)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    ScreenCapturePageFlat pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0) {
                        pf->total = pg.total_;
                        pf->count->setText(std::format("{} capture{}",
                            pg.total_, pg.total_ == 1 ? "" : "s"));
                    }
                    if (pg.valid_) {
                        // Filename from path
                        std::string p(pg.path_);
                        std::string fname = p.substr(p.rfind('/') + 1);
                        if (fname.size() > 32) fname = fname.substr(0, 31) + "\xe2\x80\xa6";

                        auto name_lbl = std::make_shared<TextWidget>(fname, 10.0f);
                        name_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});
                        name_lbl->setFlexGrow(1.0f);

                        auto dim_lbl = std::make_shared<TextWidget>(
                            std::format("{}x{}", pg.width_, pg.height_), 10.0f);
                        dim_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        std::string captured_path(pg.path_);
                        auto del_btn = std::make_shared<ButtonWidget>("\xe2\x9c\x95",
                            [captured_path, pf_weak]() {
                                if (!g_shell_xbus) return;
                                ScreenCaptureDeleteRequest req{};
                                std::strncpy(req.path_, captured_path.c_str(),
                                             sizeof(req.path_) - 1);
                                std::vector<uint8_t> buf(sizeof(req));
                                std::memcpy(buf.data(), &req, sizeof(req));
                                g_shell_xbus->Call("com.freebsdx.ScreenCapture",
                                    static_cast<uint32_t>(ScreenCaptureOpcode::DeleteCapture),
                                    buf, nullptr);
                                if (auto p = pf_weak.lock()) p->fetch(0);
                            });
                        del_btn->setFixedSize(26, 22);
                        del_btn->setBackgroundColor({0.45f, 0.18f, 0.18f, 1.0f});

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl, std::make_shared<Spacer>(6, 1),
                                dim_lbl, std::make_shared<Spacer>(6, 1), del_btn});
                        row->setAlignment(Alignment::Center);
                        pf->col->addChild(row);
                    }
                    if (!pg.valid_ && page == 0) {
                        auto empty = std::make_shared<TextWidget>("No captures yet", 10.0f);
                        empty->setColor({0.40f, 0.40f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                    } else if (pg.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };
}

// Upcoming Events (in-rebuild fetcher). Header + count label + result column + a
// Today/Week/Month range filter button row (driven by a shared range index + a
// highlight closure); rebuild_uev builds a local UevFetcher that pages
// com.freebsdx.Calendar ListEvents for the current month and filters client-side
// by the selected range (time/title rows, with a day badge when range > today).
// Only header/count/col/range-index/range-buttons + rebuild_uev escape (the range
// button array is carded); the UevFetcher + highlight closure + kUevRange* tables
// stay builder-local. Behaviour identical to the former inline block.
void buildUpcomingEventsPanel(ShellContext& ctx) {
    ctx.uev_hdr_lbl    = std::make_shared<TextWidget>("\xf0\x9f\x93\x85 Upcoming Events", 14.0f);
    ctx.uev_count_lbl  = std::make_shared<TextWidget>("", 11.0f);
    ctx.uev_count_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
    ctx.uev_col        = std::make_shared<FlexColumn>();
    ctx.uev_col->setGap(5);

    // Range filter: Today / This Week / This Month
    static constexpr uint32_t kUevRangeCount = 3u;
    static constexpr const char* kUevRangeLabels[] = {"Today", "Week", "Month"};
    ctx.uev_range_cur  = std::make_shared<uint32_t>(0u);  // 0=today,1=week,2=month
    ctx.uev_range_btns = std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kUevRangeCount>>();
    for (uint32_t i = 0; i < kUevRangeCount; ++i)
        (*ctx.uev_range_btns)[i] = std::make_shared<ButtonWidget>(kUevRangeLabels[i], [](){});

    ctx.rebuild_uev = std::make_shared<std::function<void()>>();

    auto uev_range_hl = [uev_range_btns = ctx.uev_range_btns, uev_range_cur = ctx.uev_range_cur]() {
        for (uint32_t i = 0; i < kUevRangeCount; ++i) {
            bool sel = (i == *uev_range_cur);
            (*uev_range_btns)[i]->setBackgroundColor(
                sel ? Color{0.25f, 0.45f, 0.70f, 1.0f}
                    : Color{0.18f, 0.20f, 0.28f, 1.0f});
        }
    };
    uev_range_hl();

    for (uint32_t i = 0; i < kUevRangeCount; ++i) {
        (*ctx.uev_range_btns)[i]->setFixedSize(60, 24);
        (*ctx.uev_range_btns)[i]->onClick([i, uev_range_cur = ctx.uev_range_cur, uev_range_hl, rebuild_uev = ctx.rebuild_uev]() {
            *uev_range_cur = i;
            uev_range_hl();
            if (rebuild_uev && *rebuild_uev) (*rebuild_uev)();
        });
    }

    *ctx.rebuild_uev = [uev_col = ctx.uev_col, uev_count_lbl = ctx.uev_count_lbl, uev_range_cur = ctx.uev_range_cur,
                    ruev_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_uev)]() {
        if (!g_shell_xbus) return;
        uev_col->clearChildren();

        // Compute date range from today
        std::time_t now = std::time(nullptr);
        std::tm* lt = std::localtime(&now);
        if (!lt) return;
        uint16_t cur_year  = static_cast<uint16_t>(lt->tm_year + 1900);
        uint8_t  cur_month = static_cast<uint8_t>(lt->tm_mon + 1);

        // For "today" we request the current month and filter day-client-side
        // For "week"/"month" we fetch the same month (month range in 1 call)
        struct UevFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::function<void(uint32_t)> fetch;
            uint32_t range;       // 0=today,1=week,2=month
            uint8_t  today_day;
            uint8_t  today_month;
            uint16_t today_year;
            uint32_t shown{0};
        };
        auto pf = std::make_shared<UevFetcher>();
        pf->col        = uev_col;
        pf->count      = uev_count_lbl;
        pf->range      = *uev_range_cur;
        pf->today_day  = static_cast<uint8_t>(lt->tm_mday);
        pf->today_month = cur_month;
        pf->today_year  = cur_year;

        pf->fetch = [pf_weak = std::weak_ptr<UevFetcher>(pf),
                     cur_year, cur_month](uint32_t page) {
            if (!g_shell_xbus) return;
            CalendarListRequest req{};
            req.year_  = cur_year;
            req.month_ = cur_month;
            req.page_  = page;
            std::vector<uint8_t> buf(sizeof(req));
            std::memcpy(buf.data(), &req, sizeof(req));
            g_shell_xbus->Call("com.freebsdx.Calendar",
                static_cast<uint32_t>(CalendarOpcode::ListEvents), buf,
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    CalendarEventPage ep{};
                    std::memcpy(&ep, resp.payload_, sizeof(ep));
                    if (ep.valid_) {
                        // Filter by range
                        bool show = false;
                        if (pf->range == 0) {
                            show = (ep.day_ == pf->today_day);
                        } else if (pf->range == 1) {
                            // Within 7 days of today
                            show = (ep.day_ >= pf->today_day &&
                                    ep.day_ < pf->today_day + 7);
                        } else {
                            show = true;  // whole month
                        }
                        if (show) {
                            pf->shown++;
                            // Time string: HH:MM or "All day"
                            std::string time_str = ep.all_day_
                                ? "All day"
                                : std::format("{:02d}:{:02d}", ep.hour_, ep.minute_);
                            auto time_lbl = std::make_shared<TextWidget>(time_str, 10.0f);
                            time_lbl->setColor({0.45f, 0.65f, 0.90f, 1.0f});
                            time_lbl->setFixedSize(52, 16);

                            auto title_lbl = std::make_shared<TextWidget>(
                                std::string(ep.title_), 11.0f);
                            title_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});
                            title_lbl->setFlexGrow(1.0f);

                            // Day badge when range > today
                            auto row_widgets = std::vector<std::shared_ptr<Widget>>{
                                time_lbl, std::make_shared<Spacer>(6, 1), title_lbl};
                            if (pf->range > 0) {
                                auto day_lbl = std::make_shared<TextWidget>(
                                    std::format("{:02d}", ep.day_), 10.0f);
                                day_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
                                day_lbl->setFixedSize(20, 16);
                                row_widgets.insert(row_widgets.begin(), day_lbl);
                                row_widgets.insert(row_widgets.begin() + 1,
                                    std::make_shared<Spacer>(4, 1));
                            }
                            auto ev_row = std::make_shared<FlexRow>(row_widgets);
                            ev_row->setAlignment(Alignment::Center);
                            pf->col->addChild(ev_row);
                        }
                    }
                    if (!ep.valid_ && ep.page_ == 0) {
                        // No events at all
                        auto no_ev = std::make_shared<TextWidget>("No events", 10.0f);
                        no_ev->setColor({0.45f, 0.45f, 0.50f, 1.0f});
                        pf->col->addChild(no_ev);
                        pf->count->setText("0 events");
                    } else if (!ep.valid_ || (ep.page_ + 1 >= ep.total_ && ep.total_ > 0)) {
                        // Done
                        pf->count->setText(std::format("{} event{}",
                            pf->shown, pf->shown == 1 ? "" : "s"));
                    } else {
                        // More pages
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };
}


// AC stub sections: App Volume, Screen Time, DND-priority. Each is just a header
// label + result column + an empty rebuild_X shared_ptr; the rebuild bodies are
// assigned later in OnConnect (via the aliases in main()). Behaviour identical to
// the former inline stubs.
void buildAcStubSections(ShellContext& ctx) {
    ctx.audio_vol_hdr_lbl = std::make_shared<TextWidget>("🔊 App Volume", 14.0f);
    ctx.audio_vol_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.audio_vol_col = std::make_shared<FlexColumn>();
    ctx.audio_vol_col->setGap(4);
    ctx.rebuild_audio_vol = std::make_shared<std::function<void()>>();

    // Screen time section
    ctx.screen_time_hdr_lbl = std::make_shared<TextWidget>("⏱ Screen Time — Today", 14.0f);
    ctx.screen_time_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.screen_time_col = std::make_shared<FlexColumn>();
    ctx.screen_time_col->setGap(2);
    ctx.rebuild_screen_time = std::make_shared<std::function<void()>>();

    // DND priority apps section
    ctx.dnd_exc_hdr_lbl = std::make_shared<TextWidget>("★ Priority Apps (bypass DND)", 14.0f);
    ctx.dnd_exc_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.dnd_exc_col = std::make_shared<FlexColumn>();
    ctx.dnd_exc_col->setGap(2);
    ctx.rebuild_dnd_exc = std::make_shared<std::function<void()>>();
}

// Location Services (QS toggle). A switch bound to com.freebsdx.LocationServices
// SetEnabled + a requesting-app count label + a "Last: <app>" label; update_loc_ui
// (fed by a GetState reply + broadcast + timer) refreshes all three. Behaviour
// identical to the former inline block.
void buildLocationServicesTogglePanel(ShellContext& ctx) {
    ctx.loc_switch = std::make_shared<SwitchWidget>(false);
    ctx.loc_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        LocationEnableRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.LocationServices",
            static_cast<uint32_t>(LocationOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.loc_count_lbl = std::make_shared<TextWidget>("0 apps", 11.0f);
    ctx.loc_count_lbl->setColor({0.6f, 0.6f, 0.6f, 1.0f});
    ctx.loc_last_lbl  = std::make_shared<TextWidget>("", 10.0f);
    ctx.loc_last_lbl->setColor({0.55f, 0.75f, 0.95f, 1.0f});
    ctx.update_loc_ui = [loc_switch = ctx.loc_switch, loc_count_lbl = ctx.loc_count_lbl,
                         loc_last_lbl = ctx.loc_last_lbl](const LocationState& st) {
        loc_switch->setValue(st.enabled_ != 0);
        loc_count_lbl->setText(std::format("{} app{}",
            st.requesting_count_, st.requesting_count_ != 1 ? "s" : ""));
        if (st.last_app_[0])
            loc_last_lbl->setText(std::string("Last: ") + st.last_app_);
        else
            loc_last_lbl->setText("");
    };
}

// Audio Output sink selector. A "current sink" name label + a pills row; rebuild_aout
// pages com.freebsdx.AudioOutput ListSinks and builds one pill button per sink
// (active one highlighted; clicking sets the active sink then re-triggers the
// rebuild). The per-sink pill buttons are built inside the rebuild. Behaviour
// identical to the former inline block.
void buildAudioOutputPanel(ShellContext& ctx) {
    ctx.aout_name_lbl  = std::make_shared<TextWidget>("—", 11.0f);
    ctx.aout_name_lbl->setColor({0.65f, 0.65f, 0.70f, 1.0f});
    ctx.aout_pills_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{});

    ctx.rebuild_aout = std::make_shared<std::function<void()>>();
    *ctx.rebuild_aout = [aout_name_lbl = ctx.aout_name_lbl, aout_pills_row = ctx.aout_pills_row,
                     ra_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_aout)]() {
        if (!g_shell_xbus) return;
        aout_pills_row->clearChildren();
        g_shell_xbus->Call("com.freebsdx.AudioOutput",
            static_cast<uint32_t>(AudioOutputOpcode::ListSinks),
            {reinterpret_cast<const uint8_t*>((const uint32_t[]){0}), sizeof(uint32_t)},
            [aout_name_lbl, aout_pills_row, ra_weak](const XBusMessage& r) {
                AudioSinkPage sp{};
                std::memcpy(&sp, r.payload_, sizeof(sp));
                for (uint32_t i = 0; i < sp.count_; ++i) {
                    const auto& e = sp.entries_[i];
                    if (e.active_) aout_name_lbl->setText(std::string(e.name_));
                    auto btn = std::make_shared<ButtonWidget>(std::string(e.name_));
                    btn->setFontSize(10.0f);
                    btn->setBackgroundColor(e.active_
                        ? Color{0.25f, 0.45f, 0.72f, 1.0f}
                        : Color{0.22f, 0.22f, 0.26f, 1.0f});
                    char sink_id[12]{};
                    std::memcpy(sink_id, e.id_, sizeof(sink_id));
                    btn->onClick([sink_id, ra_weak]() {
                        if (!g_shell_xbus) return;
                        AudioSinkSelectRequest req{};
                        std::memcpy(req.id_, sink_id, sizeof(sink_id));
                        g_shell_xbus->Call("com.freebsdx.AudioOutput",
                            static_cast<uint32_t>(AudioOutputOpcode::SetActiveSink),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                            [ra_weak](const XBusMessage&) {
                                if (auto ra = ra_weak.lock()) if (*ra) (*ra)();
                            });
                    });
                    if (!aout_pills_row->children().empty())
                        aout_pills_row->addChild(std::make_shared<Spacer>(4, 1));
                    aout_pills_row->addChild(btn);
                }
            });
    };
}

// NTP / Time Sync toggle. A switch bound to com.freebsdx.NTP SetEnabled + a status
// label (Synced/drift/Unsynced/Disabled) + a server label; update_ntp_ui (fed by a
// GetState reply + broadcast + timer) refreshes them. Behaviour identical to the
// former inline block.
void buildNtpTogglePanel(ShellContext& ctx) {
    ctx.ntp_switch     = std::make_shared<SwitchWidget>(true);
    ctx.ntp_status_lbl = std::make_shared<TextWidget>("Synced", 11.0f);
    ctx.ntp_status_lbl->setColor({0.35f, 0.80f, 0.45f, 1.0f});
    ctx.ntp_server_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.ntp_server_lbl->setColor({0.45f, 0.65f, 0.90f, 1.0f});

    ctx.update_ntp_ui = [ntp_switch = ctx.ntp_switch, ntp_status_lbl = ctx.ntp_status_lbl,
                         ntp_server_lbl = ctx.ntp_server_lbl](const NtpState& st) {
        ntp_switch->setValue(st.enabled_ != 0);
        if (!st.enabled_) {
            ntp_status_lbl->setText("Disabled");
            ntp_status_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
            ntp_server_lbl->setText("");
            return;
        }
        if (st.synced_) {
            std::string drift = st.drift_ms_ == 0
                ? "Synced"
                : std::format("Synced {:+}ms", st.drift_ms_);
            ntp_status_lbl->setText(drift);
            ntp_status_lbl->setColor({0.35f, 0.80f, 0.45f, 1.0f});
        } else {
            ntp_status_lbl->setText("Unsynced");
            ntp_status_lbl->setColor({0.95f, 0.72f, 0.20f, 1.0f});
        }
        ntp_server_lbl->setText(st.server_[0] ? std::string(st.server_) : "");
    };

    ctx.ntp_switch->setOnChanged([ntp_status_lbl = ctx.ntp_status_lbl, ntp_server_lbl = ctx.ntp_server_lbl](bool on) {
        ntp_status_lbl->setText(on ? "Starting…" : "Disabled");
        ntp_status_lbl->setColor(on
            ? Color{0.95f, 0.72f, 0.20f, 1.0f}
            : Color{0.50f, 0.50f, 0.55f, 1.0f});
        if (!on) ntp_server_lbl->setText("");
        if (!g_shell_xbus) return;
        NtpRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.NTP",
            static_cast<uint32_t>(NtpOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
}

// Mouse & Pointer. A speed slider (1–10) + value label + natural-scroll/
// acceleration/tap-to-click switches + a status label; a builder-local send_ptr
// helper posts the full PointerConfig to com.freebsdx.Pointer SetConfig on any
// change, and update_ptr_ui (fed by a GetConfig reply + broadcast + timer)
// reflects incoming state. The ptr_speed_cur state and send_ptr helper stay
// builder-local (kept alive by the escaping update_ptr_ui + the widgets' change
// handlers). Behaviour identical to the former inline block.
void buildMousePointerPanel(ShellContext& ctx) {
    auto ptr_speed_cur    = std::make_shared<uint32_t>(5u);
    ctx.ptr_speed_lbl    = std::make_shared<TextWidget>("5", 11.0f);
    ctx.ptr_speed_lbl->setColor({0.65f, 0.65f, 0.70f, 1.0f});
    ctx.ptr_speed_slider = std::make_shared<SliderWidget>(5.0f, 1.0f, 10.0f);
    ctx.ptr_speed_slider->setFixedSize(140, 18);
    ctx.ptr_natural_sw   = std::make_shared<SwitchWidget>(false);
    ctx.ptr_accel_sw     = std::make_shared<SwitchWidget>(true);
    ctx.ptr_tap_sw       = std::make_shared<SwitchWidget>(true);
    ctx.ptr_status_lbl   = std::make_shared<TextWidget>("", 10.0f);
    ctx.ptr_status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});

    auto send_ptr = [ptr_speed_cur, ptr_natural_sw = ctx.ptr_natural_sw, ptr_accel_sw = ctx.ptr_accel_sw,
                     ptr_tap_sw = ctx.ptr_tap_sw, ptr_status_lbl = ctx.ptr_status_lbl]() {
        if (!g_shell_xbus) return;
        PointerConfig cfg{};
        cfg.speed_         = *ptr_speed_cur;
        cfg.natural_scroll_ = ptr_natural_sw->value() ? 1u : 0u;
        cfg.acceleration_  = ptr_accel_sw->value()   ? 1u : 0u;
        cfg.tap_to_click_  = ptr_tap_sw->value()     ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.Pointer",
            static_cast<uint32_t>(PointerOpcode::SetConfig),
            {reinterpret_cast<const uint8_t*>(&cfg), sizeof(cfg)},
            [ptr_status_lbl](const XBusMessage&) {
                ptr_status_lbl->setText("Saved \xe2\x9c\x93");
            });
    };

    ctx.ptr_speed_slider->setOnChanged([ptr_speed_cur, ptr_speed_lbl = ctx.ptr_speed_lbl, send_ptr](float v) {
        *ptr_speed_cur = static_cast<uint32_t>(std::clamp(static_cast<int>(v + 0.5f), 1, 10));
        ptr_speed_lbl->setText(std::format("{}", *ptr_speed_cur));
        send_ptr();
    });
    ctx.ptr_natural_sw->setOnChanged([send_ptr](bool) { send_ptr(); });
    ctx.ptr_accel_sw->setOnChanged([send_ptr](bool)   { send_ptr(); });
    ctx.ptr_tap_sw->setOnChanged([send_ptr](bool)      { send_ptr(); });

    ctx.update_ptr_ui = [ptr_speed_cur, ptr_speed_lbl = ctx.ptr_speed_lbl, ptr_speed_slider = ctx.ptr_speed_slider,
                         ptr_natural_sw = ctx.ptr_natural_sw, ptr_accel_sw = ctx.ptr_accel_sw,
                         ptr_tap_sw = ctx.ptr_tap_sw](const PointerConfig& cfg) {
        uint32_t spd = std::clamp(cfg.speed_, 1u, 10u);
        *ptr_speed_cur = spd;
        ptr_speed_lbl->setText(std::format("{}", spd));
        ptr_speed_slider->setValue(static_cast<float>(spd));
        ptr_natural_sw->setValue(cfg.natural_scroll_ != 0);
        ptr_accel_sw->setValue(cfg.acceleration_ != 0);
        ptr_tap_sw->setValue(cfg.tap_to_click_ != 0);
    };
}


// Bluetooth device panel. A paired-device list + status label (rebuild_bt_devices
// pages com.freebsdx.Bluetooth ListDevices via a local self-recursive PageFetcher,
// one device/page, each row a name + RSSI + Connect/Disconnect) + a Scan button +
// an adapter on/off toggle (Enable/Disable buttons in bt_toggle_row + state label,
// visibility flipped by update_bt_ui). The PageFetcher stays builder-local.
// Behaviour identical to the former inline block.
void buildBluetoothDevicePanel(ShellContext& ctx) {
    ctx.bt_devices_col    = std::make_shared<FlexColumn>();
    ctx.bt_devices_col->setGap(4);
    ctx.bt_status_lbl     = std::make_shared<TextWidget>("", 11.0f);
    ctx.bt_status_lbl->setColor({0.5f, 0.5f, 0.55f, 1.0f});

    // Paged device list loader (1 device per page, same weak_ptr pattern as scan)
    ctx.rebuild_bt_devices = std::make_shared<std::function<void()>>();
    *ctx.rebuild_bt_devices = [bt_devices_col = ctx.bt_devices_col, bt_status_lbl = ctx.bt_status_lbl,
                            rbd_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_bt_devices)]() {
        if (!g_shell_xbus) return;
        bt_devices_col->clearChildren();
        bt_status_lbl->setText("Loading…");

        // Inner page fetcher declared as shared_ptr so it can self-reference
        struct PageFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   status;
            std::function<void(uint32_t)> fetch;
        };
        auto pf = std::make_shared<PageFetcher>();
        pf->col    = bt_devices_col;
        pf->status = bt_status_lbl;
        pf->fetch  = [pf_weak = std::weak_ptr<PageFetcher>(pf)](uint32_t page) mutable {
            if (!g_shell_xbus) return;
            g_shell_xbus->Call("com.freebsdx.Bluetooth",
                static_cast<uint32_t>(BluetoothOpcode::ListDevices), {},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    BluetoothDevicePage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0) {
                        pf->col->clearChildren();
                        pf->status->setText("");
                        if (pg.total_ == 0) {
                            auto empty = std::make_shared<TextWidget>("No paired devices", 11.0f);
                            empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                            pf->col->addChild(empty);
                            return;
                        }
                    }
                    if (pg.count_ > 0) {
                        const auto& dev = pg.devices_[0];
                        std::string name = dev.name_[0]
                            ? std::string(dev.name_)
                            : std::string(dev.addr_.str_);
                        bool connected = dev.connected_ != 0;

                        auto name_lbl = std::make_shared<TextWidget>(name, 12.0f);
                        name_lbl->setColor(connected
                            ? Color{0.3f, 0.9f, 0.5f, 1.0f}
                            : Color{0.78f, 0.78f, 0.82f, 1.0f});

                        std::string addr_s(dev.addr_.str_);
                        auto action_btn = std::make_shared<ButtonWidget>(
                            connected ? "Disconnect" : "Connect",
                            [addr_s, connected]() {
                                if (!g_shell_xbus) return;
                                BluetoothDeviceRequest req{};
                                std::strncpy(req.addr_.str_, addr_s.c_str(),
                                    sizeof(req.addr_.str_) - 1);
                                auto op = connected
                                    ? BluetoothOpcode::Disconnect
                                    : BluetoothOpcode::Connect;
                                g_shell_xbus->Call("com.freebsdx.Bluetooth",
                                    static_cast<uint32_t>(op),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    nullptr);
                            });
                        action_btn->setFixedSize(80, 26);
                        action_btn->setBackgroundColor(connected
                            ? Color{0.35f, 0.15f, 0.15f, 1.0f}
                            : Color{0.15f, 0.28f, 0.45f, 1.0f});

                        auto rssi_lbl = dev.rssi_ > 0
                            ? std::make_shared<TextWidget>(
                                std::format("{}%", dev.rssi_), 10.0f)
                            : std::make_shared<TextWidget>("", 10.0f);
                        rssi_lbl->setColor({0.45f, 0.45f, 0.5f, 1.0f});

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl, rssi_lbl,
                                std::make_shared<Spacer>(1, 1, true),
                                action_btn});
                        row->setGap(6);
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

    ctx.bt_scan_btn = std::make_shared<ButtonWidget>("🔍 Scan",
        [rebuild_bt_devices = ctx.rebuild_bt_devices]() {
            if (!g_shell_xbus) return;
            g_shell_xbus->Call("com.freebsdx.Bluetooth",
                static_cast<uint32_t>(BluetoothOpcode::Scan), {}, nullptr);
            // Refresh list after a short delay by re-querying after scan starts
            if (rebuild_bt_devices && *rebuild_bt_devices) (*rebuild_bt_devices)();
        });
    ctx.bt_scan_btn->setFixedSize(80, 28);

    // Bluetooth adapter on/off toggle
    ctx.bt_state_lbl = std::make_shared<TextWidget>("Off", 11.0f);
    ctx.bt_state_lbl->setColor({0.5f, 0.5f, 0.55f, 1.0f});

    ctx.bt_enable_btn = std::make_shared<ButtonWidget>("Enable", []() {
        if (!g_shell_xbus) return;
        BluetoothEnableRequest req{};
        req.enabled_ = 1;
        g_shell_xbus->Call("com.freebsdx.Bluetooth",
            static_cast<uint32_t>(BluetoothOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.bt_enable_btn->setFixedSize(72, 28);
    ctx.bt_enable_btn->setBackgroundColor({0.18f, 0.38f, 0.22f, 1.0f});

    ctx.bt_disable_btn = std::make_shared<ButtonWidget>("Disable", []() {
        if (!g_shell_xbus) return;
        BluetoothEnableRequest req{};
        req.enabled_ = 0;
        g_shell_xbus->Call("com.freebsdx.Bluetooth",
            static_cast<uint32_t>(BluetoothOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.bt_disable_btn->setFixedSize(72, 28);
    ctx.bt_disable_btn->setBackgroundColor({0.35f, 0.15f, 0.15f, 1.0f});
    ctx.bt_disable_btn->setVisibility(false);

    ctx.bt_toggle_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        ctx.bt_enable_btn, std::make_shared<Spacer>(6, 1), ctx.bt_disable_btn
    });

    ctx.update_bt_ui = [bt_state_lbl = ctx.bt_state_lbl, bt_enable_btn = ctx.bt_enable_btn,
                        bt_disable_btn = ctx.bt_disable_btn,
                        rebuild_bt_devices = ctx.rebuild_bt_devices](const BluetoothState& st) {
        if (st.enabled_) {
            bt_state_lbl->setText("On");
            bt_state_lbl->setColor({0.3f, 0.9f, 0.5f, 1.0f});
            bt_enable_btn->setVisibility(false);
            bt_disable_btn->setVisibility(true);
            if (rebuild_bt_devices && *rebuild_bt_devices) (*rebuild_bt_devices)();
        } else {
            bt_state_lbl->setText("Off");
            bt_state_lbl->setColor({0.5f, 0.5f, 0.55f, 1.0f});
            bt_enable_btn->setVisibility(true);
            bt_disable_btn->setVisibility(false);
        }
    };
}

// VPN quick-connect. Status + info labels + an interface input + Connect/Disconnect
// buttons (in vpn_btn_row; both capture vpn_iface_input and post Connect/Disconnect
// to com.freebsdx.VPN). update_vpn_ui (fed by a GetStatus reply + broadcast + timer)
// flips the status text/colour, info line, and button visibility. Behaviour
// identical to the former inline block.
void buildVpnPanel(ShellContext& ctx) {
    ctx.vpn_status_lbl = std::make_shared<TextWidget>("Disconnected", 11.0f);
    ctx.vpn_status_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.vpn_info_lbl   = std::make_shared<TextWidget>("", 10.0f);
    ctx.vpn_info_lbl->setColor({0.45f, 0.75f, 0.55f, 1.0f});
    ctx.vpn_iface_input = std::make_shared<TextInputWidget>();
    ctx.vpn_iface_input->setPlaceholder("Interface, e.g. wg0  (empty = auto)");

    ctx.vpn_conn_btn = std::make_shared<ButtonWidget>("Connect", [vpn_iface_input = ctx.vpn_iface_input]() {
        if (!g_shell_xbus) return;
        VpnConnectRequest req{};
        std::strncpy(req.interface_, vpn_iface_input->getText().c_str(),
            sizeof(req.interface_) - 1);
        g_shell_xbus->Call("com.freebsdx.VPN",
            static_cast<uint32_t>(VpnOpcode::Connect),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.vpn_conn_btn->setFixedSize(82, 28);
    ctx.vpn_conn_btn->setBackgroundColor({0.18f, 0.38f, 0.22f, 1.0f});

    ctx.vpn_disc_btn = std::make_shared<ButtonWidget>("Disconnect", [vpn_iface_input = ctx.vpn_iface_input]() {
        if (!g_shell_xbus) return;
        VpnConnectRequest req{};
        std::strncpy(req.interface_, vpn_iface_input->getText().c_str(),
            sizeof(req.interface_) - 1);
        g_shell_xbus->Call("com.freebsdx.VPN",
            static_cast<uint32_t>(VpnOpcode::Disconnect),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.vpn_disc_btn->setFixedSize(92, 28);
    ctx.vpn_disc_btn->setBackgroundColor({0.35f, 0.15f, 0.15f, 1.0f});
    ctx.vpn_disc_btn->setVisibility(false);

    ctx.vpn_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        ctx.vpn_conn_btn, std::make_shared<Spacer>(6, 1), ctx.vpn_disc_btn
    });

    // Helper to update VPN UI from a VpnStatus struct (called from GetStatus + broadcast)
    ctx.update_vpn_ui = [vpn_status_lbl = ctx.vpn_status_lbl, vpn_info_lbl = ctx.vpn_info_lbl,
                         vpn_conn_btn = ctx.vpn_conn_btn, vpn_disc_btn = ctx.vpn_disc_btn](
        const VpnStatus& st) {
        if (st.connected_) {
            vpn_status_lbl->setText(std::string("● ") + st.interface_);
            vpn_status_lbl->setColor({0.3f, 0.9f, 0.5f, 1.0f});
            std::string info = std::string(st.ip_address_);
            if (st.endpoint_[0]) info += "  →  " + std::string(st.endpoint_);
            vpn_info_lbl->setText(info);
            vpn_conn_btn->setVisibility(false);
            vpn_disc_btn->setVisibility(true);
        } else {
            vpn_status_lbl->setText("Disconnected");
            vpn_status_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
            vpn_info_lbl->setText("");
            vpn_conn_btn->setVisibility(true);
            vpn_disc_btn->setVisibility(false);
        }
    };
}

// Wi-Fi Hotspot config form (the SSID/passphrase inputs + Apply button that posts
// a HotspotConfig to com.freebsdx.Hotspot Configure). The Hotspot enable/disable
// tile itself is a separate inline buildEnableDisableTile call in main() whose
// extra_update pre-fills ctx.hs_ssid_input by reference — so ctx.hs_ssid_input must
// already exist (default-null) before this builder runs, and this builder assigns
// it. hs_pass_input + hs_apply_btn stay builder-local (held by hs_cfg_row1 /
// hs_cfg_row2). Behaviour identical to the former inline block.
void buildHotspotConfigPanel(ShellContext& ctx) {
    ctx.hs_ssid_input = std::make_shared<TextInputWidget>();
    ctx.hs_ssid_input->setPlaceholder("SSID");
    ctx.hs_ssid_input->setFixedSize(140, 28);
    auto hs_pass_input = std::make_shared<TextInputWidget>();
    hs_pass_input->setPlaceholder("Passphrase (blank = open)");
    hs_pass_input->setFixedSize(188, 28);

    auto hs_apply_btn = std::make_shared<ButtonWidget>("Apply",
        [hs_ssid_input = ctx.hs_ssid_input, hs_pass_input]() {
            if (!g_shell_xbus) return;
            HotspotConfig cfg{};
            std::strncpy(cfg.ssid_,       hs_ssid_input->getText().c_str(),
                sizeof(cfg.ssid_) - 1);
            std::strncpy(cfg.passphrase_, hs_pass_input->getText().c_str(),
                sizeof(cfg.passphrase_) - 1);
            g_shell_xbus->Call("com.freebsdx.Hotspot",
                static_cast<uint32_t>(HotspotOpcode::Configure),
                {reinterpret_cast<const uint8_t*>(&cfg), sizeof(cfg)}, nullptr);
        });
    hs_apply_btn->setFixedSize(58, 28);

    ctx.hs_cfg_row1 = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        ctx.hs_ssid_input, std::make_shared<Spacer>(6, 1), hs_apply_btn
    });
    ctx.hs_cfg_row2 = hs_pass_input;
}

// Screen Cast. Status + elapsed labels + a receiver list + Scan/Stop buttons (in
// cast_ctrl_row). rebuild_cast_receivers pages com.freebsdx.Cast Scan via a local
// CastFetcher (per-receiver name/signal/protocol button → Connect); Scan's onClick
// is wired after the rebuild body; Stop posts Disconnect. update_cast_ui (fed by a
// GetState reply + broadcast + timer) flips the casting/idle presentation. The
// CastFetcher stays builder-local. Behaviour identical to the former inline block.
void buildScreenCastPanel(ShellContext& ctx) {
    ctx.cast_status_lbl = std::make_shared<TextWidget>("Off", 11.0f);
    ctx.cast_status_lbl->setColor({0.5f, 0.5f, 0.55f, 1.0f});
    ctx.cast_elapsed_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.cast_elapsed_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});
    ctx.cast_elapsed_lbl->setVisibility(false);

    ctx.cast_receiver_col = std::make_shared<FlexColumn>();
    ctx.cast_receiver_col->setGap(4);

    // Scan button — triggers discovery and populates cast_receiver_col
    ctx.cast_scan_btn = std::make_shared<ButtonWidget>("⟳ Scan", [](){});
    ctx.cast_scan_btn->setFixedSize(72, 26);

    ctx.cast_stop_btn = std::make_shared<ButtonWidget>("✕ Stop", []() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.Cast",
            static_cast<uint32_t>(CastOpcode::Disconnect), {}, nullptr);
    });
    ctx.cast_stop_btn->setFixedSize(64, 28);
    ctx.cast_stop_btn->setColor({0.65f, 0.20f, 0.20f, 1.0f});
    ctx.cast_stop_btn->setVisibility(false);

    ctx.cast_ctrl_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        ctx.cast_scan_btn, std::make_shared<Spacer>(1, 1, true), ctx.cast_stop_btn
    });

    // Wire scan: clears receiver list, fetches page 0
    ctx.rebuild_cast_receivers = std::make_shared<std::function<void()>>();
    *ctx.rebuild_cast_receivers = [cast_receiver_col = ctx.cast_receiver_col,
                                rcr_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_cast_receivers)]() {
        if (!g_shell_xbus) return;
        cast_receiver_col->clearChildren();

        struct CastFetcher {
            std::shared_ptr<FlexColumn> col;
            std::function<void(uint32_t)> fetch;
        };
        auto cf = std::make_shared<CastFetcher>();
        cf->col = cast_receiver_col;
        cf->fetch = [cf_weak = std::weak_ptr<CastFetcher>(cf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.Cast",
                static_cast<uint32_t>(CastOpcode::Scan),
                {reinterpret_cast<const uint8_t*>(&pg_num),
                 reinterpret_cast<const uint8_t*>(&pg_num) + sizeof(pg_num)},
                [cf_weak, page](const XBusMessage& resp) {
                    auto cf = cf_weak.lock();
                    if (!cf) return;
                    CastReceiverPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No receivers found", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        cf->col->addChild(empty);
                        return;
                    }
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& r = pg.entries_[i];
                        std::string sig = r.signal_ >= 70 ? "▂▄▆█"
                                        : r.signal_ >= 40 ? "▂▄▆░"
                                        : r.signal_ >= 20 ? "▂▄░░"
                                        :                   "▂░░░";
                        std::string proto = r.protocol_ == 1 ? "AirPlay" : "Miracast";
                        std::string rname(r.name_);
                        auto btn = std::make_shared<ButtonWidget>(
                            rname + "  " + sig + "  " + proto,
                            [rname]() {
                                if (!g_shell_xbus) return;
                                CastConnectRequest req{};
                                std::strncpy(req.receiver_name_, rname.c_str(),
                                             sizeof(req.receiver_name_) - 1);
                                g_shell_xbus->Call("com.freebsdx.Cast",
                                    static_cast<uint32_t>(CastOpcode::Connect),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    nullptr);
                            });
                        cf->col->addChild(btn);
                    }
                    if (pg.has_more_) {
                        if (auto p = cf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        cf->fetch(0);
    };

    // Wire the scan button now that rebuild_cast_receivers is ready
    ctx.cast_scan_btn->onClick([rebuild_cast_receivers = ctx.rebuild_cast_receivers]() {
        if (rebuild_cast_receivers && *rebuild_cast_receivers)
            (*rebuild_cast_receivers)();
    });

    ctx.update_cast_ui = [cast_status_lbl = ctx.cast_status_lbl, cast_elapsed_lbl = ctx.cast_elapsed_lbl,
                          cast_stop_btn = ctx.cast_stop_btn, cast_scan_btn = ctx.cast_scan_btn,
                          cast_receiver_col = ctx.cast_receiver_col](const CastState& st) {
        if (st.casting_) {
            uint32_t m = st.elapsed_secs_ / 60;
            uint32_t s = st.elapsed_secs_ % 60;
            cast_status_lbl->setText("● " + std::string(st.receiver_name_));
            cast_status_lbl->setColor({0.30f, 0.90f, 0.50f, 1.0f});
            cast_elapsed_lbl->setText(std::format("{:02}:{:02}", m, s));
            cast_elapsed_lbl->setVisibility(true);
            cast_stop_btn->setVisibility(true);
            cast_scan_btn->setVisibility(false);
            cast_receiver_col->clearChildren();
        } else {
            cast_status_lbl->setText("Off");
            cast_status_lbl->setColor({0.5f, 0.5f, 0.55f, 1.0f});
            cast_elapsed_lbl->setVisibility(false);
            cast_stop_btn->setVisibility(false);
            cast_scan_btn->setVisibility(true);
        }
    };
}

// Remote Desktop (QS) password form. A VNC-password input + Set button (posts a
// RemoteDesktopPasswordRequest to com.freebsdx.RemoteDesktop SetPassword, then
// clears the field), packed into rd_pass_row. The pass input + Set button stay
// builder-local (held by rd_pass_row, the only escaping handle). The enable/
// disable tile and rd_btn_row remain inline in main(). Behaviour identical to the
// former inline block.
void buildRemoteDesktopPassForm(ShellContext& ctx) {
    auto rd_pass_input = std::make_shared<TextInputWidget>();
    rd_pass_input->setPlaceholder("VNC password (blank = disable)");
    rd_pass_input->setFixedSize(200, 28);

    auto rd_setpass_btn = std::make_shared<ButtonWidget>("Set", [rd_pass_input]() {
        if (!g_shell_xbus) return;
        RemoteDesktopPasswordRequest req{};
        std::strncpy(req.password_, rd_pass_input->getText().c_str(), sizeof(req.password_) - 1);
        g_shell_xbus->Call("com.freebsdx.RemoteDesktop",
            static_cast<uint32_t>(RemoteDesktopOpcode::SetPassword),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        rd_pass_input->setText("");
    });
    rd_setpass_btn->setFixedSize(38, 28);

    ctx.rd_pass_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        rd_pass_input, std::make_shared<Spacer>(6, 1), rd_setpass_btn
    });
}

// Media playback controls. Title/artist/progress labels + Prev/Play-Pause/Next
// buttons (in mp_btn_row) posting to com.freebsdx.MediaPlayer; a shared mp_playing
// flag toggles Play↔Pause and the play-button glyph. update_mp_ui (fed by the
// StateChanged broadcast + a reply + timer) refreshes labels + play glyph +
// mp_playing. The Prev/Next buttons stay builder-local (held by mp_btn_row).
// Behaviour identical to the former inline block.
void buildMediaPlaybackPanel(ShellContext& ctx) {
    ctx.mp_title_lbl    = std::make_shared<TextWidget>("Nothing playing", 12.0f);
    ctx.mp_title_lbl->setColor({0.92f, 0.92f, 0.95f, 1.0f});
    ctx.mp_artist_lbl   = std::make_shared<TextWidget>("", 10.0f);
    ctx.mp_artist_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.mp_progress_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.mp_progress_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

    ctx.mp_playing = std::make_shared<bool>(false);

    auto mp_prev_btn = std::make_shared<ButtonWidget>("⏮", []() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.MediaPlayer",
            static_cast<uint32_t>(MediaPlayerOpcode::Previous), {}, nullptr);
    });
    mp_prev_btn->setFixedSize(40, 32);

    ctx.mp_play_btn = std::make_shared<ButtonWidget>("⏯", [mp_playing = ctx.mp_playing]() {
        if (!g_shell_xbus) return;
        auto op = *mp_playing ? MediaPlayerOpcode::Pause : MediaPlayerOpcode::Play;
        g_shell_xbus->Call("com.freebsdx.MediaPlayer",
            static_cast<uint32_t>(op), {}, nullptr);
    });
    ctx.mp_play_btn->setFixedSize(48, 32);
    ctx.mp_play_btn->setBackgroundColor({0.18f, 0.22f, 0.35f, 1.0f});

    auto mp_next_btn = std::make_shared<ButtonWidget>("⏭", []() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.MediaPlayer",
            static_cast<uint32_t>(MediaPlayerOpcode::Next), {}, nullptr);
    });
    mp_next_btn->setFixedSize(40, 32);

    ctx.mp_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        mp_prev_btn, std::make_shared<Spacer>(6, 1), ctx.mp_play_btn,
        std::make_shared<Spacer>(6, 1), mp_next_btn
    });
    ctx.mp_btn_row->setAlignment(Alignment::Center);

    // Helper called from StateChanged broadcast
    ctx.update_mp_ui = [mp_title_lbl = ctx.mp_title_lbl, mp_artist_lbl = ctx.mp_artist_lbl,
                        mp_progress_lbl = ctx.mp_progress_lbl,
                        mp_play_btn = ctx.mp_play_btn, mp_playing = ctx.mp_playing](const MediaPlayerState& st) {
        *mp_playing = st.is_playing_ != 0;
        if (st.title_[0]) {
            mp_title_lbl->setText(std::string(st.title_));
        } else {
            mp_title_lbl->setText("Nothing playing");
        }
        mp_artist_lbl->setText(st.artist_[0] ? std::string(st.artist_) : "");
        mp_play_btn->setText(*mp_playing ? "⏸" : "▶");
        if (st.duration_ms_ > 0) {
            int pos_s = static_cast<int>(st.position_ms_ / 1000);
            int dur_s = static_cast<int>(st.duration_ms_ / 1000);
            int pct   = static_cast<int>(st.position_ms_ * 100u / st.duration_ms_);
            mp_progress_lbl->setText(std::format("{:d}:{:02d} / {:d}:{:02d}  ({}%)",
                pos_s / 60, pos_s % 60, dur_s / 60, dur_s % 60, pct));
        } else {
            mp_progress_lbl->setText("");
        }
    };
}

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

// Audio EQ. A status label + 5 band sliders (−12..+12 dB) with value labels + a
// row of preset pill buttons (Flat/Bass+/Vocal/Gaming/Custom). Slider changes set
// the Custom preset and post; preset buttons apply the preset's gains to the
// sliders and post; send_eq copies eq_gains + preset into an AudioEqConfig →
// com.freebsdx.AudioEq SetEq. update_eq_ui (fed by a GetEq reply + broadcast +
// timer) reflects incoming config. The eq_gains/eq_preset_cur state and the
// eq_hl_preset/send_eq helper closures + the kEqPresets/kEqPresetGains tables stay
// builder-local. Behaviour identical to the former inline block.
void buildAudioEqPanel(ShellContext& ctx) {
    static const char* kEqPresets[]   = {"Flat", "Bass+", "Vocal", "Gaming", "Custom"};
    static const std::array<int8_t[5], 5> kEqPresetGains{{
        { 0,  0,  0,  0,  0},  // Flat
        { 6,  3,  0, -1, -1},  // Bass Boost
        {-2, -1,  3,  4,  2},  // Vocal
        { 4,  2,  0,  1,  3},  // Gaming
        { 0,  0,  0,  0,  0},  // Custom (placeholder — user-defined)
    }};

    auto eq_gains      = std::make_shared<std::array<int8_t, 5>>();
    eq_gains->fill(0);
    auto eq_preset_cur = std::make_shared<uint8_t>(0u); // 0=Flat
    ctx.eq_status_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.eq_status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});

    // 5 band sliders
    for (int b = 0; b < 5; ++b) {
        ctx.eq_sliders[b] = std::make_shared<SliderWidget>(0.0f, -12.0f, 12.0f);
        ctx.eq_sliders[b]->setFixedSize(120, 18);
        ctx.eq_val_lbls[b] = std::make_shared<TextWidget>("0 dB", 10.0f);
        ctx.eq_val_lbls[b]->setColor({0.65f, 0.65f, 0.70f, 1.0f});
    }

    // Preset pill buttons
    ctx.eq_preset_btns = std::make_shared<std::vector<std::shared_ptr<ButtonWidget>>>();
    for (int p = 0; p < 5; ++p) {
        ctx.eq_preset_btns->push_back(std::make_shared<ButtonWidget>(kEqPresets[p]));
        (*ctx.eq_preset_btns)[p]->setFixedSize(56, 22);
    }

    // Highlight active preset
    auto eq_hl_preset = [eq_preset_btns = ctx.eq_preset_btns, eq_preset_cur]() {
        Color hi  = {0.25f, 0.50f, 0.80f, 0.90f};
        Color dim = {0.22f, 0.22f, 0.28f, 0.90f};
        for (size_t i = 0; i < eq_preset_btns->size(); ++i)
            (*eq_preset_btns)[i]->setBackgroundColor(
                static_cast<uint8_t>(i) == *eq_preset_cur ? hi : dim);
    };
    eq_hl_preset();

    // Send current gains to daemon
    auto send_eq = [eq_gains, eq_preset_cur, eq_status_lbl = ctx.eq_status_lbl]() {
        if (!g_shell_xbus) return;
        AudioEqConfig cfg{};
        std::copy(eq_gains->begin(), eq_gains->end(), cfg.gains_);
        cfg.preset_ = *eq_preset_cur;
        g_shell_xbus->Call("com.freebsdx.AudioEq",
            static_cast<uint32_t>(AudioEqOpcode::SetEq),
            {reinterpret_cast<const uint8_t*>(&cfg), sizeof(cfg)},
            [eq_status_lbl](const XBusMessage&) {
                eq_status_lbl->setText("Saved \xe2\x9c\x93");
            });
    };

    // Wire sliders
    for (int b = 0; b < 5; ++b) {
        ctx.eq_sliders[b]->setOnChanged(
            [b, eq_gains, eq_val_lbls = ctx.eq_val_lbls, eq_preset_cur, eq_hl_preset, send_eq](float v) {
                (*eq_gains)[b] = static_cast<int8_t>(std::clamp(static_cast<int>(v), -12, 12));
                eq_val_lbls[b]->setText(std::format("{:+d} dB", static_cast<int>(v)));
                *eq_preset_cur = 4; // Custom
                eq_hl_preset();
                send_eq();
            });
    }

    // Wire preset buttons
    for (int p = 0; p < 5; ++p) {
        (*ctx.eq_preset_btns)[p]->onClick(
            [p, eq_gains, eq_sliders = ctx.eq_sliders, eq_val_lbls = ctx.eq_val_lbls,
             eq_preset_cur, eq_hl_preset, send_eq]() {
                *eq_preset_cur = static_cast<uint8_t>(p);
                if (p < 4) { // apply preset gains (not Custom)
                    for (int b = 0; b < 5; ++b) {
                        (*eq_gains)[b] = kEqPresetGains[p][b];
                        eq_sliders[b]->setValue(static_cast<float>(kEqPresetGains[p][b]));
                        eq_val_lbls[b]->setText(
                            std::format("{:+d} dB", static_cast<int>(kEqPresetGains[p][b])));
                    }
                }
                eq_hl_preset();
                send_eq();
            });
    }

    ctx.update_eq_ui = [eq_gains, eq_sliders = ctx.eq_sliders, eq_val_lbls = ctx.eq_val_lbls,
                        eq_preset_cur, eq_hl_preset](const AudioEqConfig& cfg) {
        *eq_preset_cur = cfg.preset_ < 5 ? cfg.preset_ : 4u;
        for (int b = 0; b < 5; ++b) {
            (*eq_gains)[b] = cfg.gains_[b];
            eq_sliders[b]->setValue(static_cast<float>(cfg.gains_[b]));
            eq_val_lbls[b]->setText(std::format("{:+d} dB", static_cast<int>(cfg.gains_[b])));
        }
        eq_hl_preset();
    };
}

// Power menu (Sleep / Restart / Shut Down / Lock). Four action buttons + a confirm
// row (label + Yes/Cancel) shown for the destructive Restart/Shut-Down actions.
// Sleep→Power Suspend, Lock→Lock, Restart/Shut-Down stash the pending opcode and
// reveal the confirm row; Yes dispatches the pending opcode (Power Reboot /
// System Shutdown) and hides the row. The confirm label, pending-opcode holder,
// Yes/Cancel buttons, and the hide_confirm/make_power_btn helpers stay
// builder-local (held by power_confirm_row / captured by the action buttons).
// Behaviour identical to the former inline block.
void buildPowerMenuPanel(ShellContext& ctx) {
    // Confirm row shown when Restart or Shut Down is tapped; hidden otherwise.
    auto power_confirm_lbl = std::make_shared<TextWidget>("Are you sure?", 12.0f);
    power_confirm_lbl->setColor({1.0f, 0.85f, 0.4f, 1.0f});
    auto power_pending = std::make_shared<uint32_t>(0); // opcode to execute on confirm
    ctx.power_confirm_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        power_confirm_lbl,
        std::make_shared<Spacer>(1, 1, true),
        // "Yes" and "Cancel" wired below after power_confirm_row exists
    });
    ctx.power_confirm_row->setVisibility(false);

    // Helper: hide confirm row
    auto hide_confirm = [power_confirm_row = ctx.power_confirm_row]() {
        power_confirm_row->setVisibility(false);
    };

    // Wire Yes / Cancel into confirm row after capture is available
    auto power_yes_btn = std::make_shared<ButtonWidget>("Yes",
        [power_pending, hide_confirm]() {
            if (!g_shell_xbus) return;
            uint32_t op = *power_pending;
            hide_confirm();
            if (op == static_cast<uint32_t>(PowerOpcode::Reboot)) {
                g_shell_xbus->Call("com.freebsdx.Power", op, {}, nullptr);
            } else if (op == 0x02u /* SystemOpcode::Shutdown */) {
                g_shell_xbus->Call("com.freebsdx.System", op, {}, nullptr);
            }
        });
    power_yes_btn->setPadding(4);
    auto power_cancel_btn = std::make_shared<ButtonWidget>("Cancel", [hide_confirm]() {
        hide_confirm();
    });
    power_cancel_btn->setPadding(4);
    ctx.power_confirm_row->addChild(power_yes_btn);
    ctx.power_confirm_row->addChild(std::make_shared<Spacer>(6, 1));
    ctx.power_confirm_row->addChild(power_cancel_btn);

    auto make_power_btn = [](const std::string& label) {
        auto btn = std::make_shared<ButtonWidget>(label, [](){});
        btn->setPadding(8);
        btn->setBackgroundColor({0.16f, 0.18f, 0.26f, 1.0f});
        return btn;
    };
    ctx.power_sleep_btn    = make_power_btn("💤 Sleep");
    ctx.power_restart_btn  = make_power_btn("↺ Restart");
    ctx.power_shutdown_btn = make_power_btn("⏻ Shut Down");
    ctx.power_lock_btn     = make_power_btn("🔒 Lock");

    *ctx.power_lock_btn = ButtonWidget(ctx.power_lock_btn->child(), []() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.Lock",
            static_cast<uint32_t>(LockOpcode::Lock), {}, nullptr);
    });
    *ctx.power_sleep_btn = ButtonWidget(ctx.power_sleep_btn->child(), []() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.Power",
            static_cast<uint32_t>(PowerOpcode::Suspend), {}, nullptr);
    });
    *ctx.power_restart_btn = ButtonWidget(ctx.power_restart_btn->child(),
        [power_confirm_row = ctx.power_confirm_row, power_pending, power_confirm_lbl]() {
            *power_pending = static_cast<uint32_t>(PowerOpcode::Reboot);
            power_confirm_lbl->setText("Restart now?");
            power_confirm_row->setVisibility(true);
        });
    *ctx.power_shutdown_btn = ButtonWidget(ctx.power_shutdown_btn->child(),
        [power_confirm_row = ctx.power_confirm_row, power_pending, power_confirm_lbl]() {
            *power_pending = 0x02u; // SystemOpcode::Shutdown
            power_confirm_lbl->setText("Shut down now?");
            power_confirm_row->setVisibility(true);
        });
}

// Game Mode quick-settings tile. Pure move out of main() — creates the widgets
// into ctx and wires the enable/disable buttons + the state updater. Behaviour is
// identical to the former inline block (callbacks capture the shared_ptrs by
// value). g_shell_xbus is the process-global XBus client.
void buildGameModePanel(ShellContext& ctx) {
    ctx.gm_status_lbl = std::make_shared<TextWidget>("Off", 11.0f);
    ctx.gm_status_lbl->setColor({0.5f, 0.5f, 0.55f, 1.0f});
    ctx.gm_app_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.gm_app_lbl->setColor({0.5f, 0.75f, 0.95f, 1.0f});
    ctx.gm_app_lbl->setVisibility(false);

    ctx.gm_enable_btn = std::make_shared<ButtonWidget>("Enable", []() {
        if (!g_shell_xbus) return;
        GameModeEnableRequest req{};
        req.enabled_ = 1u;
        g_shell_xbus->Call("com.freebsdx.GameMode",
            static_cast<uint32_t>(GameModeOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.gm_enable_btn->setFixedSize(72, 28);
    ctx.gm_enable_btn->setColor({0.20f, 0.65f, 0.30f, 1.0f});

    ctx.gm_disable_btn = std::make_shared<ButtonWidget>("Disable", []() {
        if (!g_shell_xbus) return;
        GameModeEnableRequest req{};
        req.enabled_ = 0u;
        g_shell_xbus->Call("com.freebsdx.GameMode",
            static_cast<uint32_t>(GameModeOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.gm_disable_btn->setFixedSize(72, 28);
    ctx.gm_disable_btn->setColor({0.70f, 0.20f, 0.20f, 1.0f});
    ctx.gm_disable_btn->setVisibility(false);

    ctx.gm_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        ctx.gm_enable_btn, std::make_shared<Spacer>(6, 1), ctx.gm_disable_btn
    });

    ctx.update_gm_ui = [gm_status_lbl = ctx.gm_status_lbl, gm_app_lbl = ctx.gm_app_lbl,
                        gm_enable_btn = ctx.gm_enable_btn, gm_disable_btn = ctx.gm_disable_btn]
                       (const GameModeState& st) {
        if (st.enabled_) {
            uint32_t m = st.elapsed_secs_ / 60;
            uint32_t s = st.elapsed_secs_ % 60;
            std::string elapsed = m > 0
                ? std::format("{} min", m)
                : std::format("{} s",   s);
            gm_status_lbl->setText("● Active");
            gm_status_lbl->setColor({0.30f, 0.90f, 0.50f, 1.0f});
            if (st.active_app_[0]) {
                gm_app_lbl->setText(std::string(st.active_app_) + "  ·  " + elapsed);
                gm_app_lbl->setVisibility(true);
            } else {
                gm_app_lbl->setVisibility(false);
            }
            gm_enable_btn->setVisibility(false);
            gm_disable_btn->setVisibility(true);
        } else {
            gm_status_lbl->setText("Off");
            gm_status_lbl->setColor({0.5f, 0.5f, 0.55f, 1.0f});
            gm_app_lbl->setVisibility(false);
            gm_enable_btn->setVisibility(true);
            gm_disable_btn->setVisibility(false);
        }
    };
}

// Camera & Microphone privacy panel. Two identical rows (📷 / 🎤): a status
// label plus Block/Allow buttons whose visibility tracks the blocked state, and
// one updater that rewrites both rows from a PrivacyStatus broadcast. Pure move
// out of main() — behaviour identical to the former inline block (callbacks
// capture the shared_ptrs by value).
void buildPrivacyPanel(ShellContext& ctx) {
    ctx.cam_status_lbl = std::make_shared<TextWidget>("Allowed", 11.0f);
    ctx.cam_status_lbl->setColor({0.3f, 0.9f, 0.5f, 1.0f});
    ctx.cam_block_btn = std::make_shared<ButtonWidget>("Block", []() {
        if (!g_shell_xbus) return;
        PrivacyDeviceRequest req{};
        req.blocked_ = 1;
        g_shell_xbus->Call("com.freebsdx.Privacy",
            static_cast<uint32_t>(PrivacyOpcode::SetCameraBlocked),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.cam_block_btn->setFixedSize(60, 26);
    ctx.cam_block_btn->setBackgroundColor({0.42f, 0.12f, 0.12f, 1.0f});
    ctx.cam_allow_btn = std::make_shared<ButtonWidget>("Allow", []() {
        if (!g_shell_xbus) return;
        PrivacyDeviceRequest req{};
        req.blocked_ = 0;
        g_shell_xbus->Call("com.freebsdx.Privacy",
            static_cast<uint32_t>(PrivacyOpcode::SetCameraBlocked),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.cam_allow_btn->setFixedSize(60, 26);
    ctx.cam_allow_btn->setBackgroundColor({0.18f, 0.35f, 0.20f, 1.0f});
    ctx.cam_allow_btn->setVisibility(false);

    ctx.cam_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("📷", 14.0f),
        std::make_shared<Spacer>(8, 1),
        ctx.cam_status_lbl,
        std::make_shared<Spacer>(1, 1, true),
        ctx.cam_block_btn, ctx.cam_allow_btn
    });
    ctx.cam_row->setGap(6);
    ctx.cam_row->setAlignment(Alignment::Center);

    ctx.mic_status_lbl = std::make_shared<TextWidget>("Allowed", 11.0f);
    ctx.mic_status_lbl->setColor({0.3f, 0.9f, 0.5f, 1.0f});
    ctx.mic_block_btn = std::make_shared<ButtonWidget>("Block", []() {
        if (!g_shell_xbus) return;
        PrivacyDeviceRequest req{};
        req.blocked_ = 1;
        g_shell_xbus->Call("com.freebsdx.Privacy",
            static_cast<uint32_t>(PrivacyOpcode::SetMicBlocked),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.mic_block_btn->setFixedSize(60, 26);
    ctx.mic_block_btn->setBackgroundColor({0.42f, 0.12f, 0.12f, 1.0f});
    ctx.mic_allow_btn = std::make_shared<ButtonWidget>("Allow", []() {
        if (!g_shell_xbus) return;
        PrivacyDeviceRequest req{};
        req.blocked_ = 0;
        g_shell_xbus->Call("com.freebsdx.Privacy",
            static_cast<uint32_t>(PrivacyOpcode::SetMicBlocked),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.mic_allow_btn->setFixedSize(60, 26);
    ctx.mic_allow_btn->setBackgroundColor({0.18f, 0.35f, 0.20f, 1.0f});
    ctx.mic_allow_btn->setVisibility(false);

    ctx.mic_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🎤", 14.0f),
        std::make_shared<Spacer>(8, 1),
        ctx.mic_status_lbl,
        std::make_shared<Spacer>(1, 1, true),
        ctx.mic_block_btn, ctx.mic_allow_btn
    });
    ctx.mic_row->setGap(6);
    ctx.mic_row->setAlignment(Alignment::Center);

    ctx.update_privacy_ui = [cam_status_lbl = ctx.cam_status_lbl,
                             cam_block_btn = ctx.cam_block_btn, cam_allow_btn = ctx.cam_allow_btn,
                             mic_status_lbl = ctx.mic_status_lbl,
                             mic_block_btn = ctx.mic_block_btn, mic_allow_btn = ctx.mic_allow_btn](
                                const PrivacyStatus& st) {
        // Camera
        if (st.camera_blocked_) {
            cam_status_lbl->setText("Blocked");
            cam_status_lbl->setColor({0.85f, 0.35f, 0.35f, 1.0f});
            cam_block_btn->setVisibility(false);
            cam_allow_btn->setVisibility(true);
        } else {
            cam_status_lbl->setText(st.camera_app_[0]
                ? std::string("In use: ") + st.camera_app_ : "Allowed");
            cam_status_lbl->setColor(st.camera_app_[0]
                ? Color{0.95f, 0.65f, 0.20f, 1.0f}
                : Color{0.30f, 0.90f, 0.50f, 1.0f});
            cam_block_btn->setVisibility(true);
            cam_allow_btn->setVisibility(false);
        }
        // Microphone
        if (st.mic_blocked_) {
            mic_status_lbl->setText("Blocked");
            mic_status_lbl->setColor({0.85f, 0.35f, 0.35f, 1.0f});
            mic_block_btn->setVisibility(false);
            mic_allow_btn->setVisibility(true);
        } else {
            mic_status_lbl->setText(st.mic_app_[0]
                ? std::string("In use: ") + st.mic_app_ : "Allowed");
            mic_status_lbl->setColor(st.mic_app_[0]
                ? Color{0.95f, 0.65f, 0.20f, 1.0f}
                : Color{0.30f, 0.90f, 0.50f, 1.0f});
            mic_block_btn->setVisibility(true);
            mic_allow_btn->setVisibility(false);
        }
    };
}

// Wake Lock tile: a switch + three duration presets (15m / 1h / ∞) that keep
// the display awake. Buttons highlight the active preset; the updater reflects
// the daemon's remaining time. Pure move out of main().
void buildWakeLockPanel(ShellContext& ctx) {
    constexpr int kWakeLockDurCount = 3;
    static const uint32_t kWakeLockDurSecs[]   = {900, 3600, 0};   // 15m, 1h, ∞
    static const char*    kWakeLockDurLabels[] = {"15m", "1h", "\xe2\x88\x9e"};
    ctx.wl_active     = std::make_shared<bool>(false);
    ctx.wl_dur_cur    = std::make_shared<uint32_t>(0u);
    ctx.wl_dur_btns   = std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kWakeLockDurCount>>();
    ctx.wl_switch     = std::make_shared<SwitchWidget>(false);
    ctx.wl_status_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.wl_status_lbl->setColor({0.90f, 0.80f, 0.35f, 1.0f});

    auto wl_dur_hl = [wl_dur_btns = ctx.wl_dur_btns, wl_dur_cur = ctx.wl_dur_cur,
                      wl_active = ctx.wl_active]() {
        for (int i = 0; i < kWakeLockDurCount; ++i) {
            bool sel = (*wl_dur_cur == static_cast<uint32_t>(i));
            (*wl_dur_btns)[i]->setBackgroundColor(
                (*wl_active && sel) ? Color{0.22f, 0.50f, 0.88f, 1.0f}
                                    : Color{0.22f, 0.22f, 0.26f, 1.0f});
        }
    };

    for (int i = 0; i < kWakeLockDurCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kWakeLockDurLabels[i]);
        btn->setFixedSize(36, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});
        btn->onClick([i, wl_active = ctx.wl_active, wl_switch = ctx.wl_switch,
                      wl_dur_cur = ctx.wl_dur_cur, wl_dur_hl,
                      wl_status_lbl = ctx.wl_status_lbl]() {
            *wl_dur_cur = static_cast<uint32_t>(i);
            *wl_active  = true;
            wl_switch->setValue(true);
            wl_dur_hl();
            wl_status_lbl->setText(kWakeLockDurSecs[i] == 0
                ? "Always on"
                : std::format("{} remaining",
                    kWakeLockDurSecs[i] == 900 ? "15 min" : "1 h"));
            if (!g_shell_xbus) return;
            WakeLockRequest req{};
            req.active_        = 1u;
            req.duration_secs_ = kWakeLockDurSecs[i];
            g_shell_xbus->Call("com.freebsdx.WakeLock",
                static_cast<uint32_t>(WakeLockOpcode::SetLock),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
        (*ctx.wl_dur_btns)[i] = btn;
    }
    wl_dur_hl();

    ctx.wl_switch->setOnChanged([wl_active = ctx.wl_active, wl_dur_hl,
                                 wl_status_lbl = ctx.wl_status_lbl](bool on) {
        *wl_active = on;
        wl_dur_hl();
        if (!on) wl_status_lbl->setText("");
        if (!g_shell_xbus) return;
        WakeLockRequest req{};
        req.active_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.WakeLock",
            static_cast<uint32_t>(WakeLockOpcode::SetLock),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });

    ctx.update_wl_ui = [wl_active = ctx.wl_active, wl_switch = ctx.wl_switch, wl_dur_hl,
                        wl_status_lbl = ctx.wl_status_lbl](const WakeLockState& st) {
        *wl_active = st.active_ != 0;
        wl_switch->setValue(*wl_active);
        wl_dur_hl();
        if (!*wl_active) {
            wl_status_lbl->setText("");
            return;
        }
        if (st.remaining_secs_ == 0) {
            wl_status_lbl->setText("Always on");
        } else {
            uint32_t m = st.remaining_secs_ / 60;
            uint32_t s = st.remaining_secs_ % 60;
            wl_status_lbl->setText(m > 0
                ? std::format("{} min remaining", m)
                : std::format("{} sec remaining", s));
        }
    };
}

// Remote Login (SSH daemon) tile: a switch that starts/stops sshd, with an
// info line showing the port + active session count. Pure move out of main().
void buildRemoteLoginPanel(ShellContext& ctx) {
    ctx.rl_switch   = std::make_shared<SwitchWidget>(false);
    ctx.rl_info_lbl = std::make_shared<TextWidget>("Disabled", 10.0f);
    ctx.rl_info_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

    ctx.update_rl_ui = [rl_switch = ctx.rl_switch,
                        rl_info_lbl = ctx.rl_info_lbl](const RemoteLoginState& st) {
        rl_switch->setValue(st.enabled_ != 0);
        if (st.enabled_) {
            std::string s = std::format("sshd :{}", st.port_);
            if (st.sessions_ > 0)
                s += std::format(" \xc2\xb7 {} session{}", st.sessions_,
                    st.sessions_ == 1 ? "" : "s");
            rl_info_lbl->setText(s);
            rl_info_lbl->setColor({0.30f, 0.85f, 0.45f, 1.0f});
        } else {
            rl_info_lbl->setText("Disabled");
            rl_info_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
        }
    };

    ctx.rl_switch->setOnChanged([rl_info_lbl = ctx.rl_info_lbl](bool on) {
        rl_info_lbl->setText(on ? "Starting\xe2\x80\xa6" : "Stopping\xe2\x80\xa6");
        rl_info_lbl->setColor({0.90f, 0.72f, 0.20f, 1.0f});
        if (!g_shell_xbus) return;
        RemoteLoginRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.RemoteLogin",
            static_cast<uint32_t>(RemoteLoginOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
}

// Language / Locale tile: four preset buttons (EN/JA/ZH/DE) that set the system
// locale, with the active preset highlighted and a sub-label showing the full
// locale string. Pure move out of main().
void buildLocalePanel(ShellContext& ctx) {
    static constexpr uint32_t kLocalePresetCount = 4u;
    static constexpr const char* kLocalePresetLabels[] = {"EN", "JA", "ZH", "DE"};
    static constexpr const char* kLocalePresetValues[] = {
        "en_US.UTF-8", "ja_JP.UTF-8", "zh_CN.UTF-8", "de_DE.UTF-8"};
    ctx.locale_cur     = std::make_shared<std::string>("en_US.UTF-8");
    ctx.locale_btns    = std::make_shared<std::array<std::shared_ptr<ButtonWidget>,
                                                      kLocalePresetCount>>();
    ctx.locale_sub_lbl = std::make_shared<TextWidget>("en_US.UTF-8", 10.0f);
    ctx.locale_sub_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

    auto locale_hl = [locale_btns = ctx.locale_btns, locale_cur = ctx.locale_cur]() {
        for (uint32_t i = 0; i < kLocalePresetCount; ++i) {
            bool active = (*locale_cur == kLocalePresetValues[i]);
            (*locale_btns)[i]->setBackgroundColor(
                active ? Color{0.20f, 0.55f, 0.90f, 1.0f}
                       : Color{0.22f, 0.22f, 0.28f, 1.0f});
        }
    };

    for (uint32_t i = 0; i < kLocalePresetCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kLocalePresetLabels[i]);
        btn->setFixedSize(46, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.28f, 1.0f});
        btn->onClick([i, locale_cur = ctx.locale_cur, locale_hl,
                      locale_sub_lbl = ctx.locale_sub_lbl]() {
            *locale_cur = kLocalePresetValues[i];
            locale_hl();
            locale_sub_lbl->setText(kLocalePresetValues[i]);
            if (!g_shell_xbus) return;
            LocaleSetRequest req{};
            std::strncpy(req.locale_, kLocalePresetValues[i], sizeof(req.locale_) - 1);
            g_shell_xbus->Call("com.freebsdx.Locale",
                static_cast<uint32_t>(LocaleOpcode::SetLocale),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                nullptr);
        });
        (*ctx.locale_btns)[i] = btn;
    }
    locale_hl();

    ctx.update_locale_ui = [locale_cur = ctx.locale_cur, locale_hl,
                            locale_sub_lbl = ctx.locale_sub_lbl](const LocaleState& st) {
        *locale_cur = std::string(st.locale_);
        locale_hl();
        locale_sub_lbl->setText(std::string(st.locale_));
    };
}

// Accessibility quick panel: three toggle buttons (Reader / Contrast / Magnify)
// that each flip an Accessibility feature; the active ones highlight. Pure move
// out of main() — a11y_hl and a11y_send stay local closures.
void buildA11yQuickPanel(ShellContext& ctx) {
    ctx.a11y_reader_on   = std::make_shared<bool>(false);
    ctx.a11y_contrast_on = std::make_shared<bool>(false);
    ctx.a11y_magnify_on  = std::make_shared<bool>(false);

    ctx.a11y_reader_btn   = std::make_shared<ButtonWidget>("Reader");
    ctx.a11y_contrast_btn = std::make_shared<ButtonWidget>("Contrast");
    ctx.a11y_magnify_btn  = std::make_shared<ButtonWidget>("Magnify");
    for (auto& b : {ctx.a11y_reader_btn, ctx.a11y_contrast_btn, ctx.a11y_magnify_btn}) {
        b->setFixedSize(58, 26);
        b->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});
    }

    auto a11y_hl = [a11y_reader_on = ctx.a11y_reader_on, a11y_contrast_on = ctx.a11y_contrast_on,
                    a11y_magnify_on = ctx.a11y_magnify_on, a11y_reader_btn = ctx.a11y_reader_btn,
                    a11y_contrast_btn = ctx.a11y_contrast_btn,
                    a11y_magnify_btn = ctx.a11y_magnify_btn]() {
        a11y_reader_btn->setBackgroundColor(*a11y_reader_on
            ? Color{0.20f, 0.45f, 0.85f, 1.0f} : Color{0.22f, 0.22f, 0.26f, 1.0f});
        a11y_contrast_btn->setBackgroundColor(*a11y_contrast_on
            ? Color{0.20f, 0.45f, 0.85f, 1.0f} : Color{0.22f, 0.22f, 0.26f, 1.0f});
        a11y_magnify_btn->setBackgroundColor(*a11y_magnify_on
            ? Color{0.20f, 0.45f, 0.85f, 1.0f} : Color{0.22f, 0.22f, 0.26f, 1.0f});
    };

    auto a11y_send = [](uint32_t feature, uint32_t enabled) {
        if (!g_shell_xbus) return;
        A11ySetRequest req{};
        req.feature_ = feature;
        req.enabled_ = enabled;
        g_shell_xbus->Call("com.freebsdx.Accessibility",
            static_cast<uint32_t>(A11yOpcode::SetFeature),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    };

    ctx.a11y_reader_btn->onClick([a11y_reader_on = ctx.a11y_reader_on, a11y_hl, a11y_send]() {
        *a11y_reader_on = !*a11y_reader_on;
        a11y_hl();
        a11y_send(0, *a11y_reader_on ? 1u : 0u);
    });
    ctx.a11y_contrast_btn->onClick([a11y_contrast_on = ctx.a11y_contrast_on, a11y_hl, a11y_send]() {
        *a11y_contrast_on = !*a11y_contrast_on;
        a11y_hl();
        a11y_send(1, *a11y_contrast_on ? 1u : 0u);
    });
    ctx.a11y_magnify_btn->onClick([a11y_magnify_on = ctx.a11y_magnify_on, a11y_hl, a11y_send]() {
        *a11y_magnify_on = !*a11y_magnify_on;
        a11y_hl();
        a11y_send(2, *a11y_magnify_on ? 1u : 0u);
    });

    ctx.update_a11y_ui_b = [a11y_reader_on = ctx.a11y_reader_on,
                            a11y_contrast_on = ctx.a11y_contrast_on,
                            a11y_magnify_on = ctx.a11y_magnify_on, a11y_hl](const A11yState& st) {
        *a11y_reader_on   = st.reader_   != 0;
        *a11y_contrast_on = st.contrast_ != 0;
        *a11y_magnify_on  = st.magnify_  != 0;
        a11y_hl();
    };
}

// Proxy Settings tile: three mode buttons (Off / Auto / Manual) + an info line
// summarising the active proxy (PAC URL or host:port). Pure move out of main().
void buildProxyPanel(ShellContext& ctx) {
    constexpr int kProxyModeCount = 3;
    static const char* kProxyModeLabels[] = {"Off", "Auto", "Manual"};
    ctx.proxy_mode_cur  = std::make_shared<uint32_t>(0u);
    ctx.proxy_mode_btns = std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kProxyModeCount>>();
    ctx.proxy_info_lbl  = std::make_shared<TextWidget>("", 10.0f);
    ctx.proxy_info_lbl->setColor({0.55f, 0.75f, 0.55f, 1.0f});

    auto proxy_hl = [proxy_mode_btns = ctx.proxy_mode_btns, proxy_mode_cur = ctx.proxy_mode_cur]() {
        for (int i = 0; i < kProxyModeCount; ++i) {
            bool active = (*proxy_mode_cur == static_cast<uint32_t>(i));
            (*proxy_mode_btns)[i]->setBackgroundColor(
                active ? Color{0.20f, 0.45f, 0.85f, 1.0f}
                       : Color{0.22f, 0.22f, 0.26f, 1.0f});
        }
    };

    for (int i = 0; i < kProxyModeCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kProxyModeLabels[i]);
        btn->setFixedSize(52, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});
        btn->onClick([i, proxy_mode_cur = ctx.proxy_mode_cur, proxy_hl]() {
            *proxy_mode_cur = static_cast<uint32_t>(i);
            proxy_hl();
            if (!g_shell_xbus) return;
            ProxyConfig req{};
            req.mode_ = static_cast<uint32_t>(i);
            g_shell_xbus->Call("com.freebsdx.Proxy",
                static_cast<uint32_t>(ProxyOpcode::SetConfig),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
        (*ctx.proxy_mode_btns)[i] = btn;
    }
    proxy_hl();

    ctx.update_proxy_ui = [proxy_mode_cur = ctx.proxy_mode_cur, proxy_hl,
                           proxy_info_lbl = ctx.proxy_info_lbl](const ProxyConfig& st) {
        *proxy_mode_cur = std::min(st.mode_, 2u);
        proxy_hl();
        if (st.mode_ == 0) {
            proxy_info_lbl->setText("");
        } else if (st.mode_ == 1) {
            proxy_info_lbl->setText(st.pac_url_[0] ? std::string(st.pac_url_) : "WPAD");
        } else {
            if (st.http_host_[0]) {
                proxy_info_lbl->setText(
                    std::format("{}:{}", std::string(st.http_host_), st.http_port_));
            } else {
                proxy_info_lbl->setText("");
            }
        }
    };
}

// DNS Configuration tile: three mode buttons (Auto / DoH / Custom) + a resolver
// info line. Pure move out of main().
void buildDnsConfigPanel(ShellContext& ctx) {
    constexpr int kDnsModeCount = 3;
    static const char* kDnsModeLabels[] = {"Auto", "DoH", "Custom"};
    ctx.dns_mode_cur     = std::make_shared<uint32_t>(0u);
    ctx.dns_mode_btns    = std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kDnsModeCount>>();
    ctx.dns_resolver_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.dns_resolver_lbl->setColor({0.45f, 0.65f, 0.90f, 1.0f});

    auto dns_hl = [dns_mode_btns = ctx.dns_mode_btns, dns_mode_cur = ctx.dns_mode_cur]() {
        for (int i = 0; i < kDnsModeCount; ++i) {
            bool active = (*dns_mode_cur == static_cast<uint32_t>(i));
            (*dns_mode_btns)[i]->setBackgroundColor(
                active ? Color{0.20f, 0.45f, 0.85f, 1.0f}
                       : Color{0.22f, 0.22f, 0.26f, 1.0f});
        }
    };

    for (int i = 0; i < kDnsModeCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kDnsModeLabels[i]);
        btn->setFixedSize(48, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});
        btn->onClick([i, dns_mode_cur = ctx.dns_mode_cur, dns_hl]() {
            *dns_mode_cur = static_cast<uint32_t>(i);
            dns_hl();
            if (!g_shell_xbus) return;
            DnsSetRequest req{};
            req.mode_ = static_cast<uint32_t>(i);
            g_shell_xbus->Call("com.freebsdx.DNS",
                static_cast<uint32_t>(DnsOpcode::SetMode),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
        (*ctx.dns_mode_btns)[i] = btn;
    }
    dns_hl();

    ctx.update_dns_ui = [dns_mode_cur = ctx.dns_mode_cur, dns_hl,
                         dns_resolver_lbl = ctx.dns_resolver_lbl](const DnsConfig& st) {
        *dns_mode_cur = std::min(st.mode_, 2u);
        dns_hl();
        dns_resolver_lbl->setText(st.resolver_[0] ? std::string(st.resolver_) : "");
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

// Time Zone tile: ‹ / › buttons cycle a curated timezone list; labels show the
// zone name, display name, and local time. Pure move out of main() (tz_send
// stays a local closure).
void buildTimeZonePanel(ShellContext& ctx) {
    static const char* kCommonTZ[] = {
        "UTC",
        "America/New_York", "America/Chicago", "America/Denver",
        "America/Los_Angeles", "America/Vancouver", "America/Toronto",
        "America/Sao_Paulo",
        "Europe/London", "Europe/Paris", "Europe/Berlin",
        "Europe/Amsterdam", "Europe/Moscow",
        "Africa/Cairo",
        "Asia/Dubai", "Asia/Kolkata", "Asia/Bangkok",
        "Asia/Singapore", "Asia/Shanghai", "Asia/Tokyo",
        "Asia/Seoul",
        "Australia/Sydney", "Pacific/Auckland",
    };
    constexpr int kTZCount = static_cast<int>(
        sizeof(kCommonTZ) / sizeof(kCommonTZ[0]));

    ctx.tz_idx      = std::make_shared<int>(0);
    ctx.tz_name_lbl = std::make_shared<TextWidget>("UTC", 11.0f);
    ctx.tz_name_lbl->setColor({0.88f, 0.88f, 0.92f, 1.0f});
    ctx.tz_disp_lbl = std::make_shared<TextWidget>("UTC+0", 10.0f);
    ctx.tz_disp_lbl->setColor({0.45f, 0.65f, 0.90f, 1.0f});
    ctx.tz_time_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.tz_time_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

    ctx.tz_prev_btn = std::make_shared<ButtonWidget>("\xe2\x80\xb9");
    ctx.tz_next_btn = std::make_shared<ButtonWidget>("\xe2\x80\xba");
    ctx.tz_prev_btn->setFixedSize(24, 26);
    ctx.tz_next_btn->setFixedSize(24, 26);
    ctx.tz_prev_btn->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});
    ctx.tz_next_btn->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});

    auto tz_send = [tz_idx = ctx.tz_idx, tz_name_lbl = ctx.tz_name_lbl,
                    tz_disp_lbl = ctx.tz_disp_lbl]() {
        const char* tz = kCommonTZ[*tz_idx];
        tz_name_lbl->setText(std::string(tz));
        tz_disp_lbl->setText("");
        if (!g_shell_xbus) return;
        TimezoneSetRequest req{};
        std::strncpy(req.tz_name_, tz, sizeof(req.tz_name_) - 1);
        g_shell_xbus->Call("com.freebsdx.Timezone",
            static_cast<uint32_t>(TimezoneOpcode::SetTZ),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    };

    ctx.tz_prev_btn->onClick([tz_idx = ctx.tz_idx, tz_send]() {
        *tz_idx = (*tz_idx - 1 + kTZCount) % kTZCount;
        tz_send();
    });
    ctx.tz_next_btn->onClick([tz_idx = ctx.tz_idx, tz_send]() {
        *tz_idx = (*tz_idx + 1) % kTZCount;
        tz_send();
    });

    ctx.update_tz_ui = [tz_idx = ctx.tz_idx, tz_name_lbl = ctx.tz_name_lbl,
                        tz_disp_lbl = ctx.tz_disp_lbl, tz_time_lbl = ctx.tz_time_lbl]
                       (const TimezoneState& st) {
        // Find index in curated list
        for (int i = 0; i < kTZCount; ++i) {
            if (std::strcmp(kCommonTZ[i], st.tz_name_) == 0) {
                *tz_idx = i;
                break;
            }
        }
        tz_name_lbl->setText(st.tz_name_[0] ? std::string(st.tz_name_) : "UTC");
        tz_disp_lbl->setText(st.display_name_[0]
            ? std::string(st.display_name_) : "");
        tz_time_lbl->setText(st.local_time_[0]
            ? std::string(st.local_time_) : "");
    };
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

void buildBatteryHealthPanel(ShellContext& ctx) {
    ctx.bhealth_opt_sw     = std::make_shared<SwitchWidget>(false);
    ctx.bhealth_status_lbl = std::make_shared<TextWidget>("", 10.0f);
    ctx.bhealth_status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});
    ctx.bhealth_info_lbl   = std::make_shared<TextWidget>("", 10.0f);
    ctx.bhealth_info_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

    ctx.bhealth_opt_sw->setOnChanged([bhealth_opt_sw = ctx.bhealth_opt_sw,
                                      bhealth_status_lbl = ctx.bhealth_status_lbl](bool) {
        if (!g_shell_xbus) return;
        BatteryHealthRequest req{};
        req.optimized_charging_ = bhealth_opt_sw->value() ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.BatteryHealth",
            static_cast<uint32_t>(BatteryHealthOpcode::SetState),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [bhealth_status_lbl](const XBusMessage&) {
                bhealth_status_lbl->setText("Saved \xe2\x9c\x93");
            });
    });

    ctx.update_bhealth_ui = [bhealth_opt_sw = ctx.bhealth_opt_sw,
                             bhealth_info_lbl = ctx.bhealth_info_lbl](const BatteryHealthState& st) {
        bhealth_opt_sw->setValue(st.optimized_charging_ != 0);
        bhealth_info_lbl->setText(std::format("{}% health \xc2\xb7 {} cycles",
            st.health_pct_, st.cycle_count_));
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

void buildNotifSchedulePanel(ShellContext& ctx) {
    ctx.nsch_enabled_sw    = std::make_shared<SwitchWidget>(false);
    ctx.nsch_start_h_input = std::make_shared<TextInputWidget>();
    ctx.nsch_start_m_input = std::make_shared<TextInputWidget>();
    ctx.nsch_end_h_input   = std::make_shared<TextInputWidget>();
    ctx.nsch_end_m_input   = std::make_shared<TextInputWidget>();
    ctx.nsch_calls_sw      = std::make_shared<SwitchWidget>(true);
    ctx.nsch_alarms_sw     = std::make_shared<SwitchWidget>(true);
    ctx.nsch_weekend_sw    = std::make_shared<SwitchWidget>(false);
    ctx.nsch_status_lbl    = std::make_shared<TextWidget>("", 10.0f);
    ctx.nsch_status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});
    ctx.nsch_start_h_input->setPlaceholder("HH");
    ctx.nsch_start_h_input->setFixedSize(40, 28);
    ctx.nsch_start_m_input->setPlaceholder("MM");
    ctx.nsch_start_m_input->setFixedSize(40, 28);
    ctx.nsch_end_h_input->setPlaceholder("HH");
    ctx.nsch_end_h_input->setFixedSize(40, 28);
    ctx.nsch_end_m_input->setPlaceholder("MM");
    ctx.nsch_end_m_input->setFixedSize(40, 28);

    ctx.update_nsch_ui = [nsch_enabled_sw = ctx.nsch_enabled_sw,
                          nsch_start_h_input = ctx.nsch_start_h_input,
                          nsch_start_m_input = ctx.nsch_start_m_input,
                          nsch_end_h_input = ctx.nsch_end_h_input,
                          nsch_end_m_input = ctx.nsch_end_m_input,
                          nsch_calls_sw = ctx.nsch_calls_sw,
                          nsch_alarms_sw = ctx.nsch_alarms_sw,
                          nsch_weekend_sw = ctx.nsch_weekend_sw](
                         const NotifSchedule& s) {
        nsch_enabled_sw->setValue(s.enabled_ != 0);
        nsch_start_h_input->setText(std::format("{:02}", s.start_hour_));
        nsch_start_m_input->setText(std::format("{:02}", s.start_minute_));
        nsch_end_h_input->setText(std::format("{:02}", s.end_hour_));
        nsch_end_m_input->setText(std::format("{:02}", s.end_minute_));
        nsch_calls_sw->setValue(s.allow_calls_ != 0);
        nsch_alarms_sw->setValue(s.allow_alarms_ != 0);
        nsch_weekend_sw->setValue(s.weekend_only_ != 0);
    };

    auto send_nsch = [nsch_enabled_sw = ctx.nsch_enabled_sw,
                      nsch_start_h_input = ctx.nsch_start_h_input,
                      nsch_start_m_input = ctx.nsch_start_m_input,
                      nsch_end_h_input = ctx.nsch_end_h_input,
                      nsch_end_m_input = ctx.nsch_end_m_input,
                      nsch_calls_sw = ctx.nsch_calls_sw,
                      nsch_alarms_sw = ctx.nsch_alarms_sw,
                      nsch_weekend_sw = ctx.nsch_weekend_sw,
                      nsch_status_lbl = ctx.nsch_status_lbl]() {
        if (!g_shell_xbus) return;
        NotifSchedule s{};
        s.enabled_      = nsch_enabled_sw->value() ? 1u : 0u;
        s.allow_calls_  = nsch_calls_sw->value()   ? 1u : 0u;
        s.allow_alarms_ = nsch_alarms_sw->value()  ? 1u : 0u;
        s.weekend_only_ = nsch_weekend_sw->value() ? 1u : 0u;
        auto parse_field = [](TextInputWidget* w, uint32_t& out, uint32_t lo, uint32_t hi) {
            int v = std::atoi(w->getText().c_str());
            out = static_cast<uint32_t>(std::clamp(v, static_cast<int>(lo), static_cast<int>(hi)));
        };
        parse_field(nsch_start_h_input.get(), s.start_hour_,   0, 23);
        parse_field(nsch_start_m_input.get(), s.start_minute_,  0, 59);
        parse_field(nsch_end_h_input.get(),   s.end_hour_,     0, 23);
        parse_field(nsch_end_m_input.get(),   s.end_minute_,   0, 59);
        g_shell_xbus->Call("com.freebsdx.NotifSchedule",
            static_cast<uint32_t>(NotifScheduleOpcode::SetSchedule),
            {reinterpret_cast<const uint8_t*>(&s), sizeof(s)},
            [nsch_status_lbl](const XBusMessage&) {
                nsch_status_lbl->setText("Saved \xe2\x9c\x93");
            });
    };
    ctx.nsch_apply_btn = std::make_shared<ButtonWidget>("Apply",
        [send_nsch]() { send_nsch(); });
    ctx.nsch_apply_btn->setFixedSize(60, 26);
    ctx.nsch_apply_btn->setBackgroundColor({0.18f, 0.36f, 0.78f, 0.95f});
    ctx.nsch_enabled_sw->setOnChanged([send_nsch](bool) { send_nsch(); });
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

void buildKeyboardBacklightPanel(ShellContext& ctx) {
    ctx.kb_bl_switch = std::make_shared<SwitchWidget>(false);
    ctx.kb_bl_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        KeyboardBacklightEnableRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.KeyboardBacklight",
            static_cast<uint32_t>(KeyboardBacklightOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });

    ctx.kb_bl_val    = std::make_shared<TextWidget>("100%", 11.0f);
    ctx.kb_bl_val->setColor({0.72f, 0.72f, 0.72f, 1.0f});
    ctx.kb_bl_slider = std::make_shared<SliderWidget>(100.0f, 0.0f, 100.0f);
    ctx.kb_bl_slider->setOnChanged([kb_bl_val = ctx.kb_bl_val](float v) {
        kb_bl_val->setText(std::format("{}%", static_cast<int>(v)));
        if (!g_shell_xbus) return;
        KeyboardBacklightBrightnessRequest req{};
        req.brightness_ = static_cast<uint32_t>(v);
        g_shell_xbus->Call("com.freebsdx.KeyboardBacklight",
            static_cast<uint32_t>(KeyboardBacklightOpcode::SetBrightness),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ctx.kb_bl_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("⌨", 15.0f),
        std::make_shared<Spacer>(6, 1),
        ctx.kb_bl_switch,
        std::make_shared<Spacer>(8, 1),
        ctx.kb_bl_slider,
        std::make_shared<Spacer>(8, 1),
        ctx.kb_bl_val,
    });

    ctx.update_kb_bl_ui = [kb_bl_switch = ctx.kb_bl_switch, kb_bl_slider = ctx.kb_bl_slider,
                           kb_bl_val = ctx.kb_bl_val](const KeyboardBacklightState& st) {
        kb_bl_switch->setValue(st.enabled_ != 0);
        float bv = std::clamp(static_cast<float>(st.brightness_), 0.0f, 100.0f);
        kb_bl_slider->setValue(bv);
        kb_bl_val->setText(std::format("{}%", static_cast<int>(bv)));
    };
}

void buildPowerProfilePanel(ShellContext& ctx) {
    // Presentation Mode toggle
    ctx.pres_switch    = std::make_shared<SwitchWidget>(false);
    ctx.pres_status_lbl = std::make_shared<TextWidget>("Off", 11.0f);
    ctx.pres_status_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

    ctx.update_pres_ui = [pres_switch = ctx.pres_switch,
                          pres_status_lbl = ctx.pres_status_lbl](const PresentationState& st) {
        bool on = st.active_ != 0;
        pres_switch->setValue(on);
        pres_status_lbl->setText(on ? "Presenting" : "Off");
        pres_status_lbl->setColor(on
            ? Color{0.95f, 0.72f, 0.20f, 1.0f}   // amber when active
            : Color{0.50f, 0.50f, 0.55f, 1.0f}); // dim when off
    };

    ctx.pres_switch->setOnChanged([pres_status_lbl = ctx.pres_status_lbl](bool on) {
        pres_status_lbl->setText(on ? "Presenting" : "Off");
        pres_status_lbl->setColor(on
            ? Color{0.95f, 0.72f, 0.20f, 1.0f}
            : Color{0.50f, 0.50f, 0.55f, 1.0f});
        if (!g_shell_xbus) return;
        PresentationRequest req{};
        req.active_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.Presentation",
            static_cast<uint32_t>(PresentationOpcode::SetActive),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });

    ctx.pp_status_lbl = std::make_shared<TextWidget>("⚖ Balanced", 11.0f);
    ctx.pp_status_lbl->setColor({0.55f, 0.75f, 1.0f, 1.0f});

    static constexpr const char* kPpBtnNames[] =
        {"Performance", "Balanced", "Power Save", "Emergency"};
    static const Color kPpBtnColors[] = {
        {0.75f, 0.28f, 0.10f, 1.0f},  // Performance — ember-orange
        {0.18f, 0.38f, 0.22f, 1.0f},  // Balanced — green
        {0.15f, 0.25f, 0.55f, 1.0f},  // Power Save — blue
        {0.45f, 0.12f, 0.45f, 1.0f},  // Emergency — purple
    };

    std::array<std::shared_ptr<ButtonWidget>, 4> pp_btns;
    for (uint32_t pi = 0; pi < 4; ++pi) {
        pp_btns[pi] = std::make_shared<ButtonWidget>(kPpBtnNames[pi], [pi]() {
            if (!g_shell_xbus) return;
            PowerProfileRequest req{};
            req.profile_ = pi;
            g_shell_xbus->Call("com.freebsdx.PowerProfile",
                static_cast<uint32_t>(PowerProfileOpcode::SetProfile),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
        pp_btns[pi]->setFixedSize(90, 28);
        pp_btns[pi]->setBackgroundColor(kPpBtnColors[pi]);
    }
    ctx.pp_row1 = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        pp_btns[0], std::make_shared<Spacer>(6, 1), pp_btns[1]
    });
    ctx.pp_row2 = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        pp_btns[2], std::make_shared<Spacer>(6, 1), pp_btns[3]
    });

    ctx.update_pp_ui = [pp_status_lbl = ctx.pp_status_lbl](const PowerProfileState& st) {
        uint32_t p = st.profile_ < 4u ? st.profile_ : 1u;
        static constexpr const char* kPpLabels[] =
            {"⚡ Performance", "⚖ Balanced", "🔋 Power Save", "🚨 Emergency"};
        static const Color kPpStatusColors[] = {
            {1.0f,  0.55f, 0.25f, 1.0f},
            {0.55f, 0.75f, 1.0f,  1.0f},
            {0.40f, 0.65f, 1.0f,  1.0f},
            {0.85f, 0.40f, 0.85f, 1.0f},
        };
        pp_status_lbl->setText(kPpLabels[p]);
        pp_status_lbl->setColor(kPpStatusColors[p]);
    };
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

void buildFocusModePanel(ShellContext& ctx) {
    ctx.focus_status_lbl = std::make_shared<TextWidget>("Off", 11.0f);
    ctx.focus_status_lbl->setColor({0.5f, 0.5f, 0.55f, 1.0f});

    // Duration buttons: 25 min / 1 h / 4 h / ∞
    static constexpr std::array<const char*, 4> kFocusDurLabels  = {"25 m", "1 h", "4 h", "∞"};
    static constexpr std::array<uint32_t, 4>    kFocusDurSecs    = {25*60, 3600, 4*3600, 0};
    auto focus_dur_btns = std::make_shared<
        std::array<std::shared_ptr<ButtonWidget>, 4>>();
    for (int i = 0; i < 4; ++i) {
        (*focus_dur_btns)[i] = std::make_shared<ButtonWidget>(
            std::string(kFocusDurLabels[i]), []() {
                // category sent after category buttons are built; use Work(0) as default
            });
        (*focus_dur_btns)[i]->setFixedSize(50, 28);
    }
    ctx.focus_dur_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        (*focus_dur_btns)[0], std::make_shared<Spacer>(4, 1),
        (*focus_dur_btns)[1], std::make_shared<Spacer>(4, 1),
        (*focus_dur_btns)[2], std::make_shared<Spacer>(4, 1),
        (*focus_dur_btns)[3],
    });

    // Category pills: Work / Personal / All
    static constexpr std::array<const char*, 3> kCatLabels = {"Work", "Personal", "All"};
    auto focus_cat_btns = std::make_shared<
        std::array<std::shared_ptr<ButtonWidget>, 3>>();
    for (int i = 0; i < 3; ++i) {
        (*focus_cat_btns)[i] = std::make_shared<ButtonWidget>(
            std::string(kCatLabels[i]), []() {});
        (*focus_cat_btns)[i]->setFixedSize(70, 26);
    }
    // Active-category highlight helper
    auto focus_highlight_cat = [focus_cat_btns](int active) {
        for (int i = 0; i < 3; ++i) {
            (*focus_cat_btns)[i]->setBackgroundColor(
                i == active ? Color{0.30f, 0.55f, 0.90f, 1.0f}
                            : Color{0.20f, 0.20f, 0.24f, 1.0f});
        }
    };
    focus_highlight_cat(0); // default Work

    // Shared mutable state for selected category
    auto focus_cat_sel = std::make_shared<uint32_t>(0u);

    // Wire category buttons
    for (int i = 0; i < 3; ++i) {
        (*focus_cat_btns)[i]->onClick([i, focus_cat_sel, focus_highlight_cat]() {
            *focus_cat_sel = static_cast<uint32_t>(i);
            focus_highlight_cat(i);
        });
    }

    // Wire duration buttons (now focus_cat_sel is available)
    for (int i = 0; i < 4; ++i) {
        (*focus_dur_btns)[i]->onClick([i, focus_cat_sel]() {
            if (!g_shell_xbus) return;
            FocusStartRequest req{};
            req.duration_secs_ = kFocusDurSecs[i];
            req.category_      = *focus_cat_sel;
            g_shell_xbus->Call("com.freebsdx.FocusMode",
                static_cast<uint32_t>(FocusModeOpcode::Start),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
    }

    ctx.focus_end_btn = std::make_shared<ButtonWidget>("✕ End", []() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.FocusMode",
            static_cast<uint32_t>(FocusModeOpcode::Stop), {}, nullptr);
    });
    ctx.focus_end_btn->setFixedSize(64, 28);
    ctx.focus_end_btn->setColor({0.55f, 0.20f, 0.20f, 1.0f});
    ctx.focus_end_btn->setVisibility(false);

    ctx.focus_cat_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        (*focus_cat_btns)[0], std::make_shared<Spacer>(4, 1),
        (*focus_cat_btns)[1], std::make_shared<Spacer>(4, 1),
        (*focus_cat_btns)[2],
        std::make_shared<Spacer>(1, 1, true),
        ctx.focus_end_btn,
    });

    ctx.update_focus_ui = [focus_status_lbl = ctx.focus_status_lbl, focus_end_btn = ctx.focus_end_btn,
                           focus_dur_row = ctx.focus_dur_row,
                           focus_highlight_cat](const FocusModeState& st) {
        if (st.active_) {
            std::string cat_str =
                st.category_ == static_cast<uint32_t>(FocusModeCategory::Personal)
                    ? "Personal"
                : st.category_ == static_cast<uint32_t>(FocusModeCategory::All)
                    ? "All"
                : "Work";
            if (st.remaining_secs_ == 0) {
                focus_status_lbl->setText("● ∞ · " + cat_str);
            } else {
                uint32_t m = st.remaining_secs_ / 60;
                uint32_t h = m / 60; m %= 60;
                focus_status_lbl->setText(h > 0
                    ? std::format("● {}h {:02}m left · {}", h, m, cat_str)
                    : std::format("● {} min left · {}", m, cat_str));
            }
            focus_status_lbl->setColor({0.30f, 0.85f, 0.55f, 1.0f});
            focus_end_btn->setVisibility(true);
            focus_highlight_cat(static_cast<int>(st.category_));
        } else {
            focus_status_lbl->setText("Off");
            focus_status_lbl->setColor({0.5f, 0.5f, 0.55f, 1.0f});
            focus_end_btn->setVisibility(false);
        }
    };
}

void buildBatteryIndicatorPanel(ShellContext& ctx) {
    // Hidden when battery_state_ == 3 (absent / desktop with no battery).
    // Shows "🔋75%" when discharging, "⚡75%" when charging, "⚡" when full/AC.
    ctx.bat_btn = std::make_shared<ButtonWidget>("🔋", [](){});
    ctx.bat_btn->setFontSize(12.0f);
    ctx.bat_btn->setPadding(6);
    ctx.bat_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});
    ctx.bat_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});
    ctx.bat_btn->setColor(0.85f, 0.85f, 0.85f, 1.0f);
    ctx.bat_btn->setVisibility(false); // hidden until GetPowerState confirms battery present

    // Helper shared between connect-time init and broadcast handler
    ctx.update_battery = std::make_shared<std::function<void(const PowerState&)>>();
    *ctx.update_battery = [bat_btn = ctx.bat_btn](const PowerState& ps) {
        if (ps.battery_state_ == 3) {       // absent
            bat_btn->setVisibility(false);
            return;
        }
        bat_btn->setVisibility(true);
        std::string icon;
        if (ps.battery_state_ == 1)         // charging
            icon = "⚡";
        else if (ps.battery_state_ == 2)    // full / on AC
            icon = "⚡";
        else                                 // discharging
            icon = "🔋";
        bat_btn->setText(std::format("{}{:d}%", icon, ps.battery_life_));
        // Tint red below 20 %
        if (ps.battery_state_ == 0 && ps.battery_life_ <= 20)
            bat_btn->setColor(1.0f, 0.35f, 0.35f, 1.0f);
        else
            bat_btn->setColor(0.85f, 0.85f, 0.85f, 1.0f);
    };
}

void buildNotifBadgePanel(ShellContext& ctx) {
    ctx.notif_unread = std::make_shared<int>(0);
    ctx.notif_badge_lbl = std::make_shared<TextWidget>("", 9.0f);
    ctx.notif_badge_lbl->setColor({1.0f, 1.0f, 1.0f, 1.0f});
    ctx.notif_badge_box = std::make_shared<ContainerWidget>(ctx.notif_badge_lbl);
    ctx.notif_badge_box->setBackgroundColor({0.90f, 0.18f, 0.18f, 1.0f});
    ctx.notif_badge_box->setPadding(3);
    ctx.notif_badge_box->setVisibility(false);
    auto notif_bell_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🔔", 14.0f), ctx.notif_badge_box
    });
    // Callback wired after action_center is created (see below)
    ctx.notif_tbar_btn = std::make_shared<ButtonWidget>(notif_bell_row, [](){});
    ctx.notif_tbar_btn->setPadding(6);
    ctx.notif_tbar_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});
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

void buildSystemTrayPanel(ShellContext& ctx) {
    ctx.tray_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{});

    // Paged loader for tray entries
    ctx.load_tray = std::make_shared<std::function<void(uint32_t)>>();
    *ctx.load_tray = [tray_row = ctx.tray_row, load_tray = ctx.load_tray](uint32_t page) {
        if (!g_shell_xbus) return;
        TrayPageRequest req{};
        req.page_ = page;
        g_shell_xbus->Call("com.freebsdx.Tray",
            static_cast<uint32_t>(TrayOpcode::ListEntries),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [tray_row, load_tray, page](const XBusMessage& resp) {
                TrayEntryPage pg{};
                std::memcpy(&pg, resp.payload_, sizeof(pg));

                if (page == 0) tray_row->clearChildren();

                if (pg.valid_) {
                    uint32_t    id        = pg.entry_.id_;
                    std::string icon_text = pg.entry_.icon_text_[0]
                        ? std::string(pg.entry_.icon_text_)
                        : "◉";
                    std::string tooltip   = pg.entry_.tooltip_;

                    auto icon_lbl = std::make_shared<TextWidget>(icon_text, 14.0f);
                    icon_lbl->setColor({0.88f, 0.88f, 0.88f, 1.0f});

                    auto btn = std::make_shared<ButtonWidget>(icon_lbl, [id]() {
                        if (!g_shell_xbus) return;
                        TrayIdRequest cr{};
                        cr.id_ = id;
                        g_shell_xbus->Call("com.freebsdx.Tray",
                            static_cast<uint32_t>(TrayOpcode::Click),
                            {reinterpret_cast<const uint8_t*>(&cr), sizeof(cr)}, nullptr);
                    });
                    btn->setPadding(4);
                    tray_row->addChild(btn);
                    tray_row->addChild(std::make_shared<Spacer>(4, 1));

                    if (pg.has_more_) {
                        auto fn = load_tray;
                        if (fn && *fn) (*fn)(page + 1);
                    }
                }
            });
    };
}

void buildContactSearchPanel(ShellContext& ctx) {
    ctx.contact_hdr_lbl = std::make_shared<TextWidget>("👤 Contacts", 14.0f);
    ctx.contact_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.contact_search_input = std::make_shared<TextInputWidget>();
    ctx.contact_search_input->setPlaceholder("Search by name, phone, email…");
    ctx.contact_results_col = std::make_shared<FlexColumn>();
    ctx.contact_results_col->setGap(3);
    auto contact_empty_lbl = std::make_shared<TextWidget>("Type to search contacts", 11.0f);
    contact_empty_lbl->setColor({0.4f, 0.4f, 0.45f, 1.0f});
    ctx.contact_results_col->addChild(contact_empty_lbl);

    // Wired in OnConnect so xbus is available
    ctx.do_contact_search = std::make_shared<std::function<void(const std::string&)>>();

    ctx.contact_search_input->setOnChanged([do_contact_search = ctx.do_contact_search](const std::string& q) {
        if (q.size() >= 2 && *do_contact_search) (*do_contact_search)(q);
    });
}

// Quick Notes mini-grid (action center). Pure move out of main(): builds the
// header row + result column and the rebuild callback that renders up to 4 note
// cards. Behaviour is identical to the former inline block.
void buildNotesMiniPanel(ShellContext& ctx) {
    ctx.notes_mini_col = std::make_shared<FlexColumn>();
    ctx.notes_mini_col->setGap(4);

    ctx.rebuild_notes_mini = std::make_shared<std::function<void(const std::vector<NoteEntry>&)>>();

    ctx.notes_mini_hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        []{ auto t = std::make_shared<TextWidget>("Quick Notes", 14.0f);
            t->setColor(0.6f, 0.6f, 0.65f, 1.0f); return t; }(),
        std::make_shared<Spacer>(1, 1, true),
        []{ auto b = std::make_shared<ButtonWidget>(
                std::make_shared<TextWidget>("Super+N", 11.0f), [](){});
            b->setBackgroundColor({0.15f, 0.15f, 0.22f, 1.0f});
            b->setPadding(4);
            return b; }()
    });

    *ctx.rebuild_notes_mini = [notes_mini_col = ctx.notes_mini_col](const std::vector<NoteEntry>& entries) {
        notes_mini_col->clearChildren();
        static const Color kNoteColors[] = {
            {0.18f, 0.18f, 0.25f, 1.0f}, // 0=default
            {0.30f, 0.26f, 0.08f, 1.0f}, // 1=yellow
            {0.10f, 0.25f, 0.12f, 1.0f}, // 2=green
            {0.08f, 0.18f, 0.32f, 1.0f}, // 3=blue
            {0.28f, 0.10f, 0.18f, 1.0f}, // 4=pink
        };
        int shown = 0;
        for (const auto& e : entries) {
            if (shown >= 4) break; // cap at 4 cards in the panel
            auto card_col = std::make_shared<FlexColumn>();
            card_col->setPadding(8);
            if (e.title_[0]) {
                auto tl = std::make_shared<TextWidget>(std::string(e.title_), 12.0f);
                tl->setColor(0.90f, 0.90f, 0.90f, 1.0f);
                card_col->addChild(tl);
            }
            if (e.body_[0]) {
                std::string preview(e.body_);
                if (preview.size() > 60) preview = preview.substr(0, 57) + "...";
                auto bl = std::make_shared<TextWidget>(preview, 11.0f);
                bl->setColor(0.65f, 0.65f, 0.70f, 1.0f);
                card_col->addChild(bl);
            }
            auto card = std::make_shared<ContainerWidget>(card_col);
            card->setBackgroundColor(kNoteColors[e.color_ < 5 ? e.color_ : 0]);
            notes_mini_col->addChild(card);
            ++shown;
        }
        if (shown == 0) {
            auto lbl = std::make_shared<TextWidget>("No notes yet — press Super+N", 11.0f);
            lbl->setColor(0.4f, 0.4f, 0.45f, 1.0f);
            lbl->setPadding(4);
            notes_mini_col->addChild(lbl);
        }
    };
}

// Clipboard history mini-panel (action center). Builds the header row + result
// column; *rebuild_clip_ac is wired later in OnConnect once ch_entries is
// available. Behaviour is identical to the former inline block.
void buildClipboardMiniPanel(ShellContext& ctx) {
    auto clip_ac_hdr_lbl = std::make_shared<TextWidget>("📋 Clipboard", 14.0f);
    clip_ac_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.clip_ac_hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        clip_ac_hdr_lbl
    });
    ctx.clip_ac_col = std::make_shared<FlexColumn>();
    ctx.clip_ac_col->setGap(2);
    // *rebuild_clip_ac is wired in OnConnect once ch_entries is available
    ctx.rebuild_clip_ac = std::make_shared<std::function<void()>>();
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

// Gesture Config (quick-settings card). Enable switch + 2 sensitivity sliders +
// 3 behaviour switches, all wired to send_gest_cfg (builder-local helper). The
// update_gest_ui closure mirrors a GestureConfig back into the controls.
// Behaviour is identical to the former inline block.
void buildGestureConfigPanel(ShellContext& ctx) {
    ctx.gest_cfg       = std::make_shared<GestureConfig>();
    ctx.gest_switch    = std::make_shared<SwitchWidget>(false);
    ctx.gest_swipe3_sl = std::make_shared<SliderWidget>(50.0f, 0.0f, 100.0f);
    ctx.gest_pinch_sl  = std::make_shared<SliderWidget>(50.0f, 0.0f, 100.0f);
    ctx.gest_tap_sw    = std::make_shared<SwitchWidget>(false);
    ctx.gest_nscroll_sw = std::make_shared<SwitchWidget>(false);
    ctx.gest_2fscroll_sw = std::make_shared<SwitchWidget>(false);

    auto send_gest_cfg = [gest_cfg = ctx.gest_cfg, gest_switch = ctx.gest_switch,
                          gest_swipe3_sl = ctx.gest_swipe3_sl, gest_pinch_sl = ctx.gest_pinch_sl,
                          gest_tap_sw = ctx.gest_tap_sw, gest_nscroll_sw = ctx.gest_nscroll_sw,
                          gest_2fscroll_sw = ctx.gest_2fscroll_sw]() {
        if (!g_shell_xbus) return;
        GestureConfig cfg{};
        cfg.enabled_            = gest_switch->value()    ? 1u : 0u;
        cfg.swipe3_sensitivity_ = static_cast<uint32_t>(gest_swipe3_sl->value());
        cfg.swipe4_sensitivity_ = cfg.swipe3_sensitivity_;
        cfg.pinch_sensitivity_  = static_cast<uint32_t>(gest_pinch_sl->value());
        cfg.tap_to_click_       = gest_tap_sw->value()    ? 1u : 0u;
        cfg.natural_scroll_     = gest_nscroll_sw->value() ? 1u : 0u;
        cfg.two_finger_scroll_  = gest_2fscroll_sw->value() ? 1u : 0u;
        cfg.edge_scroll_        = gest_cfg->edge_scroll_;
        *gest_cfg = cfg;
        std::vector<uint8_t> buf(sizeof(cfg));
        std::memcpy(buf.data(), &cfg, sizeof(cfg));
        g_shell_xbus->Call("com.freebsdx.Gesture",
            static_cast<uint32_t>(GestureOpcode::SetConfig), buf, nullptr);
    };

    ctx.gest_switch->setOnChanged([send_gest_cfg](bool) { send_gest_cfg(); });
    ctx.gest_tap_sw->setOnChanged([send_gest_cfg](bool) { send_gest_cfg(); });
    ctx.gest_nscroll_sw->setOnChanged([send_gest_cfg](bool) { send_gest_cfg(); });
    ctx.gest_2fscroll_sw->setOnChanged([send_gest_cfg](bool) { send_gest_cfg(); });
    ctx.gest_swipe3_sl->setOnChanged([send_gest_cfg](float) { send_gest_cfg(); });
    ctx.gest_pinch_sl->setOnChanged([send_gest_cfg](float) { send_gest_cfg(); });

    ctx.update_gest_ui = [gest_cfg = ctx.gest_cfg, gest_switch = ctx.gest_switch,
                           gest_swipe3_sl = ctx.gest_swipe3_sl, gest_pinch_sl = ctx.gest_pinch_sl,
                           gest_tap_sw = ctx.gest_tap_sw, gest_nscroll_sw = ctx.gest_nscroll_sw,
                           gest_2fscroll_sw = ctx.gest_2fscroll_sw](const GestureConfig& cfg) {
        *gest_cfg = cfg;
        gest_switch->setValue(cfg.enabled_ != 0);
        gest_swipe3_sl->setValue(static_cast<float>(cfg.swipe3_sensitivity_));
        gest_pinch_sl->setValue(static_cast<float>(cfg.pinch_sensitivity_));
        gest_tap_sw->setValue(cfg.tap_to_click_ != 0);
        gest_nscroll_sw->setValue(cfg.natural_scroll_ != 0);
        gest_2fscroll_sw->setValue(cfg.two_finger_scroll_ != 0);
    };
}

// Stylus input (quick-settings card). Palm-rejection switch + a 3-way pressure
// button row (Soft/Normal/Firm) + a connection-status label. The pressure
// buttons and update closure gate on stylus_connected; stylus_pressure_hl stays
// a builder-local highlight helper. Behaviour is identical to the former inline
// block.
void buildStylusPanel(ShellContext& ctx) {
    constexpr int kPressureCount = 3;
    static const char* kPressureLabels[] = {"Soft", "Normal", "Firm"};
    ctx.stylus_palm_switch  = std::make_shared<SwitchWidget>(true);
    ctx.stylus_connected    = std::make_shared<bool>(false);
    ctx.stylus_pressure_cur = std::make_shared<uint32_t>(1u);
    ctx.stylus_pressure_btns =
        std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kPressureCount>>();

    auto stylus_pressure_hl = [stylus_pressure_btns = ctx.stylus_pressure_btns,
                               stylus_pressure_cur = ctx.stylus_pressure_cur,
                               stylus_connected = ctx.stylus_connected]() {
        for (int i = 0; i < kPressureCount; ++i) {
            bool active = (*stylus_pressure_cur == static_cast<uint32_t>(i));
            (*stylus_pressure_btns)[i]->setBackgroundColor(
                !*stylus_connected ? Color{0.18f, 0.18f, 0.20f, 1.0f}
                : active           ? Color{0.20f, 0.45f, 0.85f, 1.0f}
                                   : Color{0.22f, 0.22f, 0.26f, 1.0f});
        }
    };

    for (int i = 0; i < kPressureCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kPressureLabels[i]);
        btn->setFixedSize(46, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});
        btn->onClick([i, stylus_pressure_cur = ctx.stylus_pressure_cur, stylus_pressure_hl,
                      stylus_connected = ctx.stylus_connected]() {
            if (!*stylus_connected) return;
            *stylus_pressure_cur = static_cast<uint32_t>(i);
            stylus_pressure_hl();
            if (!g_shell_xbus) return;
            StylusRequest req{};
            req.pressure_curve_ = static_cast<uint32_t>(i);
            g_shell_xbus->Call("com.freebsdx.Stylus",
                static_cast<uint32_t>(StylusOpcode::SetPressure),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
        (*ctx.stylus_pressure_btns)[i] = btn;
    }
    stylus_pressure_hl();

    ctx.stylus_conn_lbl = std::make_shared<TextWidget>("No stylus detected", 10.0f);
    ctx.stylus_conn_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

    ctx.update_stylus_ui = [stylus_palm_switch = ctx.stylus_palm_switch,
                             stylus_connected = ctx.stylus_connected,
                             stylus_pressure_cur = ctx.stylus_pressure_cur, stylus_pressure_hl,
                             stylus_conn_lbl = ctx.stylus_conn_lbl](const StylusState& st) {
        *stylus_connected = st.connected_ != 0;
        stylus_palm_switch->setValue(st.palm_rejection_ != 0);
        *stylus_pressure_cur = std::min(st.pressure_curve_, 2u);
        stylus_pressure_hl();
        stylus_conn_lbl->setText(*stylus_connected ? "Stylus connected" : "No stylus detected");
        stylus_conn_lbl->setColor(*stylus_connected
            ? Color{0.35f, 0.80f, 0.45f, 1.0f}
            : Color{0.50f, 0.50f, 0.55f, 1.0f});
    };

    ctx.stylus_palm_switch->setOnChanged([stylus_connected = ctx.stylus_connected](bool on) {
        if (!*stylus_connected) return;
        if (!g_shell_xbus) return;
        StylusRequest req{};
        req.palm_rejection_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.Stylus",
            static_cast<uint32_t>(StylusOpcode::SetPalmReject),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });

    ctx.stylus_pressure_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<Spacer>(1, 1, true),
        (*ctx.stylus_pressure_btns)[0],
        std::make_shared<Spacer>(4, 1),
        (*ctx.stylus_pressure_btns)[1],
        std::make_shared<Spacer>(4, 1),
        (*ctx.stylus_pressure_btns)[2],
    });
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

// Cursor Size presets (quick-settings card). A 3-way cycle selector (S/M/L) —
// shared index + button row + cursor_size_hl highlight helper (stays
// builder-local) + update_cursor_size_ui. Behaviour is identical to the former
// inline block.
void buildCursorSizePanel(ShellContext& ctx) {
    constexpr int kCursorSizeCount = 3;
    static const char*    kCursorSizeLabels[] = {"S", "M", "L"};
    ctx.cursor_size_cur  = std::make_shared<uint32_t>(1u); // default Medium
    ctx.cursor_size_btns = std::make_shared<std::array<std::shared_ptr<ButtonWidget>, kCursorSizeCount>>();

    auto cursor_size_hl = [cursor_size_btns = ctx.cursor_size_btns, cursor_size_cur = ctx.cursor_size_cur]() {
        for (int i = 0; i < kCursorSizeCount; ++i) {
            bool active = (*cursor_size_cur == static_cast<uint32_t>(i));
            (*cursor_size_btns)[i]->setBackgroundColor(
                active ? Color{0.20f, 0.45f, 0.85f, 1.0f}
                       : Color{0.22f, 0.22f, 0.26f, 1.0f});
        }
    };

    for (int i = 0; i < kCursorSizeCount; ++i) {
        auto btn = std::make_shared<ButtonWidget>(kCursorSizeLabels[i]);
        btn->setFixedSize(36, 26);
        btn->setBackgroundColor({0.22f, 0.22f, 0.26f, 1.0f});
        btn->onClick([i, cursor_size_cur = ctx.cursor_size_cur, cursor_size_hl]() {
            *cursor_size_cur = static_cast<uint32_t>(i);
            cursor_size_hl();
            if (!g_shell_xbus) return;
            CursorSizeRequest req{};
            req.size_ = static_cast<uint32_t>(i);
            g_shell_xbus->Call("com.freebsdx.CursorSize",
                static_cast<uint32_t>(CursorSizeOpcode::SetSize),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        });
        (*ctx.cursor_size_btns)[i] = btn;
    }
    cursor_size_hl();

    ctx.update_cursor_size_ui = [cursor_size_cur = ctx.cursor_size_cur, cursor_size_hl](const CursorSizeState& st) {
        *cursor_size_cur = std::min(st.size_, 2u);
        cursor_size_hl();
    };

    ctx.cursor_size_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("\xf0\x9f\x96\xb1 Cursor", 12.0f),
        std::make_shared<Spacer>(1, 1, true),
        (*ctx.cursor_size_btns)[0],
        std::make_shared<Spacer>(4, 1),
        (*ctx.cursor_size_btns)[1],
        std::make_shared<Spacer>(4, 1),
        (*ctx.cursor_size_btns)[2],
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

// Microphone Mute (quick-settings card). Switch + Live/MUTED status label
// (green↔red); update_mic_mute_ui mirrors a MicMuteState into them. Behaviour
// is identical to the former inline block.
void buildMicMutePanel(ShellContext& ctx) {
    ctx.mic_mute_switch  = std::make_shared<SwitchWidget>(false);
    ctx.mic_mute_status  = std::make_shared<TextWidget>("Live", 11.0f);
    ctx.mic_mute_status->setColor({0.45f, 0.85f, 0.45f, 1.0f}); // green = live
    ctx.mic_mute_switch->setOnChanged([mic_mute_status = ctx.mic_mute_status](bool on) {
        if (!g_shell_xbus) return;
        MicMuteRequest req{};
        req.muted_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.MicMute",
            static_cast<uint32_t>(MicMuteOpcode::SetMuted),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        mic_mute_status->setText(on ? "MUTED" : "Live");
        mic_mute_status->setColor(on
            ? Color{0.90f, 0.35f, 0.35f, 1.0f}   // red = muted
            : Color{0.45f, 0.85f, 0.45f, 1.0f}); // green = live
    });
    ctx.update_mic_mute_ui = [mic_mute_switch = ctx.mic_mute_switch, mic_mute_status = ctx.mic_mute_status](const MicMuteState& st) {
        bool muted = st.muted_ != 0;
        mic_mute_switch->setValue(muted);
        mic_mute_status->setText(muted ? "MUTED" : "Live");
        mic_mute_status->setColor(muted
            ? Color{0.90f, 0.35f, 0.35f, 1.0f}
            : Color{0.45f, 0.85f, 0.45f, 1.0f});
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

// Location Services app list (paginated Fetcher). A latent panel (its widgets
// are not shown in the quick-settings card): master switch + per-app precision
// cycle rows, paged from com.freebsdx.Location ListApps. Only update_loc_policy
// + rebuild_loc escape; the widgets + LocAppFetcher machinery stay
// builder-local. Behaviour is identical to the former inline block.
void buildLocationServicesPanel(ShellContext& ctx) {
    auto loc_master_sw   = std::make_shared<SwitchWidget>(true);
    auto loc_app_col     = std::make_shared<FlexColumn>();
    loc_app_col->setGap(4);
    auto loc_count_lbl_b   = std::make_shared<TextWidget>("", 11.0f);
    loc_count_lbl_b->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    auto loc_status_lbl  = std::make_shared<TextWidget>("", 10.0f);
    loc_status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});

    ctx.update_loc_policy = [loc_master_sw](const LocationState& s) {
        loc_master_sw->setValue(s.enabled_ != 0);
    };

    loc_master_sw->setOnChanged([loc_status_lbl](bool on) {
        if (!g_shell_xbus) return;
        LocationEnableRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.Location",
            static_cast<uint32_t>(LocationOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [loc_status_lbl](const XBusMessage&) {
                loc_status_lbl->setText("Saved \xe2\x9c\x93");
            });
    });

    ctx.rebuild_loc = std::make_shared<std::function<void()>>();
    struct LocAppFetcher {
        std::shared_ptr<FlexColumn>              col;
        std::shared_ptr<TextWidget>              count_lbl;
        std::weak_ptr<std::function<void()>>     rw;
        std::function<void(uint32_t)>            fetch;
    };
    auto laf = std::make_shared<LocAppFetcher>();
    auto laf_weak = std::weak_ptr<LocAppFetcher>(laf);

    laf->col       = loc_app_col;
    laf->count_lbl = loc_count_lbl_b;
    laf->rw        = ctx.rebuild_loc;
    laf->fetch = [laf_weak](uint32_t page) {
        auto laf = laf_weak.lock();
        if (!laf || !g_shell_xbus) return;
        uint32_t pg = page;
        g_shell_xbus->Call("com.freebsdx.Location",
            static_cast<uint32_t>(LocationOpcode::ListApps),
            {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
            [laf_weak, page](const XBusMessage& r) {
                auto laf = laf_weak.lock();
                if (!laf) return;
                LocationAppPage ap{};
                std::memcpy(&ap, r.payload_, sizeof(ap));
                if (!ap.valid_) return;
                if (page == 0)
                    laf->col->clearChildren();
                // Precision label mapping
                static const char* kPrec[] = {"Off", "~Approx", "Precise"};
                static const std::array<Color,3> kPrecColor{{
                    {0.70f,0.35f,0.35f,1.0f},
                    {0.75f,0.65f,0.30f,1.0f},
                    {0.35f,0.70f,0.45f,1.0f},
                }};
                uint32_t prec = ap.precision_ < 3 ? ap.precision_ : 0;
                auto name_lbl = std::make_shared<TextWidget>(ap.name_[0] ? ap.name_ : ap.identifier_, 11.0f);
                name_lbl->setColor({0.85f,0.85f,0.90f,1.0f});
                auto prec_lbl = std::make_shared<TextWidget>(kPrec[prec], 10.0f);
                prec_lbl->setColor(kPrecColor[prec]);
                // Cycle button: tap to rotate Off→Approx→Precise→Off
                auto id_str = std::make_shared<std::string>(ap.identifier_);
                auto cur_prec = std::make_shared<uint32_t>(prec);
                auto cycle_btn = std::make_shared<ButtonWidget>("\xe2\x86\xbb");
                cycle_btn->setFixedSize(26, 20);
                cycle_btn->setBackgroundColor({0.25f,0.25f,0.32f,0.90f});
                cycle_btn->onClick([id_str, cur_prec, prec_lbl, laf_weak]() {
                    *cur_prec = (*cur_prec + 1) % 3;
                    auto laf = laf_weak.lock();
                    if (!laf || !g_shell_xbus) return;
                    LocationAppPolicyRequest req{};
                    std::strncpy(req.identifier_, id_str->c_str(), sizeof(req.identifier_)-1);
                    req.precision_ = *cur_prec;
                    g_shell_xbus->Call("com.freebsdx.Location",
                        static_cast<uint32_t>(LocationOpcode::SetAppPolicy),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [laf_weak](const XBusMessage&) {
                            if (auto laf = laf_weak.lock(); laf && laf->rw.lock())
                                (*laf->rw.lock())();
                        });
                });
                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    name_lbl,
                    std::make_shared<Spacer>(1,1,true),
                    prec_lbl,
                    std::make_shared<Spacer>(6,1),
                    cycle_btn,
                });
                laf->col->addChild(row);
                if (laf->count_lbl)
                    laf->count_lbl->setText(std::format("{}", ap.total_));
                if (ap.has_more_) laf->fetch(page + 1);
            });
    };

    *ctx.rebuild_loc = [loc_app_col, laf]() {
        loc_app_col->clearChildren();
        if (laf->fetch) laf->fetch(0);
    };
}

// Camera Devices (paginated Fetcher list). Count label + result column +
// rebuild_cam_dev, which spins a builder-local CamFetcher paging com.freebsdx.
// Camera ListDevices, rendering name/resolution rows with Open/Close buttons.
// Behaviour is identical to the former inline block.
void buildCameraDevicesPanel(ShellContext& ctx) {
    ctx.cam_dev_count_lbl = std::make_shared<TextWidget>("No cameras", 10.0f);
    ctx.cam_dev_count_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
    ctx.cam_dev_col = std::make_shared<FlexColumn>();
    ctx.cam_dev_col->setGap(4);

    ctx.rebuild_cam_dev = std::make_shared<std::function<void()>>();
    *ctx.rebuild_cam_dev = [cam_dev_col = ctx.cam_dev_col, cam_dev_count_lbl = ctx.cam_dev_count_lbl,
                        rcd_weak = std::weak_ptr<std::function<void()>>(ctx.rebuild_cam_dev)]() {
        if (!g_shell_xbus) return;
        cam_dev_col->clearChildren();

        struct CamFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<CamFetcher>();
        pf->col   = cam_dev_col;
        pf->count = cam_dev_count_lbl;
        pf->fetch = [pf_weak = std::weak_ptr<CamFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.Camera",
                static_cast<uint32_t>(CameraOpcode::ListDevices),
                {reinterpret_cast<const uint8_t*>(&pg_num),
                 reinterpret_cast<const uint8_t*>(&pg_num) + sizeof(pg_num)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    CameraDevicePage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0) pf->total = pg.total_;
                    if (page == 0 && pg.total_ == 0) {
                        pf->count->setText("No cameras");
                        pf->count->setColor({0.50f, 0.50f, 0.55f, 1.0f});
                        return;
                    }
                    if (page == 0) {
                        pf->count->setText(std::format("{} device(s)", pf->total));
                        pf->count->setColor({0.35f, 0.80f, 0.50f, 1.0f});
                    }
                    if (pg.valid_) {
                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(pg.name_), 11.0f);
                        name_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});
                        name_lbl->setFlexGrow(1.0f);

                        auto res_lbl = std::make_shared<TextWidget>(
                            std::format("{}x{}", pg.width_, pg.height_), 10.0f);
                        res_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        std::string dev_path = std::string(pg.path_);
                        auto open_btn = std::make_shared<ButtonWidget>("Open",
                            [dev_path]() {
                                if (!g_shell_xbus) return;
                                CameraOpenRequest req{};
                                std::strncpy(req.path_, dev_path.c_str(),
                                             sizeof(req.path_) - 1);
                                g_shell_xbus->Call("com.freebsdx.Camera",
                                    static_cast<uint32_t>(CameraOpcode::Open),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    nullptr);
                            });
                        open_btn->setFixedSize(50, 22);
                        open_btn->setBackgroundColor({0.20f, 0.55f, 0.90f, 1.0f});

                        auto close_btn = std::make_shared<ButtonWidget>("Close",
                            []() {
                                if (!g_shell_xbus) return;
                                g_shell_xbus->Call("com.freebsdx.Camera",
                                    static_cast<uint32_t>(CameraOpcode::Close),
                                    {}, nullptr);
                            });
                        close_btn->setFixedSize(50, 22);
                        close_btn->setBackgroundColor({0.35f, 0.35f, 0.40f, 1.0f});

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl, res_lbl,
                                std::make_shared<Spacer>(4, 1),
                                open_btn,
                                std::make_shared<Spacer>(4, 1),
                                close_btn});
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
// Calendar quick-add. Header + a text input + status label + "+ Add" button (in
// cal_add_row). The button parses the input via parseCalInput, resolves the date
// (handling "tomorrow" + month-boundary via mktime normalization), and posts a
// CalendarEvent through com.freebsdx.Calendar AddEvent. The "+ Add" button stays
// builder-local (held alive by the escaping cal_add_row). Behaviour identical to
// the former inline block.
void buildCalendarQuickAddPanel(ShellContext& ctx) {
    ctx.cal_add_hdr_lbl = std::make_shared<TextWidget>("📅 Quick Add Event", 14.0f);
    ctx.cal_add_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.cal_add_input  = std::make_shared<TextInputWidget>();
    ctx.cal_add_input->setPlaceholder("Meeting at 3pm  /  Call tomorrow 14:00");
    ctx.cal_add_status = std::make_shared<TextWidget>("", 11.0f);
    ctx.cal_add_status->setColor({0.55f, 0.85f, 0.55f, 1.0f});
    auto cal_add_btn = std::make_shared<ButtonWidget>("+ Add",
        [cal_add_input = ctx.cal_add_input, cal_add_status = ctx.cal_add_status]() {
            std::string title; int h = 0, m = 0; bool tom = false;
            if (!parseCalInput(cal_add_input->getText(), title, h, m, tom)) {
                cal_add_status->setColor({0.90f, 0.50f, 0.40f, 1.0f});
                cal_add_status->setText("Include a time, e.g. \"3pm\" or \"14:00\"");
                return;
            }
            if (!g_shell_xbus) return;
            // Resolve date (handle tomorrow, month-boundary via mktime normalization)
            std::time_t base = std::time(nullptr);
            if (tom) base += 24 * 3600;
            std::tm lt{}; std::tm* lp = std::localtime(&base);
            if (!lp) return;
            lt = *lp;
            CalendarEvent ev{};
            ev.year_       = static_cast<uint16_t>(lt.tm_year + 1900);
            ev.month_      = static_cast<uint8_t>(lt.tm_mon + 1);
            ev.day_        = static_cast<uint8_t>(lt.tm_mday);
            ev.hour_       = static_cast<uint8_t>(h);
            ev.minute_     = static_cast<uint8_t>(m);
            ev.end_hour_   = static_cast<uint8_t>((h + 1) % 24);
            ev.end_minute_ = static_cast<uint8_t>(m);
            ev.reminder_min_ = 10;
            std::strncpy(ev.title_, title.c_str(), sizeof(ev.title_) - 1);
            g_shell_xbus->Call("com.freebsdx.Calendar",
                static_cast<uint32_t>(CalendarOpcode::AddEvent),
                {reinterpret_cast<const uint8_t*>(&ev), sizeof(ev)},
                [cal_add_input, cal_add_status](const XBusMessage&) {
                    cal_add_input->setText("");
                    cal_add_status->setColor({0.55f, 0.85f, 0.55f, 1.0f});
                    cal_add_status->setText("✓ Event added");
                });
        });
    cal_add_btn->setFontSize(12.0f);
    cal_add_btn->setFixedSize(60, 28);
    ctx.cal_add_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        ctx.cal_add_input, std::make_shared<Spacer>(6, 1), cal_add_btn
    });
}

void buildBatterySaverPanel(ShellContext& ctx) {
    ctx.bat_saver_switch = std::make_shared<SwitchWidget>(false);
    ctx.bat_saver_lbl    = std::make_shared<TextWidget>("Battery Saver", 12.0f);
    ctx.bat_saver_lbl->setColor({0.75f, 0.75f, 0.80f, 1.0f});
    ctx.bat_saver_switch->setOnChanged([bat_saver_lbl = ctx.bat_saver_lbl](bool on) {
        g_bat_saver = on;
        bat_saver_lbl->setColor(on ? Color{1.0f, 0.78f, 0.20f, 1.0f}
                                   : Color{0.75f, 0.75f, 0.80f, 1.0f});
        if (!g_shell_xbus) return;
        if (on) {
            // Engage: record current state, apply saver settings
            g_bat_saver_prev_lock = g_idle_lock_secs.load();
            // Dim brightness to 30%
            BacklightState br{};
            br.level_ = 30u;
            g_shell_xbus->Call("com.freebsdx.Backlight",
                static_cast<uint32_t>(BacklightOpcode::SetBrightness),
                {reinterpret_cast<const uint8_t*>(&br), sizeof(br)}, nullptr);
            // Set lock timeout to 60s
            g_idle_lock_secs.store(60);
            LockTimeoutRequest lr{}; lr.seconds_ = 60u;
            g_shell_xbus->Call("com.freebsdx.Lock",
                static_cast<uint32_t>(LockOpcode::SetTimeout),
                {reinterpret_cast<const uint8_t*>(&lr), sizeof(lr)}, nullptr);
            // Switch to PowerSaver profile
            PowerProfileRequest pr{};
            pr.profile_ = static_cast<uint32_t>(PowerProfileLevel::PowerSaver);
            g_shell_xbus->Call("com.freebsdx.PowerProfile",
                static_cast<uint32_t>(PowerProfileOpcode::SetProfile),
                {reinterpret_cast<const uint8_t*>(&pr), sizeof(pr)}, nullptr);
        } else {
            // Disengage: restore previous lock timeout
            g_idle_lock_secs.store(g_bat_saver_prev_lock);
            LockTimeoutRequest lr{}; lr.seconds_ = g_bat_saver_prev_lock;
            g_shell_xbus->Call("com.freebsdx.Lock",
                static_cast<uint32_t>(LockOpcode::SetTimeout),
                {reinterpret_cast<const uint8_t*>(&lr), sizeof(lr)}, nullptr);
            // Restore Balanced profile
            PowerProfileRequest pr{};
            pr.profile_ = static_cast<uint32_t>(PowerProfileLevel::Balanced);
            g_shell_xbus->Call("com.freebsdx.PowerProfile",
                static_cast<uint32_t>(PowerProfileOpcode::SetProfile),
                {reinterpret_cast<const uint8_t*>(&pr), sizeof(pr)}, nullptr);
            // Restore brightness to 100%
            BacklightState br{};
            br.level_ = 100u;
            g_shell_xbus->Call("com.freebsdx.Backlight",
                static_cast<uint32_t>(BacklightOpcode::SetBrightness),
                {reinterpret_cast<const uint8_t*>(&br), sizeof(br)}, nullptr);
        }
    });
    ctx.bat_saver_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🔋", 14.0f),
        std::make_shared<Spacer>(8, 1),
        ctx.bat_saver_lbl,
        std::make_shared<Spacer>(1, 1, true),
        ctx.bat_saver_switch
    });
}

void buildPomodoroPanel(ShellContext& ctx) {
    ctx.pomo_phase_lbl = std::make_shared<TextWidget>("Work", 11.0f);
    ctx.pomo_phase_lbl->setColor({0.55f, 0.90f, 0.55f, 1.0f});

    ctx.pomo_cd_lbl = std::make_shared<TextWidget>("25:00", 26.0f);
    ctx.pomo_cd_lbl->setColor({1.0f, 1.0f, 1.0f, 1.0f});
    ctx.pomo_cd_lbl->setAlignment(Alignment::Center);

    ctx.pomo_tbar_w = std::make_shared<TextWidget>("🍅 25:00", 12.0f);
    ctx.pomo_tbar_w->setColor({0.88f, 0.88f, 0.88f, 1.0f});
    ctx.pomo_tbar_w->setVisibility(false);

    ctx.pomo_start_lbl = std::make_shared<TextWidget>("▶ Start", 12.0f);
    auto pomo_start_btn = std::make_shared<ButtonWidget>(ctx.pomo_start_lbl,
        [pomo_start_lbl = ctx.pomo_start_lbl, pomo_tbar_w = ctx.pomo_tbar_w]() {
        bool now = !g_pomo_running.load();
        g_pomo_running.store(now);
        pomo_start_lbl->setText(now ? "⏸ Pause" : "▶ Resume");
        if (now) pomo_tbar_w->setVisibility(true);
    });
    pomo_start_btn->setFixedSize(110, 30);
    pomo_start_btn->setBackgroundColor({0.16f, 0.50f, 0.22f, 1.0f});

    auto pomo_reset_btn = std::make_shared<ButtonWidget>("↺ Reset",
        [pomo_phase_lbl = ctx.pomo_phase_lbl, pomo_cd_lbl = ctx.pomo_cd_lbl,
         pomo_tbar_w = ctx.pomo_tbar_w, pomo_start_lbl = ctx.pomo_start_lbl]() {
            g_pomo_running.store(false);
            g_pomo_phase.store(0);
            g_pomo_cycles.store(0);
            g_pomo_secs.store(kPomoWork);
            pomo_phase_lbl->setText("Work");
            pomo_phase_lbl->setColor({0.55f, 0.90f, 0.55f, 1.0f});
            pomo_cd_lbl->setText("25:00");
            pomo_tbar_w->setText("🍅 25:00");
            pomo_tbar_w->setVisibility(false);
            pomo_start_lbl->setText("▶ Start");
        });
    pomo_reset_btn->setFixedSize(80, 30);
    pomo_reset_btn->setBackgroundColor({0.40f, 0.16f, 0.16f, 1.0f});

    ctx.pomo_hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🍅", 14.0f),
        std::make_shared<Spacer>(8, 1),
        std::make_shared<TextWidget>("Focus Timer", 12.0f),
        std::make_shared<Spacer>(1, 1, true),
        ctx.pomo_phase_lbl
    });
    ctx.pomo_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        pomo_start_btn,
        std::make_shared<Spacer>(8, 1),
        pomo_reset_btn,
    });

    // Wallpaper rotation controls
    ctx.wp_rotate_switch = std::make_shared<SwitchWidget>(false);
    ctx.wp_folder_lbl    = std::make_shared<TextWidget>("Folder: (none)", 11.0f);
    ctx.wp_folder_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    ctx.wp_interval_lbl  = std::make_shared<TextWidget>("30 min", 12.0f);

    static constexpr std::pair<int,const char*> kWpIntervals[] = {
        {300,  "5 min"}, {1800, "30 min"}, {3600, "1 hr"}, {86400, "Daily"}
    };
    ctx.wp_interval_dec = std::make_shared<ButtonWidget>("<");
    ctx.wp_interval_dec->setFixedSize(28, 28);
    ctx.wp_interval_inc = std::make_shared<ButtonWidget>(">");
    ctx.wp_interval_inc->setFixedSize(28, 28);

    ctx.update_wp_interval_lbl = [wp_interval_lbl = ctx.wp_interval_lbl]() {
        for (const auto& [secs, lbl] : kWpIntervals)
            if (secs == g_wp_rotate_interval) { wp_interval_lbl->setText(lbl); return; }
        wp_interval_lbl->setText(std::format("{}s", g_wp_rotate_interval));
    };

    ctx.wp_interval_dec->onClick([update_wp_interval_lbl = ctx.update_wp_interval_lbl]() {
        for (int i = 1; i < 4; ++i)
            if (kWpIntervals[i].first == g_wp_rotate_interval) {
                g_wp_rotate_interval = kWpIntervals[i-1].first;
                g_wp_rotate_countdown = 0;
                update_wp_interval_lbl();
                saveWpRotate();
                return;
            }
    });
    ctx.wp_interval_inc->onClick([update_wp_interval_lbl = ctx.update_wp_interval_lbl]() {
        for (int i = 0; i < 3; ++i)
            if (kWpIntervals[i].first == g_wp_rotate_interval) {
                g_wp_rotate_interval = kWpIntervals[i+1].first;
                g_wp_rotate_countdown = 0;
                update_wp_interval_lbl();
                saveWpRotate();
                return;
            }
    });

    ctx.wp_rotate_switch->setOnChanged([](bool on) {
        g_wp_rotate_on = on;
        g_wp_rotate_countdown = 0;
        saveWpRotate();
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

// Scratch pad section (action center). A single persistent quick-note input
// backed by g_scratch_note; Clear button + on-changed both persist via
// saveScratchNote(). Behaviour is identical to the former inline block.
void buildScratchPadPanel(ShellContext& ctx) {
    auto scratch_hdr_lbl = std::make_shared<TextWidget>("📝 Scratch Pad", 14.0f);
    scratch_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.scratch_input = std::make_shared<TextInputWidget>();
    ctx.scratch_input->setPlaceholder("Type a quick note…");
    auto scratch_clear_btn = std::make_shared<ButtonWidget>("Clear", [scratch_input = ctx.scratch_input]() {
        scratch_input->setText("");
        g_scratch_note.clear();
        saveScratchNote();
    });
    scratch_clear_btn->setFontSize(11.0f);
    scratch_clear_btn->setFixedSize(50, 24);
    ctx.scratch_hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        scratch_hdr_lbl,
        std::make_shared<Spacer>(1, 1, true),
        scratch_clear_btn
    });
    ctx.scratch_input->setOnChanged([](const std::string& text) {
        g_scratch_note = text;
        saveScratchNote();
    });
}

// 3-Day Weather Forecast. Header + a row of 3 cards (day/icon/temp labels); the
// per-card label arrays are captured by forecast_rebuild, which is wired later in
// OnConnect — so the builder only creates the empty forecast_rebuild shared_ptr.
// The kDayNames table stays builder-local (the OnConnect rebuild has its own copy).
// Behaviour identical to the former inline block.
void buildWeatherForecastPanel(ShellContext& ctx) {
    ctx.forecast_hdr_lbl = std::make_shared<TextWidget>("🌤 3-Day Forecast", 14.0f);
    ctx.forecast_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});

    // Per-card label arrays — captured by rebuild_forecast wired in OnConnect
    ctx.forecast_day_lbls  = std::make_shared<std::array<std::shared_ptr<TextWidget>, 3>>();
    ctx.forecast_icon_lbls = std::make_shared<std::array<std::shared_ptr<TextWidget>, 3>>();
    ctx.forecast_temp_lbls = std::make_shared<std::array<std::shared_ptr<TextWidget>, 3>>();
    ctx.forecast_rebuild   = std::make_shared<std::function<void()>>();

    static const char* kDayNames[] = {"Today","Tomorrow","In 2 days"};
    ctx.forecast_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{});
    for (int fi = 0; fi < 3; ++fi) {
        auto day_lbl  = std::make_shared<TextWidget>(kDayNames[fi], 10.0f);
        day_lbl->setColor({0.65f, 0.65f, 0.70f, 1.0f});
        day_lbl->setAlignment(Alignment::Center);
        auto icon_lbl = std::make_shared<TextWidget>("⋯", 18.0f);
        icon_lbl->setAlignment(Alignment::Center);
        auto temp_lbl = std::make_shared<TextWidget>("--", 10.0f);
        temp_lbl->setColor({0.75f, 0.82f, 0.95f, 1.0f});
        temp_lbl->setAlignment(Alignment::Center);
        (*ctx.forecast_day_lbls)[fi]  = day_lbl;
        (*ctx.forecast_icon_lbls)[fi] = icon_lbl;
        (*ctx.forecast_temp_lbls)[fi] = temp_lbl;
        auto card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            day_lbl, std::make_shared<Spacer>(1, 4),
            icon_lbl, std::make_shared<Spacer>(1, 4), temp_lbl
        });
        card->setPadding(8);
        card->setBackgroundColor({0.12f, 0.15f, 0.24f, 1.0f});
        card->setAlignment(Alignment::Center);
        ctx.forecast_row->addChild(card);
        if (fi < 2) ctx.forecast_row->addChild(std::make_shared<Spacer>(6, 1));
    }
}

// Quick countdown timer. Header + duration input + big countdown label + optional
// label input + status label + Start/Stop buttons (in timer_btn_row). Start parses
// parseTimerInput and arms the file-scope g_countdown (under g_countdown_mtx); Stop
// clears it. The countdown label is ticked elsewhere (timer loop). The Start/Stop
// buttons stay builder-local (held by the escaping timer_btn_row). Behaviour
// identical to the former inline block.
void buildQuickTimerPanel(ShellContext& ctx) {
    ctx.timer_hdr_lbl    = std::make_shared<TextWidget>("⏱ Quick Timer", 14.0f);
    ctx.timer_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.timer_input      = std::make_shared<TextInputWidget>();
    ctx.timer_input->setPlaceholder("10m  /  1h30m  /  90s");
    ctx.timer_cd_lbl     = std::make_shared<TextWidget>("", 20.0f);
    ctx.timer_cd_lbl->setColor({0.45f, 0.85f, 1.0f, 1.0f});
    ctx.timer_cd_lbl->setAlignment(Alignment::Center);
    ctx.timer_label_input = std::make_shared<TextInputWidget>();
    ctx.timer_label_input->setPlaceholder("Label (optional)");
    ctx.timer_status_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.timer_status_lbl->setColor({0.90f, 0.50f, 0.40f, 1.0f});
    auto timer_start_btn  = std::make_shared<ButtonWidget>("▶ Start",
        [timer_input = ctx.timer_input, timer_label_input = ctx.timer_label_input,
         timer_cd_lbl = ctx.timer_cd_lbl, timer_status_lbl = ctx.timer_status_lbl]() {
            int secs = parseTimerInput(timer_input->getText());
            if (secs <= 0) {
                timer_status_lbl->setText("Enter a duration, e.g. \"10m\"");
                return;
            }
            timer_status_lbl->setText("");
            std::string lbl = timer_label_input->getText();
            if (lbl.empty()) lbl = "Timer";
            std::lock_guard<std::mutex> lk(g_countdown_mtx);
            g_countdown.label  = lbl;
            g_countdown.end_at = std::time(nullptr) + secs;
            g_countdown.active = true;
            timer_cd_lbl->setText(fmtCountdown(secs));
        });
    timer_start_btn->setBackgroundColor({0.18f, 0.45f, 0.22f, 1.0f});
    timer_start_btn->setFixedSize(80, 30);
    auto timer_stop_btn   = std::make_shared<ButtonWidget>("■ Stop",
        [timer_cd_lbl = ctx.timer_cd_lbl, timer_status_lbl = ctx.timer_status_lbl]() {
            std::lock_guard<std::mutex> lk(g_countdown_mtx);
            g_countdown.active = false;
            timer_cd_lbl->setText("");
            timer_status_lbl->setText("Stopped");
        });
    timer_stop_btn->setBackgroundColor({0.40f, 0.14f, 0.14f, 1.0f});
    timer_stop_btn->setFixedSize(72, 30);
    ctx.timer_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        timer_start_btn, std::make_shared<Spacer>(6, 1), timer_stop_btn,
        std::make_shared<Spacer>(8, 1), ctx.timer_status_lbl
    });
}

// Pomodoro session log. Header row (label + Clear button) + result column;
// rebuild_pomo_log's body is assigned later (in OnConnect), so the builder only
// creates the empty shared_ptr. Clear wipes the file-scope g_pomo_log (under
// g_pomo_log_mtx), persists via savePomoLog, and re-triggers the rebuild. The
// header label + Clear button stay builder-local (held by the escaping
// pomo_log_hdr_row). Behaviour identical to the former inline block.
void buildPomodoroLogPanel(ShellContext& ctx) {
    auto pomo_log_hdr_lbl = std::make_shared<TextWidget>("🍅 Session Log", 14.0f);
    pomo_log_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.pomo_log_col = std::make_shared<FlexColumn>();
    ctx.pomo_log_col->setGap(3);
    ctx.rebuild_pomo_log = std::make_shared<std::function<void()>>();
    auto pomo_log_clear_btn = std::make_shared<ButtonWidget>("Clear",
        [rebuild_pomo_log = ctx.rebuild_pomo_log]() {
            { std::lock_guard<std::mutex> lk(g_pomo_log_mtx); g_pomo_log.clear(); }
            savePomoLog();
            if (*rebuild_pomo_log) (*rebuild_pomo_log)();
        });
    pomo_log_clear_btn->setFixedSize(52, 26);
    ctx.pomo_log_hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        pomo_log_hdr_lbl, std::make_shared<Spacer>(8, 1), pomo_log_clear_btn
    });
}

// App usage heatmap. Header row (label + Reset) + a monospace bar label rendered
// from the file-scope g_app_hourly via fmtHeatmap + an hour-axis label. Reset
// zeroes g_app_hourly, persists via saveAppHeatmap, and refreshes the bar; the bar
// is also refreshed elsewhere (g_heat_update_fn, wired in OnConnect). The header
// label + Reset button stay builder-local (held by the escaping heat_hdr_row).
// Behaviour identical to the former inline block.
void buildAppHeatmapPanel(ShellContext& ctx) {
    auto heat_hdr_lbl = std::make_shared<TextWidget>("📈 App Usage Heatmap", 14.0f);
    heat_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.heat_bar_lbl = std::make_shared<TextWidget>(fmtHeatmap(g_app_hourly), 13.0f);
    ctx.heat_bar_lbl->setFontFamily("monospace");
    ctx.heat_bar_lbl->setColor({0.45f, 0.85f, 0.75f, 1.0f});
    ctx.heat_hr_lbl = std::make_shared<TextWidget>("0  3  6  9 12 15 18 21", 10.0f);
    ctx.heat_hr_lbl->setFontFamily("monospace");
    ctx.heat_hr_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});
    auto heat_reset_btn = std::make_shared<ButtonWidget>("Reset",
        [heat_bar_lbl = ctx.heat_bar_lbl]() {
            g_app_hourly.fill(0);
            saveAppHeatmap();
            heat_bar_lbl->setText(fmtHeatmap(g_app_hourly));
        });
    heat_reset_btn->setFixedSize(60, 26);
    ctx.heat_hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        heat_hdr_lbl, std::make_shared<Spacer>(8, 1), heat_reset_btn
    });
}

// Screenshot & Recording. Header + status label + a Full/Delay(3s) capture button
// row + an Open button (revealed by the capture reply, opens the last file via the
// MIME handler) + a Record toggle + recording-status label. The recording state
// (ss_recording bool) + toggle/status labels are driven by RecordingChanged and
// reply handlers wired elsewhere. The Full/Delay buttons stay builder-local (held
// by the escaping ss_btn_row). Behaviour identical to the former inline block.
void buildScreenshotPanel(ShellContext& ctx) {
    ctx.ss_hdr_lbl    = std::make_shared<TextWidget>("📸 Screenshot & Recording", 14.0f);
    ctx.ss_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.ss_status_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.ss_status_lbl->setColor({0.5f, 0.5f, 0.55f, 1.0f});
    ctx.ss_last_path  = std::make_shared<std::string>();
    ctx.ss_open_btn   = std::make_shared<ButtonWidget>("Open", [ss_last_path = ctx.ss_last_path]() {
        if (!g_shell_xbus || ss_last_path->empty()) return;
        MimeQueryRequest qr{};
        std::strncpy(qr.path_, ss_last_path->c_str(), sizeof(qr.path_) - 1);
        qr.is_path_ = 1;
        std::string p = *ss_last_path;
        g_shell_xbus->Call("com.freebsdx.MIME",
            static_cast<uint32_t>(MimeOpcode::QueryHandler),
            {reinterpret_cast<const uint8_t*>(&qr), sizeof(qr)},
            [p](const XBusMessage& resp) {
                MimeHandlerInfo info{};
                std::memcpy(&info, resp.payload_, sizeof(info));
                if (info.app_id_[0]) launchApp(info.app_id_, p);
            });
    });
    ctx.ss_open_btn->setFixedSize(50, 26);
    ctx.ss_open_btn->setBackgroundColor({0.18f, 0.22f, 0.35f, 1.0f});
    ctx.ss_open_btn->setVisibility(false);

    auto ss_full_btn = std::make_shared<ButtonWidget>("📸 Full", []() {
        if (!g_shell_xbus) return;
        ScreenshotRequest req{};
        req.output_idx_ = 0;
        g_shell_xbus->Call("com.freebsdx.Screenshot",
            static_cast<uint32_t>(ScreenshotOpcode::Capture),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    ss_full_btn->setFixedSize(90, 30);
    ss_full_btn->setBackgroundColor({0.18f, 0.22f, 0.35f, 1.0f});

    auto ss_delay_btn = std::make_shared<ButtonWidget>("⏱ 3s", [ss_status_lbl = ctx.ss_status_lbl]() {
        if (!g_shell_xbus) return;
        ss_status_lbl->setText("Capturing in 3s…");
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!g_shell_xbus) return;
            ScreenshotRequest req{};
            req.output_idx_ = 0;
            g_shell_xbus->Call("com.freebsdx.Screenshot",
                static_cast<uint32_t>(ScreenshotOpcode::Capture),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        }).detach();
    });
    ss_delay_btn->setFixedSize(70, 30);
    ss_delay_btn->setBackgroundColor({0.25f, 0.20f, 0.35f, 1.0f});

    // Recording toggle — state tracked via shared bool, updated from RecordingChanged broadcast
    ctx.ss_recording       = std::make_shared<bool>(false);
    ctx.rec_toggle_btn     = std::make_shared<ButtonWidget>("⏺ Record", nullptr);
    ctx.rec_status_lbl     = std::make_shared<TextWidget>("", 11.0f);
    ctx.rec_status_lbl->setColor({0.85f, 0.40f, 0.40f, 1.0f});
    ctx.rec_toggle_btn->setFixedSize(100, 30);
    ctx.rec_toggle_btn->setBackgroundColor({0.32f, 0.12f, 0.12f, 1.0f});
    ctx.rec_toggle_btn->onClick([ss_recording = ctx.ss_recording, rec_toggle_btn = ctx.rec_toggle_btn,
                                 rec_status_lbl = ctx.rec_status_lbl]() {
        if (!g_shell_xbus) return;
        if (!*ss_recording) {
            ScreenshotRecordingRequest req{};
            req.output_idx_ = 0;
            req.fps_        = 10;
            g_shell_xbus->Call("com.freebsdx.Screenshot",
                static_cast<uint32_t>(ScreenshotOpcode::StartRecording),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        } else {
            g_shell_xbus->Call("com.freebsdx.Screenshot",
                static_cast<uint32_t>(ScreenshotOpcode::StopRecording), {}, nullptr);
        }
    });

    ctx.ss_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        ss_full_btn, std::make_shared<Spacer>(6, 1), ss_delay_btn
    });
    ctx.ss_last_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        ctx.ss_status_lbl, std::make_shared<Spacer>(6, 1), ctx.ss_open_btn
    });
    ctx.ss_last_row->setAlignment(Alignment::Center);
}
// Recent Files. Header + result column; rebuild_rf iterates the file-scope
// g_recent_files map (up to 8), rendering per-file name + truncated dir (HOME→~) +
// an Open button that resolves the MIME handler and launches it. No paged fetcher.
// Behaviour identical to the former inline block.
void buildRecentFilesPanel(ShellContext& ctx) {
    ctx.rf_hdr_lbl = std::make_shared<TextWidget>("📂 Recent Files", 14.0f);
    ctx.rf_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    ctx.rf_col = std::make_shared<FlexColumn>();
    ctx.rf_col->setGap(4);
    ctx.rebuild_rf = std::make_shared<std::function<void()>>();
    *ctx.rebuild_rf = [rf_col = ctx.rf_col]() {
        rf_col->clearChildren();
        int count = 0;
        for (const auto& [name, path] : g_recent_files) {
            if (count >= 8) break;
            std::string p = path;
            std::string n = name;
            // Show truncated path under name
            std::string dir = path.substr(0, path.find_last_of("/\\"));
            const char* home = std::getenv("HOME");
            if (home && dir.starts_with(home)) dir = "~" + dir.substr(std::strlen(home));
            if (dir.size() > 32) dir = "…" + dir.substr(dir.size() - 31);
            auto name_lbl = std::make_shared<TextWidget>(n, 12.0f);
            name_lbl->setColor({0.92f, 0.92f, 0.95f, 1.0f});
            auto path_lbl = std::make_shared<TextWidget>(dir, 10.0f);
            path_lbl->setColor({0.5f, 0.5f, 0.55f, 1.0f});
            auto info_col = std::make_shared<FlexColumn>(
                std::vector<std::shared_ptr<Widget>>{name_lbl, path_lbl});
            info_col->setGap(1);
            auto open_btn = std::make_shared<ButtonWidget>("Open", [p]() {
                if (!g_shell_xbus) return;
                MimeQueryRequest qr{};
                std::strncpy(qr.path_, p.c_str(), sizeof(qr.path_) - 1);
                qr.is_path_ = 1;
                g_shell_xbus->Call("com.freebsdx.MIME",
                    static_cast<uint32_t>(MimeOpcode::QueryHandler),
                    {reinterpret_cast<const uint8_t*>(&qr), sizeof(qr)},
                    [p](const XBusMessage& resp) {
                        MimeHandlerInfo info{};
                        std::memcpy(&info, resp.payload_, sizeof(info));
                        if (info.app_id_[0]) launchApp(info.app_id_, p);
                    });
            });
            open_btn->setFixedSize(52, 26);
            open_btn->setBackgroundColor({0.18f, 0.22f, 0.35f, 1.0f});
            auto row = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{info_col, std::make_shared<Spacer>(1, 1), open_btn});
            row->setGap(6);
            row->setAlignment(Alignment::Center);
            rf_col->addChild(row);
            ++count;
        }
        if (count == 0) {
            auto empty = std::make_shared<TextWidget>("No recent files", 11.0f);
            empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
            rf_col->addChild(empty);
        }
    };
}

// Named Clipboard Slots. Header + a column of kNumClipSlots (A..E) rows, each with
// a preview label + Save (reads the clipboard into g_clip_slots[i], persists via
// saveClipSlots, updates the preview) and Copy (writes the slot back to the
// clipboard). The per-slot Save/Copy buttons stay builder-local (held by cs_col's
// rows); only header/col + the preview-label array escape (OnConnect prefills the
// previews). The kSlotNames table stays a builder-local static. Behaviour
// identical to the former inline block.
void buildClipboardSlotsPanel(ShellContext& ctx) {
    ctx.cs_hdr_lbl = std::make_shared<TextWidget>("📋 Clipboard Slots", 14.0f);
    ctx.cs_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    static constexpr const char* kSlotNames[kNumClipSlots] = {"A","B","C","D","E"};
    ctx.cs_col = std::make_shared<FlexColumn>();
    ctx.cs_col->setGap(5);
    for (int i = 0; i < kNumClipSlots; ++i) {
        auto slot_lbl = std::make_shared<TextWidget>(std::string(kSlotNames[i]), 12.0f);
        slot_lbl->setColor({0.6f, 0.75f, 1.0f, 1.0f});
        slot_lbl->setFixedSize(16, 1);
        auto preview = std::make_shared<TextWidget>("(empty)", 10.0f);
        preview->setColor({0.45f, 0.45f, 0.5f, 1.0f});
        ctx.cs_preview_lbls[i] = preview;
        int idx = i;
        auto save_btn = std::make_shared<ButtonWidget>("Save", [idx, preview]() {
            if (!g_shell_xbus) return;
            // Read clipboard via opcode 0x02
            g_shell_xbus->Call("com.freebsdx.Clipboard", 0x02, {},
                [idx, preview](const XBusMessage& resp) {
                    char buf[256] = {};
                    std::memcpy(buf, resp.payload_,
                        std::min(sizeof(buf) - 1, sizeof(resp.payload_)));
                    g_clip_slots[idx] = std::string(buf);
                    saveClipSlots();
                    std::string prev = g_clip_slots[idx];
                    if (prev.size() > 28) prev = prev.substr(0, 27) + "…";
                    preview->setText(prev.empty() ? "(empty)" : prev);
                });
        });
        save_btn->setFixedSize(46, 26);
        save_btn->setBackgroundColor({0.18f, 0.35f, 0.22f, 1.0f});
        auto copy_btn = std::make_shared<ButtonWidget>("Copy", [idx]() {
            if (!g_shell_xbus || g_clip_slots[idx].empty()) return;
            char buf[256] = {};
            std::strncpy(buf, g_clip_slots[idx].c_str(), sizeof(buf) - 1);
            g_shell_xbus->Call("com.freebsdx.Clipboard", 0x01,
                {reinterpret_cast<const uint8_t*>(buf), sizeof(buf)}, nullptr);
        });
        copy_btn->setFixedSize(46, 26);
        copy_btn->setBackgroundColor({0.18f, 0.22f, 0.35f, 1.0f});
        auto btn_row = std::make_shared<FlexRow>(
            std::vector<std::shared_ptr<Widget>>{save_btn, std::make_shared<Spacer>(4, 1), copy_btn});
        auto row = std::make_shared<FlexRow>(
            std::vector<std::shared_ptr<Widget>>{slot_lbl, preview, std::make_shared<Spacer>(1, 1), btn_row});
        row->setGap(6);
        row->setAlignment(Alignment::Center);
        ctx.cs_col->addChild(row);
    }
}

// Network + Wi-Fi (absorbed into Quick Settings). Connection status widgets
// (conn/ip/signal/bandwidth) + rx/tx sparklines + a scan-result list + an inline
// Wi-Fi connect panel (SSID label + password input + Join/Cancel, hidden until a
// secured network's "→" is tapped) + a paged scan loader (do_scan_page, weak-self
// recursive) + a Disconnect button. The connect-form widgets stay builder-local
// (held by wifi_connect_col / captured by do_scan_page); only the status widgets,
// sparklines, scan list, connect col, scan loader, and disconnect button escape
// (they hit the card/broadcast/timer/OnConnect sites). Behaviour identical to the
// former inline block.
void buildNetworkWifiPanel(ShellContext& ctx) {
    ctx.qp_conn_w   = std::make_shared<TextWidget>("○ Not connected", 13.0f);
    ctx.qp_ip_w     = std::make_shared<TextWidget>("", 12.0f);
    ctx.qp_signal_w = std::make_shared<TextWidget>("", 12.0f);
    ctx.qp_bw_w     = std::make_shared<TextWidget>("", 12.0f);
    ctx.qp_ip_w->setColor({0.72f, 0.72f, 0.72f, 1.0f});
    ctx.qp_signal_w->setColor({0.72f, 0.72f, 0.72f, 1.0f});
    ctx.qp_bw_w->setColor({0.72f, 0.72f, 0.72f, 1.0f});

    // Network speed sparklines
    ctx.net_rx_spark_lbl = std::make_shared<TextWidget>("↓ ▁▁▁▁▁▁▁▁▁▁", 11.0f);
    ctx.net_tx_spark_lbl = std::make_shared<TextWidget>("↑ ▁▁▁▁▁▁▁▁▁▁", 11.0f);
    ctx.net_rx_spark_lbl->setFontFamily("monospace");
    ctx.net_tx_spark_lbl->setFontFamily("monospace");
    ctx.net_rx_spark_lbl->setColor({0.45f, 0.85f, 0.65f, 1.0f});
    ctx.net_tx_spark_lbl->setColor({0.85f, 0.65f, 0.45f, 1.0f});

    ctx.qp_net_list = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});

    // Wi-Fi connect panel — shown inline when user taps "→" on a secured network.
    ctx.wifi_connect_col     = std::make_shared<FlexColumn>();
    auto wifi_selected_ssid  = std::make_shared<std::string>();
    auto wifi_ssid_lbl       = std::make_shared<TextWidget>("", 11.0f);
    wifi_ssid_lbl->setColor({0.6f, 0.85f, 1.0f, 1.0f});
    auto wifi_pass_input     = std::make_shared<TextInputWidget>();
    wifi_pass_input->setPlaceholder("Password");
    auto wifi_join_btn = std::make_shared<ButtonWidget>("Join",
        [wifi_connect_col = ctx.wifi_connect_col, wifi_selected_ssid, wifi_pass_input]() {
            if (!g_shell_xbus || wifi_selected_ssid->empty()) return;
            WifiConnectRequest req{};
            std::strncpy(req.ssid_,       wifi_selected_ssid->c_str(), sizeof(req.ssid_) - 1);
            std::strncpy(req.passphrase_, wifi_pass_input->getText().c_str(), sizeof(req.passphrase_) - 1);
            g_shell_xbus->Call("com.freebsdx.Network",
                static_cast<uint32_t>(NetworkOpcode::Connect),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
            wifi_connect_col->setVisibility(false);
        });
    wifi_join_btn->setBackgroundColor({0.18f, 0.45f, 0.22f, 1.0f});
    wifi_join_btn->setFixedSize(60, 28);
    auto wifi_cancel_btn = std::make_shared<ButtonWidget>("Cancel",
        [wifi_connect_col = ctx.wifi_connect_col]() { wifi_connect_col->setVisibility(false); });
    wifi_cancel_btn->setFixedSize(60, 28);
    auto wifi_join_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        wifi_join_btn, std::make_shared<Spacer>(6, 1), wifi_cancel_btn
    });
    {
        std::vector<std::shared_ptr<Widget>> ch{
            wifi_ssid_lbl,
            std::make_shared<Spacer>(1, 4),
            wifi_pass_input,
            std::make_shared<Spacer>(1, 6),
            wifi_join_row,
        };
        for (auto& c : ch) ctx.wifi_connect_col->addChild(c);
    }
    ctx.wifi_connect_col->setBackgroundColor({0.12f, 0.15f, 0.22f, 1.0f});
    ctx.wifi_connect_col->setPadding(8);
    ctx.wifi_connect_col->setVisibility(false);

    // Paged scan loader — uses weak_ptr self-reference to avoid circular shared_ptr
    ctx.do_scan_page = std::make_shared<std::function<void(uint32_t)>>();
    *ctx.do_scan_page = [qp_net_list = ctx.qp_net_list, wifi_connect_col = ctx.wifi_connect_col,
                     wifi_selected_ssid, wifi_ssid_lbl, wifi_pass_input,
                     do_scan_weak = std::weak_ptr<std::function<void(uint32_t)>>(ctx.do_scan_page)](
                        uint32_t page) {
        if (!g_shell_xbus) return;
        WifiScanRequest req{};
        req.page_ = page;
        g_shell_xbus->Call("com.freebsdx.Network",
            static_cast<uint32_t>(NetworkOpcode::Scan),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [qp_net_list, wifi_connect_col, wifi_selected_ssid,
             wifi_ssid_lbl, wifi_pass_input, do_scan_weak, page](const XBusMessage& resp) {
                WifiScanPage pg{};
                std::memcpy(&pg, resp.payload_, sizeof(pg));
                if (pg.page_ == 0) qp_net_list->clearChildren();
                if (pg.valid_) {
                    const auto& net = pg.network_;
                    std::string ssid = std::string(net.ssid_);
                    bool is_secure   = net.is_secure_ != 0;
                    bool is_active   = (!ssid.empty() && ssid == g_wifi_ssid);

                    auto name_lbl = std::make_shared<TextWidget>(
                        signalBars(net.signal_strength_) + "  " + ssid
                        + (is_secure ? " 🔒" : ""), 12.0f);
                    name_lbl->setColor(is_active
                        ? Color{0.3f, 0.9f, 0.5f, 1.0f}
                        : Color{0.88f, 0.88f, 0.88f, 1.0f});

                    auto conn_btn = std::make_shared<ButtonWidget>(is_active ? "✓" : "→",
                        [ssid, is_secure, wifi_connect_col,
                         wifi_selected_ssid, wifi_ssid_lbl, wifi_pass_input]() {
                            if (!g_shell_xbus) return;
                            if (!is_secure) {
                                WifiConnectRequest req{};
                                std::strncpy(req.ssid_, ssid.c_str(), sizeof(req.ssid_) - 1);
                                g_shell_xbus->Call("com.freebsdx.Network",
                                    static_cast<uint32_t>(NetworkOpcode::Connect),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                            } else {
                                *wifi_selected_ssid = ssid;
                                wifi_ssid_lbl->setText("Connect to: " + ssid);
                                wifi_pass_input->setText("");
                                wifi_connect_col->setVisibility(true);
                            }
                        });
                    conn_btn->setFixedSize(28, 24);
                    if (is_active)
                        conn_btn->setBackgroundColor({0.15f, 0.40f, 0.20f, 1.0f});

                    auto row = std::make_shared<FlexRow>(
                        std::vector<std::shared_ptr<Widget>>{
                            name_lbl, std::make_shared<Spacer>(1, 1, true), conn_btn});
                    row->setGap(4);
                    row->setAlignment(Alignment::Center);
                    qp_net_list->addChild(row);
                }
                if (pg.has_more_ && page < 15) {
                    if (auto fn = do_scan_weak.lock()) (*fn)(page + 1);
                }
            });
    };

    ctx.qp_disc_btn = std::make_shared<ButtonWidget>("Disconnect", []() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.Network",
            static_cast<uint32_t>(NetworkOpcode::Disconnect), {}, nullptr);
    });
    ctx.qp_disc_btn->setVisibility(false);
}

// DND Schedule UI. A "Schedule DND" switch + From/To hour steppers (‹ / › around a
// two-digit hour label) + a countdown label. The steppers mutate the file-scope
// g_dnd_sched_start/g_dnd_sched_end ints (by reference) and persist via
// saveDndSched; the switch toggles g_dnd_sched_on. The make_hour_dec/inc helper
// lambdas and the four stepper buttons stay builder-local (held by dnd_sched_times);
// switch/from-lbl/to-lbl/hdr/times/countdown escape. The reply/broadcast reads back
// the globals into the switch + labels. Behaviour identical to the former inline
// block.
void buildDndSchedulePanel(ShellContext& ctx) {
    ctx.dnd_sched_switch = std::make_shared<SwitchWidget>(false);
    ctx.dnd_from_lbl = std::make_shared<TextWidget>("22", 14.0f);
    ctx.dnd_to_lbl   = std::make_shared<TextWidget>("07", 14.0f);
    ctx.dnd_from_lbl->setColor({0.90f, 0.90f, 0.90f, 1.0f});
    ctx.dnd_to_lbl->setColor({0.90f, 0.90f, 0.90f, 1.0f});

    auto make_hour_dec = [](int& hr, std::shared_ptr<TextWidget> lbl) {
        return std::make_shared<ButtonWidget>("‹", [&hr, lbl]() {
            hr = (hr + 23) % 24;
            char buf[4]; std::snprintf(buf, sizeof(buf), "%02d", hr);
            lbl->setText(buf); saveDndSched();
        });
    };
    auto make_hour_inc = [](int& hr, std::shared_ptr<TextWidget> lbl) {
        return std::make_shared<ButtonWidget>("›", [&hr, lbl]() {
            hr = (hr + 1) % 24;
            char buf[4]; std::snprintf(buf, sizeof(buf), "%02d", hr);
            lbl->setText(buf); saveDndSched();
        });
    };

    auto dnd_from_dec = make_hour_dec(g_dnd_sched_start, ctx.dnd_from_lbl);
    auto dnd_from_inc = make_hour_inc(g_dnd_sched_start, ctx.dnd_from_lbl);
    auto dnd_to_dec   = make_hour_dec(g_dnd_sched_end,   ctx.dnd_to_lbl);
    auto dnd_to_inc   = make_hour_inc(g_dnd_sched_end,   ctx.dnd_to_lbl);

    ctx.dnd_sched_switch->setOnChanged([](bool on) {
        g_dnd_sched_on = on; saveDndSched();
    });

    ctx.dnd_sched_hdr = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🕐", 14.0f),
        std::make_shared<Spacer>(8, 1),
        std::make_shared<TextWidget>("Schedule DND", 12.0f),
        std::make_shared<Spacer>(1, 1, true),
        ctx.dnd_sched_switch
    });
    ctx.dnd_sched_times = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<Spacer>(22, 1),
        std::make_shared<TextWidget>("From", 11.0f),
        std::make_shared<Spacer>(6, 1),
        dnd_from_dec, std::make_shared<Spacer>(4, 1),
        ctx.dnd_from_lbl, std::make_shared<Spacer>(4, 1),
        dnd_from_inc,
        std::make_shared<Spacer>(10, 1),
        std::make_shared<TextWidget>("To", 11.0f),
        std::make_shared<Spacer>(6, 1),
        dnd_to_dec, std::make_shared<Spacer>(4, 1),
        ctx.dnd_to_lbl, std::make_shared<Spacer>(4, 1),
        dnd_to_inc,
    });
    ctx.dnd_countdown_lbl = std::make_shared<TextWidget>("", 11.0f);
    ctx.dnd_countdown_lbl->setColor({0.55f, 0.85f, 1.0f, 1.0f});
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

// Airplane Mode (cross-coupled). A switch bound to com.freebsdx.AirplaneMode
// SetEnabled; enabling it immediately clears the sibling Wi-Fi/Bluetooth QS
// switches, and update_ap_ui restores them from the reported state. Those sibling
// switches (qs_wifi_switch/qs_bt_switch) are main()-locals passed by const-ref —
// program lifetime, so capturing copies in the escaping closures is safe.
// Behaviour identical to the former inline block.
void buildAirplaneModePanel(ShellContext& ctx,
                                   const std::shared_ptr<SwitchWidget>& qs_wifi_switch,
                                   const std::shared_ptr<SwitchWidget>& qs_bt_switch) {
    ctx.ap_switch = std::make_shared<SwitchWidget>(false);
    ctx.ap_switch->setOnChanged([ap_switch = ctx.ap_switch, qs_wifi_switch, qs_bt_switch](bool on) {
        if (!g_shell_xbus) return;
        AirplaneModeEnableRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.AirplaneMode",
            static_cast<uint32_t>(AirplaneModeOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        // Immediately reflect disabled radios in the existing QS switches
        if (on) {
            qs_wifi_switch->setValue(false);
            qs_bt_switch->setValue(false);
        }
    });

    ctx.update_ap_ui = [ap_switch = ctx.ap_switch, qs_wifi_switch, qs_bt_switch]
                       (const AirplaneModeState& st) {
        ap_switch->setValue(st.enabled_ != 0);
        if (st.enabled_) {
            qs_wifi_switch->setValue(false);
            qs_bt_switch->setValue(false);
        } else {
            qs_wifi_switch->setValue(st.wifi_was_on_ != 0);
            qs_bt_switch->setValue(st.bt_was_on_ != 0);
        }
    };
}

// Focus-mode toggle (cross-coupled). A single label-button that flips
// g_focus_active: turning it on sets DND (g_dnd + the sibling qs_dnd_switch),
// records the start time, and posts a "Focus Mode On" notification; off restores
// and posts an elapsed-time notification. qs_dnd_switch is a main()-local passed by
// const-ref. focus_lbl stays builder-local (the button's child + captured by its
// onClick). Behaviour identical to the former inline block.
void buildFocusToggleButton(ShellContext& ctx,
                                   const std::shared_ptr<SwitchWidget>& qs_dnd_switch) {
    auto focus_lbl = std::make_shared<TextWidget>("🎯  Focus Mode: OFF", 13.0f);
    focus_lbl->setColor({0.75f, 0.75f, 0.80f, 1.0f});
    ctx.focus_btn = std::make_shared<ButtonWidget>(focus_lbl,
        [focus_lbl, qs_dnd_switch]() {
            g_focus_active = !g_focus_active;
            if (g_focus_active) {
                g_focus_start = std::time(nullptr);
                g_dnd = true;
                qs_dnd_switch->setValue(true);
                focus_lbl->setText("🎯  Focus Mode: ON");
                focus_lbl->setColor({0.40f, 0.85f, 0.50f, 1.0f});
            } else {
                g_dnd = false;
                qs_dnd_switch->setValue(false);
                focus_lbl->setText("🎯  Focus Mode: OFF");
                focus_lbl->setColor({0.75f, 0.75f, 0.80f, 1.0f});
            }
            if (!g_shell_xbus) return;
            NotificationPayload notif{};
            notif.timeout_ms_ = 4000;
            std::strncpy(notif.app_name_, "Focus", sizeof(notif.app_name_) - 1);
            if (g_focus_active) {
                std::strncpy(notif.title_,   "Focus Mode On",
                    sizeof(notif.title_) - 1);
                std::strncpy(notif.message_, "Notifications silenced. Stay focused.",
                    sizeof(notif.message_) - 1);
            } else {
                int elapsed = static_cast<int>(
                    (std::time(nullptr) - g_focus_start) / 60);
                std::strncpy(notif.title_, "Focus Mode Off", sizeof(notif.title_) - 1);
                std::snprintf(notif.message_, sizeof(notif.message_),
                    "Great work! Focused for %d min.", elapsed);
            }
            g_shell_xbus->Call("com.freebsdx.Notification",
                static_cast<uint32_t>(NotificationOpcode::Post),
                {reinterpret_cast<const uint8_t*>(&notif), sizeof(notif)}, nullptr);
        });
    ctx.focus_btn->setPadding(8);
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


void buildKeyboardLayoutPanel(ShellContext& ctx) {
    auto kb_layout_lbl = std::make_shared<TextWidget>("⌨ Layout", 12.0f);
    kb_layout_lbl->setColor({0.75f, 0.75f, 0.80f, 1.0f});
    ctx.kb_active_lbl = std::make_shared<TextWidget>(kLayouts[g_keymap_idx].label, 13.0f);
    ctx.kb_active_lbl->setColor({0.45f, 0.85f, 1.0f, 1.0f});
    ctx.kb_active_lbl->setFontFamily("monospace");
    auto kb_prev_btn = std::make_shared<ButtonWidget>("◀", [kb_active_lbl = ctx.kb_active_lbl]() {
        g_keymap_idx = (g_keymap_idx + kNumLayouts - 1) % kNumLayouts;
        kb_active_lbl->setText(kLayouts[g_keymap_idx].label);
        saveKeymap();
        if (!g_shell_xbus) return;
        KeymapRequest kr{}; std::strncpy(kr.layout_, kLayouts[g_keymap_idx].code, 15);
        g_shell_xbus->Call("com.freebsdx.Keymap",
            static_cast<uint32_t>(KeymapOpcode::SetLayout),
            {reinterpret_cast<const uint8_t*>(&kr), sizeof(kr)}, nullptr);
    });
    kb_prev_btn->setFixedSize(32, 30);
    auto kb_next_btn = std::make_shared<ButtonWidget>("▶", [kb_active_lbl = ctx.kb_active_lbl]() {
        g_keymap_idx = (g_keymap_idx + 1) % kNumLayouts;
        kb_active_lbl->setText(kLayouts[g_keymap_idx].label);
        saveKeymap();
        if (!g_shell_xbus) return;
        KeymapRequest kr{}; std::strncpy(kr.layout_, kLayouts[g_keymap_idx].code, 15);
        g_shell_xbus->Call("com.freebsdx.Keymap",
            static_cast<uint32_t>(KeymapOpcode::SetLayout),
            {reinterpret_cast<const uint8_t*>(&kr), sizeof(kr)}, nullptr);
    });
    kb_next_btn->setFixedSize(32, 30);
    ctx.kb_layout_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        kb_layout_lbl, std::make_shared<Spacer>(1, 1, true),
        kb_prev_btn, std::make_shared<Spacer>(6, 1),
        ctx.kb_active_lbl, std::make_shared<Spacer>(6, 1),
        kb_next_btn
    });
}
