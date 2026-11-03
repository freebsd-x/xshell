// ── xshell panel builders: networking: Wi-Fi, VPN, proxy, DNS, remote access, connections ──
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
