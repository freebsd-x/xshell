// ── xshell broadcast dispatcher ──
// Every XBus broadcast the shell receives (StateChanged from each service,
// notifications, theme, window list, …) is routed here to the panel that
// shows it. It was a 2.6k-line lambda in main() capturing 229 handles by
// value — 30 s of every main.cc compile went into building that closure.
// The handles now live in ShellContext; the aliases below give the body
// (moved verbatim) the names it always used. It only reads them, as the
// non-mutable lambda did.
#include "shell_panels.h"
#include <algorithm>
#include <cstring>
#include <format>
#include <thread>

void dispatchShellBroadcast(ShellContext& ctx, const XBusMessage& msg) {
    auto& ch_overlay = ctx.ch_overlay;
    auto& ch_entries = ctx.ch_entries;
    auto& ch_list_col_ptr = ctx.ch_list_col_ptr;
    auto& rebuild_ch = ctx.rebuild_ch;
    auto& reload_ch = ctx.reload_ch;
    auto& cp_overlay = ctx.cp_overlay;
    auto& search_portal = ctx.search_portal;
    auto& action_center = ctx.action_center;
    auto& notif_history = ctx.notif_history;
    auto& notif_list_col = ctx.notif_list_col;
    auto& notif_empty_lbl = ctx.notif_empty_lbl;
    auto& make_notif_card = ctx.make_notif_card;
    auto& ws_pips = ctx.ws_pips;
    auto& switcher_chips = ctx.switcher_chips;
    auto& switcher_panel = ctx.switcher_panel;
    auto& tbar_chips = ctx.tbar_chips;
    auto& tbar_ids = ctx.tbar_ids;
    auto& apps = ctx.apps;
    auto& net_btn = ctx.net_btn;
    auto& qp_conn_w = ctx.qp_conn_w;
    auto& qp_ip_w = ctx.qp_ip_w;
    auto& qp_signal_w = ctx.qp_signal_w;
    auto& qp_disc_btn = ctx.qp_disc_btn;
    auto& do_scan_page = ctx.do_scan_page;
    auto& wifi_connect_col = ctx.wifi_connect_col;
    auto& rebuild_bt_devices = ctx.rebuild_bt_devices;
    auto& update_bt_ui = ctx.update_bt_ui;
    auto& rebuild_usb = ctx.rebuild_usb;
    auto& ss_status_lbl = ctx.ss_status_lbl;
    auto& ss_last_path = ctx.ss_last_path;
    auto& ss_open_btn = ctx.ss_open_btn;
    auto& ss_recording = ctx.ss_recording;
    auto& rec_toggle_btn = ctx.rec_toggle_btn;
    auto& rec_status_lbl = ctx.rec_status_lbl;
    auto& vpn_status_lbl = ctx.vpn_status_lbl;
    auto& vpn_info_lbl = ctx.vpn_info_lbl;
    auto& vpn_conn_btn = ctx.vpn_conn_btn;
    auto& vpn_disc_btn = ctx.vpn_disc_btn;
    auto& update_vpn_ui = ctx.update_vpn_ui;
    auto& update_rd_ui = ctx.update_rd_ui;
    auto& rebuild_print = ctx.rebuild_print;
    auto& update_hs_ui = ctx.update_hs_ui;
    auto& update_focus_ui = ctx.update_focus_ui;
    auto& update_cast_ui = ctx.update_cast_ui;
    auto& rebuild_pkg = ctx.rebuild_pkg;
    auto& update_ab_ui = ctx.update_ab_ui;
    auto& rebuild_svc = ctx.rebuild_svc;
    auto& update_gm_ui = ctx.update_gm_ui;
    auto& rebuild_sec = ctx.rebuild_sec;
    auto& update_a11y_ui = ctx.update_a11y_ui;
    auto& update_cf_ui = ctx.update_cf_ui;
    auto& rebuild_lsess = ctx.rebuild_lsess;
    auto& update_kb_bl_ui = ctx.update_kb_bl_ui;
    auto& rebuild_bdrain = ctx.rebuild_bdrain;
    auto& update_ap_ui = ctx.update_ap_ui;
    auto& rebuild_cron = ctx.rebuild_cron;
    auto& update_loc_ui = ctx.update_loc_ui;
    auto& rebuild_aout = ctx.rebuild_aout;
    auto& update_stout_ui = ctx.update_stout_ui;
    auto& update_rot_ui = ctx.update_rot_ui;
    auto& update_mic_mute_ui = ctx.update_mic_mute_ui;
    auto& rebuild_startup = ctx.rebuild_startup;
    auto& rebuild_syslog = ctx.rebuild_syslog;
    auto& rebuild_dl = ctx.rebuild_dl;
    auto& update_auto_br_ui = ctx.update_auto_br_ui;
    auto& rebuild_sshkey = ctx.rebuild_sshkey;
    auto& update_rrate_ui = ctx.update_rrate_ui;
    auto& rebuild_envvar = ctx.rebuild_envvar;
    auto& update_scroll_dir_ui = ctx.update_scroll_dir_ui;
    auto& update_pres_ui = ctx.update_pres_ui;
    auto& update_mem_ui = ctx.update_mem_ui;
    auto& update_touchpad_ui = ctx.update_touchpad_ui;
    auto& rebuild_print_queue = ctx.rebuild_print_queue;
    auto& update_reduce_motion_ui = ctx.update_reduce_motion_ui;
    auto& rebuild_pkg_upd = ctx.rebuild_pkg_upd;
    auto& update_cursor_size_ui = ctx.update_cursor_size_ui;
    auto& rebuild_zfs = ctx.rebuild_zfs;
    auto& update_hotspot_ui = ctx.update_hotspot_ui;
    auto& rebuild_jails = ctx.rebuild_jails;
    auto& update_disp_scale_ui = ctx.update_disp_scale_ui;
    auto& rebuild_perms = ctx.rebuild_perms;
    auto& update_dev_overlay_ui = ctx.update_dev_overlay_ui;
    auto& update_ft_ui = ctx.update_ft_ui;
    auto& update_color_prof_ui = ctx.update_color_prof_ui;
    auto& rebuild_im = ctx.rebuild_im;
    auto& update_stylus_ui = ctx.update_stylus_ui;
    auto& rebuild_captures = ctx.rebuild_captures;
    auto& update_ntp_ui = ctx.update_ntp_ui;
    auto& rebuild_be = ctx.rebuild_be;
    auto& update_upd_ch_ui = ctx.update_upd_ch_ui;
    auto& update_text_size_ui = ctx.update_text_size_ui;
    auto& rebuild_kmod = ctx.rebuild_kmod;
    auto& update_tz_ui = ctx.update_tz_ui;
    auto& update_rl_ui = ctx.update_rl_ui;
    auto& update_a11y_ui_b = ctx.update_a11y_ui_b;
    auto& update_proxy_ui = ctx.update_proxy_ui;
    auto& update_dns_ui = ctx.update_dns_ui;
    auto& rebuild_audio_routes = ctx.rebuild_audio_routes;
    auto& update_wl_ui = ctx.update_wl_ui;
    auto& rebuild_shares = ctx.rebuild_shares;
    auto& rebuild_vms = ctx.rebuild_vms;
    auto& rebuild_containers = ctx.rebuild_containers;
    auto& update_nl_ui = ctx.update_nl_ui;
    auto& rebuild_crash = ctx.rebuild_crash;
    auto& update_privacy_ui = ctx.update_privacy_ui;
    auto& update_fw_ui = ctx.update_fw_ui;
    auto& update_pp_ui = ctx.update_pp_ui;
    auto& mp_playing = ctx.mp_playing;
    auto& update_mp_ui = ctx.update_mp_ui;
    auto& rebuild_storage = ctx.rebuild_storage;
    auto& qs_wifi_switch = ctx.qs_wifi_switch;
    auto& qs_wifi_sub = ctx.qs_wifi_sub;
    auto& qs_bt_switch = ctx.qs_bt_switch;
    auto& bt_btn = ctx.bt_btn;
    auto& qs_dark_switch = ctx.qs_dark_switch;
    auto& load_tray = ctx.load_tray;
    auto& weather_btn = ctx.weather_btn;
    auto& fc_overlay = ctx.fc_overlay;
    auto& fc_request_id = ctx.fc_request_id;
    auto& fc_current_path = ctx.fc_current_path;
    auto& fc_filter_mime = ctx.fc_filter_mime;
    auto& fc_save_mode = ctx.fc_save_mode;
    auto& fc_title_w = ctx.fc_title_w;
    auto& fc_ok_btn = ctx.fc_ok_btn;
    auto& fc_save_field = ctx.fc_save_field;
    auto& fc_sel_w = ctx.fc_sel_w;
    auto& fc_selected = ctx.fc_selected;
    auto& rebuild_fc = ctx.rebuild_fc;
    auto& wallpaper_img_slot = ctx.wallpaper_img_slot;
    auto& audio = ctx.audio;
    auto& show_osd = ctx.show_osd;
    auto& notif_unread = ctx.notif_unread;
    auto& notif_badge_lbl = ctx.notif_badge_lbl;
    auto& notif_badge_box = ctx.notif_badge_box;
    auto& update_battery = ctx.update_battery;
    auto& shortcuts_overlay = ctx.shortcuts_overlay;
    auto& qs_ns_switch = ctx.qs_ns_switch;
    auto& qs_ns_slider = ctx.qs_ns_slider;
    auto& qs_ns_val = ctx.qs_ns_val;
    auto& known_apps = ctx.known_apps;
    auto& muted_apps = ctx.muted_apps;
    auto& rebuild_app_list = ctx.rebuild_app_list;
    auto& disp_col = ctx.disp_col;
    auto& rebuild_disp = ctx.rebuild_disp;
    auto& qs_dnd_switch = ctx.qs_dnd_switch;
    auto& update_upd_btn = ctx.update_upd_btn;
    auto& qs_autolock_switch = ctx.qs_autolock_switch;
    auto& notes_overlay = ctx.notes_overlay;
    auto& rebuild_notes_mini = ctx.rebuild_notes_mini;
    auto& rebuild_dnd_exc = ctx.rebuild_dnd_exc;
    auto& rebuild_screen_time = ctx.rebuild_screen_time;
    auto& rebuild_audio_vol = ctx.rebuild_audio_vol;
    auto& win_rules_col = ctx.win_rules_col;
    auto& rebuild_win_rules = ctx.rebuild_win_rules;
    auto& hotkey_status_lbl = ctx.hotkey_status_lbl;
    auto& rebuild_hotkeys = ctx.rebuild_hotkeys;
    auto& ws_wp_update_hl = ctx.ws_wp_update_hl;
    auto& forecast_rebuild = ctx.forecast_rebuild;
    auto& live_titles = ctx.live_titles;
    auto& dnd_countdown_lbl = ctx.dnd_countdown_lbl;
    auto& kb_active_lbl = ctx.kb_active_lbl;
    auto& rebuild_tz = ctx.rebuild_tz;
    auto& rebuild_usbdev = ctx.rebuild_usbdev;
    auto& rebuild_slog = ctx.rebuild_slog;
    auto& rebuild_ns = ctx.rebuild_ns;
    auto& update_dns_cfg_ui = ctx.update_dns_cfg_ui;
    auto& update_nsch_ui = ctx.update_nsch_ui;
    auto& rebuild_sandbox = ctx.rebuild_sandbox;
    auto& update_slk_ui = ctx.update_slk_ui;
    auto& rebuild_btaudio = ctx.rebuild_btaudio;
    auto& update_loc_policy = ctx.update_loc_policy;
    auto& rebuild_loc = ctx.rebuild_loc;
    auto& rebuild_totp = ctx.rebuild_totp;
    auto& update_eq_ui = ctx.update_eq_ui;
    auto& rebuild_bkp = ctx.rebuild_bkp;
    auto& update_ptr_ui = ctx.update_ptr_ui;
    auto& rebuild_rem = ctx.rebuild_rem;
    auto& update_sa_ui = ctx.update_sa_ui;
    auto& rebuild_xfr = ctx.rebuild_xfr;
    auto& update_sndfx_ui = ctx.update_sndfx_ui;
    auto& rebuild_gc = ctx.rebuild_gc;
    auto& update_captions_ui = ctx.update_captions_ui;
    auto& rebuild_swifi = ctx.rebuild_swifi;
    auto& update_dsaver_ui = ctx.update_dsaver_ui;
    auto& rebuild_seckey = ctx.rebuild_seckey;
    auto& update_fastchg_ui = ctx.update_fastchg_ui;
    auto& rebuild_txtrep = ctx.rebuild_txtrep;
    auto& update_silence_ui = ctx.update_silence_ui;
    auto& rebuild_applimit = ctx.rebuild_applimit;
    auto& update_bhealth_ui = ctx.update_bhealth_ui;
    auto& rebuild_notifsnd = ctx.rebuild_notifsnd;
    auto& update_ttone_ui = ctx.update_ttone_ui;
    auto& rebuild_litem = ctx.rebuild_litem;
    auto& update_ostore_ui = ctx.update_ostore_ui;
    auto& rebuild_tdev = ctx.rebuild_tdev;
    auto& update_monoaud_ui = ctx.update_monoaud_ui;
    auto& rebuild_kbsc = ctx.rebuild_kbsc;
    auto& update_stagemgr_ui = ctx.update_stagemgr_ui;
    auto& rebuild_dapp = ctx.rebuild_dapp;
    auto& arch_status_lbl = ctx.arch_status_lbl;
    auto& iapps_prog_lbl = ctx.iapps_prog_lbl;
    auto& rdp_status_lbl = ctx.rdp_status_lbl;
    auto& rebuild_cam_dev = ctx.rebuild_cam_dev;
    auto& rebuild_clip = ctx.rebuild_clip;
    auto& rebuild_disk_usage = ctx.rebuild_disk_usage;
    auto& rebuild_disp_arr = ctx.rebuild_disp_arr;
    auto& rebuild_file_sync = ctx.rebuild_file_sync;
    auto& rebuild_fonts = ctx.rebuild_fonts;
    auto& rebuild_iapps = ctx.rebuild_iapps;
    auto& rebuild_indev = ctx.rebuild_indev;
    auto& rebuild_keychain = ctx.rebuild_keychain;
    auto& rebuild_rdp = ctx.rebuild_rdp;
    auto& rebuild_scap = ctx.rebuild_scap;
    auto& rebuild_seats = ctx.rebuild_seats;
    auto& rebuild_store = ctx.rebuild_store;
    auto& rebuild_term_sess = ctx.rebuild_term_sess;
    auto& rebuild_uev = ctx.rebuild_uev;
    auto& unlock_inputs_row = ctx.unlock_inputs_row;
    auto& unlock_locked = ctx.unlock_locked;
    auto& unlock_status_lbl = ctx.unlock_status_lbl;
    auto& update_a11y_ui_c = ctx.update_a11y_ui_c;
    auto& update_dock_ui = ctx.update_dock_ui;
    auto& update_gest_ui = ctx.update_gest_ui;
    auto& update_gpu_ui = ctx.update_gpu_ui;
    auto& update_locale_ui = ctx.update_locale_ui;
    auto& update_pkgsign_ui = ctx.update_pkgsign_ui;
    auto& update_ss_ui = ctx.update_ss_ui;
    auto& update_xwl_ui = ctx.update_xwl_ui;
    auto& store_search_input = ctx.store_search_input;

            // Per-workspace wallpaper change — update background if it's the active workspace
            if (std::strcmp(msg.service_name_, "com.freebsdx.Wallpaper") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(WallpaperOpcode::SetWorkspaceWallpaper)) {
                WallpaperWorkspaceRequest req{};
                std::memcpy(&req, msg.payload_, sizeof(req));
                if (static_cast<int>(req.workspace_idx_) == g_current_ws) {
                    wallpaper_img_slot->clearChildren();
                    if (req.path_[0]) {
                        static const ImageFitMode kFitMap[] = {
                            ImageFitMode::Cover, ImageFitMode::Contain,
                            ImageFitMode::Stretch, ImageFitMode::Center
                        };
                        auto fit = kFitMap[req.fit_mode_ < 4 ? req.fit_mode_ : 0];
                        auto img = std::make_shared<ImageWidget>(std::string(req.path_));
                        img->setFixedSize(10000.0f, 10000.0f);
                        img->setFitMode(fit);
                        wallpaper_img_slot->addChild(img);
                    }
                }
                return;
            }

            // Wallpaper change — swap image in the background layer
            if (std::strcmp(msg.service_name_, "com.freebsdx.Wallpaper") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(WallpaperOpcode::SetWallpaper)) {
                WallpaperSetRequest req{};
                std::memcpy(&req, msg.payload_, sizeof(req));
                wallpaper_img_slot->clearChildren();
                if (req.path_[0]) {
                    static const ImageFitMode kFitMap[] = {
                        ImageFitMode::Cover, ImageFitMode::Contain,
                        ImageFitMode::Stretch, ImageFitMode::Center
                    };
                    auto fit = kFitMap[req.fit_mode_ < 4 ? req.fit_mode_ : 0];
                    auto img = std::make_shared<ImageWidget>(std::string(req.path_));
                    img->setFixedSize(10000.0f, 10000.0f);
                    img->setFitMode(fit);
                    wallpaper_img_slot->addChild(img);
                }
                return;
            }

            if (std::strcmp(msg.service_name_, "com.freebsdx.Tray") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TrayOpcode::TrayChanged)) {
                if (load_tray && *load_tray) (*load_tray)(0);
                return;
            }

            // Bluetooth state changes — update qs_bt_switch, bt_btn, and device list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Bluetooth") == 0 &&
                (msg.opcode_ == static_cast<uint32_t>(BluetoothOpcode::DeviceChanged) ||
                 msg.opcode_ == static_cast<uint32_t>(BluetoothOpcode::DeviceFound))) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.Bluetooth",
                        static_cast<uint32_t>(BluetoothOpcode::GetState), {},
                        [qs_bt_switch, bt_btn](const XBusMessage& r) {
                            BluetoothState st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            qs_bt_switch->setValue(st.enabled_ != 0);
                            bt_btn->setText(st.enabled_ ? "🔵" : "BT");
                        });
                    if (rebuild_bt_devices && *rebuild_bt_devices) (*rebuild_bt_devices)();
                }
                return;
            }

            // Network status changes — update taskbar indicator + quick panel header
            if (std::strcmp(msg.service_name_, "com.freebsdx.Network") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NetworkOpcode::NetworkChanged)) {
                NetworkStatus n{};
                std::memcpy(&n, msg.payload_, sizeof(n));
                // Update taskbar button text
                if (!n.is_connected_) {
                    net_btn->setText("✕");
                    qp_conn_w->setText("○ Not connected");
                    qp_ip_w->setText("");
                    qp_signal_w->setText("");
                    qp_disc_btn->setVisibility(false);
                } else {
                    std::string bars = n.current_ssid_[0]
                        ? signalBars(n.signal_strength_)
                        : "⇄";
                    std::string iface_label = n.current_ssid_[0]
                        ? std::string(n.current_ssid_)
                        : "ETH";
                    net_btn->setText(bars + " " + iface_label);
                    qp_conn_w->setText("● " + iface_label);
                    qp_ip_w->setText(std::string(n.ip_address_));
                    if (n.current_ssid_[0])
                        qp_signal_w->setText(
                            signalBars(n.signal_strength_) +
                            std::format("  {} dBm", n.signal_strength_));
                    else
                        qp_signal_w->setText("");
                    qp_disc_btn->setVisibility(true);
                }
                // Update Wi-Fi switch, sub-label, and tracked SSID
                g_wifi_ssid = n.is_connected_ && n.current_ssid_[0]
                    ? std::string(n.current_ssid_) : "";
                qs_wifi_switch->setValue(n.is_connected_);
                qs_wifi_sub->setText(n.is_connected_ ? std::string(n.ip_address_) : "Not connected");
                // Refresh scan list so active network highlights update
                if (do_scan_page && *do_scan_page) (*do_scan_page)(0);
                return;
            }

            // Notification XBusMessages have the service name set
            if (std::strcmp(msg.service_name_, "com.freebsdx.Notification") == 0) {
                if (msg.opcode_ == static_cast<uint32_t>(NotificationOpcode::Post)) {
                    NotificationPayload payload{};
                    std::memcpy(&payload, msg.payload_, sizeof(payload));
                    // DND: suppress unless app is in priority exceptions list
                    if (g_dnd && g_dnd_exceptions.find(std::string(payload.app_name_)) == g_dnd_exceptions.end()) {
                        ++g_dnd_suppressed_count;
                        {
                            std::lock_guard<std::mutex> dlk(g_dnd_digest_mtx);
                            if (payload.app_name_[0])
                                ++g_dnd_suppressed_apps[std::string(payload.app_name_)];
                        }
                        return;
                    }
                    audio->playChime();
                    // Badge: increment unread count on the notification bell
                    ++(*notif_unread);
                    notif_badge_lbl->setText(std::to_string(*notif_unread));
                    notif_badge_box->setVisibility(true);

                    // Timestamp
                    auto t = std::chrono::system_clock::to_time_t(
                        std::chrono::system_clock::now());
                    char ts_buf[8];
                    std::strftime(ts_buf, sizeof(ts_buf), "%H:%M", std::localtime(&t));

                    auto card = make_notif_card(payload.id_,
                        std::string(payload.title_), std::string(payload.message_), ts_buf,
                        std::string(payload.app_name_));

                    // Cap history at 50 — drop oldest visible card from list
                    if (notif_history->size() >= 50) {
                        auto& oldest = notif_history->front();
                        oldest.card->setVisibility(false);
                        notif_history->erase(notif_history->begin());
                    }

                    NotifEntry entry;
                    entry.id       = payload.id_;
                    entry.title    = payload.title_;
                    entry.message  = payload.message_;
                    entry.time_str = ts_buf;
                    entry.card     = card;
                    notif_history->push_back(entry);

                    // Append to scroll list (newest at bottom)
                    notif_list_col->addChild(card);
                    notif_empty_lbl->setVisibility(false);
                    action_center->setVisibility(true);

                    // Track app name for per-app notification settings and DND exceptions
                    if (payload.app_name_[0]) {
                        std::string aname(payload.app_name_);
                        bool found = false;
                        for (const auto& a : *known_apps)
                            if (a == aname) { found = true; break; }
                        if (!found) {
                            known_apps->push_back(aname);
                            if (*rebuild_app_list) (*rebuild_app_list)();
                            if (*rebuild_dnd_exc)  (*rebuild_dnd_exc)();
                        }
                    }

                } else if (msg.opcode_ == static_cast<uint32_t>(NotificationOpcode::Dismiss)) {
                    uint32_t id = 0;
                    std::memcpy(&id, msg.payload_, sizeof(id));
                    for (auto& e : *notif_history) {
                        if (e.id == id) { e.dismissed = true; e.card->setVisibility(false); break; }
                    }
                    bool any = false;
                    for (const auto& e : *notif_history) if (!e.dismissed) { any = true; break; }
                    notif_empty_lbl->setVisibility(!any);
                }
                return;
            }

            // Shell service broadcasts
            if (std::strcmp(msg.service_name_, "com.freebsdx.Shell") == 0) {
                if (msg.opcode_ == static_cast<uint32_t>(ShellOpcode::LaunchWithFile)) {
                    ShellLaunchRequest req{};
                    std::memcpy(&req, msg.payload_, sizeof(req));
                    std::string app_id(req.app_id_);
                    std::string file_path(req.file_path_);
                    for (const auto& app : apps) {
                        if (app.identifier == app_id) {
                            // A running viewer takes this broadcast itself.
                            if (!runningWindowTakesFile(app.executable_path, file_path))
                                launchApp(app.executable_path, file_path);
                            // Record in recent files
                            if (!file_path.empty()) {
                                std::string fname = file_path.substr(
                                    file_path.find_last_of("/\\") + 1);
                                g_recent_files.erase(
                                    std::remove_if(g_recent_files.begin(), g_recent_files.end(),
                                        [&file_path](const auto& e){ return e.second == file_path; }),
                                    g_recent_files.end());
                                g_recent_files.insert(g_recent_files.begin(), {fname, file_path});
                                if (g_recent_files.size() > 20) g_recent_files.resize(20);
                                saveRecentFiles();
                            }
                            break;
                        }
                    }
                    return;
                }
                if (msg.opcode_ == static_cast<uint32_t>(ShellOpcode::WorkspaceChanged)) {
                    WorkspaceChangedPayload payload{};
                    std::memcpy(&payload, msg.payload_, sizeof(payload));
                    for (int i = 0; i < kNumWorkspaces; ++i) {
                        (*ws_pips)[i]->setColor(
                            i == static_cast<int>(payload.index_)
                                ? Color{1.0f, 1.0f, 1.0f, 1.0f}
                                : Color{0.35f, 0.35f, 0.4f, 1.0f});
                    }
                    // Track current workspace, update WS wallpaper strip highlight, load wallpaper
                    g_current_ws = static_cast<int>(payload.index_);
                    if (*ws_wp_update_hl) (*ws_wp_update_hl)();
                    if (g_shell_xbus) {
                        WallpaperWorkspaceRequest ws_req{};
                        ws_req.workspace_idx_ = payload.index_;
                        g_shell_xbus->Call("com.freebsdx.Wallpaper",
                            static_cast<uint32_t>(WallpaperOpcode::GetWorkspaceWallpaper),
                            {reinterpret_cast<const uint8_t*>(&ws_req), sizeof(ws_req)},
                            [wallpaper_img_slot](const XBusMessage& resp) {
                                WallpaperWorkspaceRequest r{};
                                std::memcpy(&r, resp.payload_, sizeof(r));
                                wallpaper_img_slot->clearChildren();
                                if (r.path_[0]) {
                                    static const ImageFitMode kFitMap[] = {
                                        ImageFitMode::Cover, ImageFitMode::Contain,
                                        ImageFitMode::Stretch, ImageFitMode::Center
                                    };
                                    auto fit = kFitMap[r.fit_mode_ < 4 ? r.fit_mode_ : 0];
                                    auto img = std::make_shared<ImageWidget>(std::string(r.path_));
                                    img->setFixedSize(10000.0f, 10000.0f);
                                    img->setFitMode(fit);
                                    wallpaper_img_slot->addChild(img);
                                }
                            });
                    }
                } else if (msg.opcode_ == static_cast<uint32_t>(ShellOpcode::SwitcherUpdate)) {
                    SwitcherPayload payload{};
                    std::memcpy(&payload, msg.payload_, sizeof(payload));
                    for (int i = 0; i < kMaxSwitcherWindows; ++i) {
                        if (i < static_cast<int>(payload.count_)) {
                            auto& chip = (*switcher_chips)[i];
                            chip.surface_id = payload.entries_[i].surface_id_;
                            chip.label->setText(std::string(payload.entries_[i].title_));
                            bool focused = (payload.entries_[i].surface_id_ == payload.focused_id_);
                            chip.card->setBackgroundColor(focused
                                ? Color{0.22f, 0.44f, 0.88f, 0.97f}
                                : Color{0.2f, 0.2f, 0.28f, 0.95f});
                            chip.card->setVisibility(true);
                        } else {
                            (*switcher_chips)[i].card->setVisibility(false);
                        }
                    }
                    switcher_panel->setVisibility(true);
                } else if (msg.opcode_ == static_cast<uint32_t>(ShellOpcode::SwitcherDismiss)) {
                    for (auto& chip : *switcher_chips) chip.card->setVisibility(false);
                    switcher_panel->setVisibility(false);
                } else if (msg.opcode_ == static_cast<uint32_t>(ShellOpcode::WindowListUpdate)) {
                    WindowListPayload payload{};
                    std::memcpy(&payload, msg.payload_, sizeof(payload));
                    // Update live title list for session save
                    live_titles->clear();
                    for (uint32_t ti = 0; ti < payload.count_ && ti < 7; ++ti)
                        if (payload.entries_[ti].title_[0])
                            live_titles->emplace_back(payload.entries_[ti].title_);
                    for (int i = 0; i < kMaxTaskbarWindows; ++i) {
                        if (i < static_cast<int>(payload.count_)) {
                            (*tbar_ids)[i] = payload.entries_[i].surface_id_;
                            (*tbar_chips)[i].label->setText(
                                std::string(payload.entries_[i].title_));
                            (*tbar_chips)[i].card->setVisibility(true);

                            // Apply window rules to newly-seen surfaces
                            uint32_t sid = payload.entries_[i].surface_id_;
                            if (sid && g_rule_applied_ids.find(sid) == g_rule_applied_ids.end()) {
                                g_rule_applied_ids.insert(sid);
                                std::string title(payload.entries_[i].title_);
                                std::string title_lc = title;
                                std::transform(title_lc.begin(), title_lc.end(),
                                               title_lc.begin(), ::tolower);
                                for (const auto& rule : g_win_rules) {
                                    std::string kw = rule.title_match;
                                    std::transform(kw.begin(), kw.end(), kw.begin(), ::tolower);
                                    if (title_lc.find(kw) == std::string::npos) continue;
                                    if (rule.maximize && g_shell_xbus)
                                        g_shell_xbus->Call("com.freebsdx.Window",
                                            static_cast<uint32_t>(WindowOpcode::Maximize),
                                            {reinterpret_cast<const uint8_t*>(&sid), sizeof(sid)},
                                            nullptr);
                                    if (rule.always_on_top && g_shell_xbus) {
                                        WinAlwaysOnTopRequest atr{};
                                        atr.surface_id_ = sid; atr.enabled_ = 1u;
                                        g_shell_xbus->Call("com.freebsdx.Window",
                                            static_cast<uint32_t>(WindowOpcode::SetAlwaysOnTop),
                                            {reinterpret_cast<const uint8_t*>(&atr), sizeof(atr)},
                                            nullptr);
                                    }
                                    if (rule.workspace >= 0 && g_shell_xbus) {
                                        WinWorkspaceRequest wsr{};
                                        wsr.surface_id_    = sid;
                                        wsr.workspace_idx_ = static_cast<uint32_t>(rule.workspace);
                                        g_shell_xbus->Call("com.freebsdx.Window",
                                            static_cast<uint32_t>(WindowOpcode::SetWorkspace),
                                            {reinterpret_cast<const uint8_t*>(&wsr), sizeof(wsr)},
                                            nullptr);
                                    }
                                    break; // first matching rule wins
                                }
                            }
                        } else {
                            (*tbar_ids)[i] = 0;
                            (*tbar_chips)[i].card->setVisibility(false);
                        }
                    }
                    // Prune stale surface IDs (keep set bounded)
                    if (g_rule_applied_ids.size() > 256) {
                        auto it = g_rule_applied_ids.begin();
                        std::advance(it, g_rule_applied_ids.size() - 128);
                        g_rule_applied_ids.erase(g_rule_applied_ids.begin(), it);
                    }
                }
                return;
            }

            // File Chooser — service asks xshell to present a native picker
            if (std::strcmp(msg.service_name_, "com.freebsdx.FileChooser") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(FileChooserOpcode::Show)) {
                FileChooserRequest req{};
                std::memcpy(&req, msg.payload_, sizeof(req));
                req.title_[sizeof(req.title_) - 1] = '\0';
                req.filter_mime_[sizeof(req.filter_mime_) - 1] = '\0';
                req.initial_path_[sizeof(req.initial_path_) - 1] = '\0';

                // One picker at a time: whoever was waiting gets an answer.
                if (fc_overlay->isVisible() && *fc_request_id != 0 && g_shell_xbus) {
                    FileChooserResponse gone{};
                    gone.opcode_     = static_cast<uint32_t>(FileChooserOpcode::Respond);
                    gone.request_id_ = *fc_request_id;
                    gone.cancelled_  = 1;
                    g_shell_xbus->Call("com.freebsdx.FileChooser",
                        static_cast<uint32_t>(FileChooserOpcode::Respond),
                        {reinterpret_cast<const uint8_t*>(&gone), sizeof(gone)}, nullptr);
                }

                *fc_request_id = req.request_id_;
                *fc_filter_mime = req.filter_mime_;
                *fc_save_mode   = (req.save_mode_ != 0);

                fc_title_w->setText(req.title_[0] ? std::string(req.title_)
                                                   : (*fc_save_mode ? "Save File" : "Open File"));
                fc_ok_btn->setText(*fc_save_mode ? "Save" : "Open");
                fc_save_field->setVisibility(*fc_save_mode);
                fc_save_field->setText("");

                std::string start = req.initial_path_[0]
                    ? std::string(req.initial_path_)
                    : (std::getenv("HOME") ? std::string(std::getenv("HOME")) : "/");
                *fc_current_path = start;
                *fc_selected = "";
                fc_sel_w->setText("");

                if (rebuild_fc && *rebuild_fc) (*rebuild_fc)(start);
                fc_overlay->setVisibility(true);
                return;
            }

            // Reset idle counter on actual user input (key, mouse, touch, scroll)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Shell") == 0) {
                const auto* im = reinterpret_cast<const InputMessage*>(msg.payload_);
                if (im->type_ == InputEventType::MouseMove   ||
                    im->type_ == InputEventType::MouseButton ||
                    im->type_ == InputEventType::KeyPress    ||
                    im->type_ == InputEventType::MouseWheel) {
                    g_idle_ticks.store(0);
                }
            }

            // Raw InputMessage broadcasts (SearchToggle, DashboardToggle, ThemeChanged …)
            const auto* input = reinterpret_cast<const InputMessage*>(msg.payload_);
            // Key capture for hotkey remapper — intercept before other handlers
            if (input->type_ == InputEventType::KeyPress && input->value_b_ == 1) {
                int cap = g_hotkey_capture_action.load();
                if (cap >= 0 && cap < kNumHotkeys) {
                    g_hotkey_capture_action.store(-1);
                    uint32_t kc   = static_cast<uint32_t>(input->value_a_);
                    uint32_t mods = input->modifiers_;
                    uint32_t aid  = kHotkeyDefs[cap].action_id;
                    g_hotkey_overrides[aid] = {kc, mods};
                    saveHotkeys();
                    hotkey_status_lbl->setText("Bound: " + fmtHotkey(kc, mods));
                    if (*rebuild_hotkeys) (*rebuild_hotkeys)();
                    if (g_shell_xbus) {
                        HotkeyRequest hkr{};
                        hkr.action_id_  = aid;
                        hkr.keycode_    = kc;
                        hkr.modifiers_  = mods;
                        g_shell_xbus->Call("com.freebsdx.Shell",
                            static_cast<uint32_t>(ShellOpcode::RegisterHotkey),
                            {reinterpret_cast<const uint8_t*>(&hkr), sizeof(hkr)}, nullptr);
                    }
                }
            }

            if (input->type_ == InputEventType::ThemeChanged) {
                g_accent_idx = static_cast<uint32_t>(input->value_b_);
                g_shell_dark = input->value_a_ != 0;
                qs_dark_switch->setValue(input->value_a_ != 0);
            } else if (input->type_ == InputEventType::SearchToggle) {
                search_portal->setVisibility(!search_portal->isVisible());
            } else if (input->type_ == InputEventType::DashboardToggle) {
                action_center->setVisibility(!action_center->isVisible());
            } else if (input->type_ == InputEventType::ClipboardHistoryToggle) {
                ch_overlay->setVisibility(!ch_overlay->isVisible());
            } else if (input->type_ == InputEventType::ColorPickerToggle) {
                cp_overlay->setVisibility(!cp_overlay->isVisible());
            } else if (input->type_ == InputEventType::ShortcutHelpToggle) {
                shortcuts_overlay->setVisibility(!shortcuts_overlay->isVisible());
            } else if (input->type_ == InputEventType::QuickNoteToggle) {
                notes_overlay->setVisibility(!notes_overlay->isVisible());
            }
            // Notes changed broadcast — reload mini-grid in action center
            if (std::strcmp(msg.service_name_, "com.freebsdx.Notes") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NotesOpcode::NoteChanged)) {
                if (g_shell_xbus) {
                    NotesPageRequest notes_req{};
                    notes_req.page_ = 0;
                    g_shell_xbus->Call("com.freebsdx.Notes",
                        static_cast<uint32_t>(NotesOpcode::List),
                        {reinterpret_cast<const uint8_t*>(&notes_req), sizeof(notes_req)},
                        [rebuild_notes_mini](const XBusMessage& r) {
                            NotesListPage pg{};
                            std::memcpy(&pg, r.payload_, sizeof(pg));
                            std::vector<NoteEntry> entries;
                            if (pg.count_ > 0) entries.push_back(pg.entry_);
                            if (*rebuild_notes_mini) (*rebuild_notes_mini)(entries);
                        });
                }
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.ClipboardHistory") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ClipboardHistoryOpcode::HistoryChanged)) {
                if (*reload_ch) (*reload_ch)();
            }
            // Screenshot taken — update action center panel + show notification
            if (std::strcmp(msg.service_name_, "com.freebsdx.Screenshot") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScreenshotOpcode::ScreenshotTaken)) {
                ScreenshotResponse resp{};
                std::memcpy(&resp, msg.payload_, sizeof(resp));
                if (resp.success_) {
                    // Update action center status
                    *ss_last_path = std::string(resp.path_);
                    std::string fname = *ss_last_path;
                    auto sl = fname.find_last_of("/\\");
                    if (sl != std::string::npos) fname = fname.substr(sl + 1);
                    ss_status_lbl->setText(fname);
                    ss_open_btn->setVisibility(true);
                    if (g_shell_xbus) {
                        NotificationPayload notif{};
                        notif.timeout_ms_ = 4000;
                        std::strncpy(notif.title_,   "Screenshot saved",  sizeof(notif.title_) - 1);
                        std::strncpy(notif.message_,  resp.path_,         sizeof(notif.message_) - 1);
                        std::strncpy(notif.app_name_, "Screenshot",       sizeof(notif.app_name_) - 1);
                        g_shell_xbus->Call("com.freebsdx.Notification",
                            static_cast<uint32_t>(NotificationOpcode::Post),
                            {reinterpret_cast<const uint8_t*>(&notif), sizeof(notif)}, nullptr);
                    }
                } else {
                    ss_status_lbl->setText("Capture failed");
                }
                return;
            }

            // Recording state changed — update toggle button and status label
            if (std::strcmp(msg.service_name_, "com.freebsdx.Screenshot") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScreenshotOpcode::RecordingChanged)) {
                ScreenshotRecordingState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                *ss_recording = st.recording_ != 0;
                rec_toggle_btn->setText(*ss_recording ? "■ Stop" : "⏺ Record");
                rec_toggle_btn->setBackgroundColor(*ss_recording
                    ? Color{0.6f, 0.12f, 0.12f, 1.0f}
                    : Color{0.32f, 0.12f, 0.12f, 1.0f});
                if (*ss_recording) {
                    rec_status_lbl->setText(std::format("● {} fps", st.fps_));
                } else {
                    // Recording stopped — show output path filename
                    if (st.path_[0]) {
                        std::string fp = std::string(st.path_);
                        auto sl = fp.find_last_of("/\\");
                        if (sl != std::string::npos) fp = fp.substr(sl + 1);
                        rec_status_lbl->setText("Saved: " + fp);
                    } else {
                        rec_status_lbl->setText("");
                    }
                }
                return;
            }

            // Media player state changed — update QS panel controls
            if (std::strcmp(msg.service_name_, "com.freebsdx.MediaPlayer") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(MediaPlayerOpcode::StateChanged)) {
                MediaPlayerState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_mp_ui(st);
                return;
            }

            // Disk topology or mount changed — refresh storage quick-view
            if (std::strcmp(msg.service_name_, "com.freebsdx.Disk") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DiskOpcode::DiskChanged)) {
                if (rebuild_storage && *rebuild_storage) (*rebuild_storage)();
                return;
            }

            // VPN tunnel changed — requery state and update UI
            if (std::strcmp(msg.service_name_, "com.freebsdx.VPN") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(VpnOpcode::VpnChanged)) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.VPN",
                        static_cast<uint32_t>(VpnOpcode::GetStatus), {},
                        [update_vpn_ui](const XBusMessage& r) {
                            VpnStatus st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_vpn_ui(st);
                        });
                }
                return;
            }

            // Focus Mode session started, updated (remaining time tick), or ended
            if (std::strcmp(msg.service_name_, "com.freebsdx.FocusMode") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(FocusModeOpcode::StateChanged)) {
                FocusModeState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_focus_ui(st);
                return;
            }

            // Cast session started, stopped, or receiver changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.Cast") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CastOpcode::StateChanged)) {
                CastState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_cast_ui(st);
                return;
            }

            // Package update started or finished — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Packages") == 0 &&
                (msg.opcode_ == static_cast<uint32_t>(PackageOpcode::UpdateStarted) ||
                 msg.opcode_ == static_cast<uint32_t>(PackageOpcode::UpdateFinished))) {
                if (rebuild_pkg && *rebuild_pkg) (*rebuild_pkg)();
                return;
            }

            // Ad-Blocker status changed (enabled/disabled or list refresh completed)
            if (std::strcmp(msg.service_name_, "com.freebsdx.AdBlock") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AdBlockOpcode::StatusChanged)) {
                AdBlockStatus st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_ab_ui(st);
                return;
            }

            // Service daemon started or stopped
            if (std::strcmp(msg.service_name_, "com.freebsdx.Services") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ServiceOpcode::ServiceChanged)) {
                if (rebuild_svc && *rebuild_svc) (*rebuild_svc)();
                return;
            }

            // Accessibility settings changed from another app or schedule
            if (std::strcmp(msg.service_name_, "com.freebsdx.A11ySettings") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AccessibilitySettingsOpcode::SettingsChanged)) {
                AccessibilityQuickSettings st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_a11y_ui(st);
                return;
            }

            // Download progress update / completion / failure — refresh list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Downloads") == 0 &&
                (msg.opcode_ == static_cast<uint32_t>(DownloadOpcode::DownloadProgress) ||
                 msg.opcode_ == static_cast<uint32_t>(DownloadOpcode::DownloadComplete) ||
                 msg.opcode_ == static_cast<uint32_t>(DownloadOpcode::DownloadFailed))) {
                if (rebuild_dl && *rebuild_dl) (*rebuild_dl)();
                return;
            }

            // Game Mode toggled or foreground app changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.GameMode") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(GameModeOpcode::StateChanged)) {
                GameModeState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_gm_ui(st);
                return;
            }

            // New security event (SSH / sudo / login / fail)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Security") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SecurityOpcode::EventAdded)) {
                if (rebuild_sec && *rebuild_sec) (*rebuild_sec)();
                return;
            }

            // Night Light state changed (schedule triggered or user toggled from another app)
            if (std::strcmp(msg.service_name_, "com.freebsdx.NightLight") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NightLightOpcode::StateChanged)) {
                NightLightState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_nl_ui(st);
                return;
            }

            // New core dump arrived — refresh crash reporter list
            if (std::strcmp(msg.service_name_, "com.freebsdx.CrashReporter") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CrashOpcode::ReportAdded)) {
                if (rebuild_crash && *rebuild_crash) (*rebuild_crash)();
                return;
            }

            // Hotspot AP started/stopped or a client associated/disassociated
            if (std::strcmp(msg.service_name_, "com.freebsdx.Hotspot") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(HotspotOpcode::StatusChanged)) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.Hotspot",
                        static_cast<uint32_t>(HotspotOpcode::GetStatus), {},
                        [update_hs_ui](const XBusMessage& r) {
                            HotspotStatus st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_hs_ui(st);
                        });
                }
                return;
            }

            // Remote Desktop server started/stopped or client connected/disconnected
            if (std::strcmp(msg.service_name_, "com.freebsdx.RemoteDesktop") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(RemoteDesktopOpcode::StatusChanged)) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.RemoteDesktop",
                        static_cast<uint32_t>(RemoteDesktopOpcode::GetStatus), {},
                        [update_rd_ui](const XBusMessage& r) {
                            RemoteDesktopStatus st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_rd_ui(st);
                        });
                }
                return;
            }

            // Bluetooth adapter toggled or device connection changed — refresh toggle + list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Bluetooth") == 0 &&
                (msg.opcode_ == static_cast<uint32_t>(BluetoothOpcode::DeviceChanged) ||
                 msg.opcode_ == static_cast<uint32_t>(BluetoothOpcode::DeviceFound))) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.Bluetooth",
                        static_cast<uint32_t>(BluetoothOpcode::GetState), {},
                        [update_bt_ui](const XBusMessage& r) {
                            BluetoothState st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_bt_ui(st);
                        });
                }
                return;
            }

            // USB device plugged or unplugged — refresh monitor
            if (std::strcmp(msg.service_name_, "com.freebsdx.Usb") == 0 &&
                (msg.opcode_ == static_cast<uint32_t>(UsbOpcode::DeviceAdded) ||
                 msg.opcode_ == static_cast<uint32_t>(UsbOpcode::DeviceRemoved))) {
                if (rebuild_usb && *rebuild_usb) (*rebuild_usb)();
                return;
            }

            // Power profile changed — sync toggle UI
            if (std::strcmp(msg.service_name_, "com.freebsdx.PowerProfile") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PowerProfileOpcode::ProfileChanged)) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.PowerProfile",
                        static_cast<uint32_t>(PowerProfileOpcode::GetProfile), {},
                        [update_pp_ui](const XBusMessage& r) {
                            PowerProfileState st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_pp_ui(st);
                        });
                }
                return;
            }

            // Camera or microphone access state changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.Privacy") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PrivacyOpcode::DeviceStateChanged)) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.Privacy",
                        static_cast<uint32_t>(PrivacyOpcode::GetStatus), {},
                        [update_privacy_ui](const XBusMessage& r) {
                            PrivacyStatus st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_privacy_ui(st);
                        });
                }
                return;
            }

            // Firewall enabled/disabled or rules changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.Firewall") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(FirewallOpcode::StatusChanged)) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.Firewall",
                        static_cast<uint32_t>(FirewallOpcode::GetStatus), {},
                        [update_fw_ui](const XBusMessage& r) {
                            FirewallStatus st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_fw_ui(st);
                        });
                }
                return;
            }

            // Volume changed (keyboard keys or another client) — show OSD
            if (std::strcmp(msg.service_name_, "com.freebsdx.Audio") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AudioOpcode::VolumeChanged)) {
                AudioVolumeState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                const std::string icon = st.muted_ ? "🔇" : "🔊";
                (*show_osd)(icon, static_cast<int>(st.volume_));
                if (*rebuild_audio_vol) (*rebuild_audio_vol)();
            }

            // Power state changed — update battery indicator
            if (std::strcmp(msg.service_name_, "com.freebsdx.Power") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PowerOpcode::PowerChanged)) {
                PowerState ps{};
                std::memcpy(&ps, msg.payload_, sizeof(ps));
                (*update_battery)(ps);
            }

            // Updates available — refresh badge and post one notification
            if (std::strcmp(msg.service_name_, "com.freebsdx.Update") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(UpdateOpcode::UpdatesAvailable)) {
                UpdateAvailableState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_upd_btn(st.count_);
                if (st.count_ > 0 && g_shell_xbus) {
                    NotificationPayload notif{};
                    notif.timeout_ms_ = 6000;
                    std::strncpy(notif.title_, "Software Updates Available",
                                 sizeof(notif.title_) - 1);
                    char body[128];
                    std::snprintf(body, sizeof(body),
                                  "%u package%s can be updated. Click ⬆ to open App Manager.",
                                  st.count_, st.count_ == 1 ? "" : "s");
                    std::strncpy(notif.message_,  body,          sizeof(notif.message_) - 1);
                    std::strncpy(notif.app_name_, "App Manager", sizeof(notif.app_name_) - 1);
                    g_shell_xbus->Call("com.freebsdx.Notification",
                        static_cast<uint32_t>(NotificationOpcode::Post),
                        {reinterpret_cast<const uint8_t*>(&notif), sizeof(notif)},
                        nullptr);
                }
            }

            // Lock state changed — sync auto-lock toggle + admin unlock row visibility
            if (std::strcmp(msg.service_name_, "com.freebsdx.Lock") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(LockOpcode::LockStateChanged)) {
                LockState ls{};
                std::memcpy(&ls, msg.payload_, sizeof(ls));
                qs_autolock_switch->setValue(ls.timeout_secs_ > 0);
                *unlock_locked = (ls.locked_ != 0);
                unlock_inputs_row->setVisibility(*unlock_locked);
                if (!*unlock_locked) {
                    // Clear credentials and status on successful unlock
                    unlock_status_lbl->setText("");
                    unlock_status_lbl->setVisibility(false);
                }
            }

            // Muted apps list changed — re-sync local set and rebuild UI
            if (std::strcmp(msg.service_name_, "com.freebsdx.Notification") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NotificationOpcode::MutedAppsChanged)) {
                NotifMutedAppList list{};
                std::memcpy(&list, msg.payload_, sizeof(list));
                muted_apps->clear();
                for (uint32_t i = 0; i < list.count_ && i < 8; ++i)
                    muted_apps->insert(std::string(list.names_[i]));
                if (*rebuild_app_list) (*rebuild_app_list)();
            }

            // Display output changed — rebuild display settings panel
            if (std::strcmp(msg.service_name_, "com.freebsdx.Display") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DisplayOpcode::OutputChanged)) {
                DisplayOutputList list{};
                std::memcpy(&list, msg.payload_, sizeof(list));
                rebuild_disp(list);
            }

            // Night Shift state changed — sync QS panel controls
            if (std::strcmp(msg.service_name_, "com.freebsdx.NightShift") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NightShiftOpcode::StateChanged)) {
                NightShiftState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                qs_ns_switch->setValue(st.enabled_ != 0);
                qs_ns_slider->setValue(static_cast<float>(st.intensity_));
                qs_ns_val->setText(std::to_string(st.intensity_) + "%");
            }

            // Weather update — refresh taskbar button text and 3-day forecast
            if (std::strcmp(msg.service_name_, "com.freebsdx.Weather") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(WeatherOpcode::WeatherChanged)) {
                const auto* w = reinterpret_cast<const WeatherCondition*>(msg.payload_);
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%s %d°", w->icon_, w->temp_c_);
                weather_btn->setText(buf);
                if (*forecast_rebuild) (*forecast_rebuild)();
            }

            // Keyboard layout confirmed by compositor
            if (std::strcmp(msg.service_name_, "com.freebsdx.Keymap") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(KeymapOpcode::LayoutChanged)) {
                KeymapRequest kr{};
                std::memcpy(&kr, msg.payload_, sizeof(kr));
                for (int i = 0; i < kNumLayouts; ++i) {
                    if (std::strcmp(kr.layout_, kLayouts[i].code) == 0) {
                        g_keymap_idx = i;
                        kb_active_lbl->setText(kLayouts[i].label);
                        break;
                    }
                }
            }

            // Color filter toggled, type or intensity changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.ColorFilter") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ColorFilterOpcode::StateChanged)) {
                ColorFilterState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_cf_ui(st);
                return;
            }

            // Login session opened or closed — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Sessions") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SessionOpcode::SessionChanged)) {
                if (rebuild_lsess && *rebuild_lsess) (*rebuild_lsess)();
                return;
            }

            // Keyboard backlight brightness changed (Fn key or external tool)
            if (std::strcmp(msg.service_name_, "com.freebsdx.KeyboardBacklight") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(KeyboardBacklightOpcode::StateChanged)) {
                KeyboardBacklightState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_kb_bl_ui(st);
                return;
            }

            // Battery drain sample refreshed — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.BatteryDrain") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(BatteryDrainOpcode::DrainChanged)) {
                if (rebuild_bdrain && *rebuild_bdrain) (*rebuild_bdrain)();
                return;
            }

            // Airplane mode toggled — sync switch + Wi-Fi/BT switches
            if (std::strcmp(msg.service_name_, "com.freebsdx.AirplaneMode") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AirplaneModeOpcode::StateChanged)) {
                AirplaneModeState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_ap_ui(st);
                return;
            }

            // Crontab edited externally or job toggled — refresh list
            if (std::strcmp(msg.service_name_, "com.freebsdx.CronJobs") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CronOpcode::JobChanged)) {
                if (rebuild_cron && *rebuild_cron) (*rebuild_cron)();
                return;
            }

            // Location grant/revoke — update QS switch + requester count
            if (std::strcmp(msg.service_name_, "com.freebsdx.LocationServices") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(LocationOpcode::StateChanged)) {
                LocationState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_loc_ui(st);
                return;
            }

            // Mic mute toggled (by any client) — sync QS switch + status label
            if (std::strcmp(msg.service_name_, "com.freebsdx.MicMute") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(MicMuteOpcode::StateChanged)) {
                MicMuteState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_mic_mute_ui(st);
                return;
            }

            // Startup items changed — rebuild list
            if (std::strcmp(msg.service_name_, "com.freebsdx.StartupItems") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(StartupOpcode::ItemsChanged)) {
                if (rebuild_startup && *rebuild_startup) (*rebuild_startup)();
                return;
            }

            // Rotation lock toggled or sensor auto-rotated — sync QS row
            if (std::strcmp(msg.service_name_, "com.freebsdx.RotationLock") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(RotationLockOpcode::StateChanged)) {
                RotationLockState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_rot_ui(st);
                return;
            }

            // Audio sink plugged/unplugged — rebuild output pill row
            if (std::strcmp(msg.service_name_, "com.freebsdx.AudioOutput") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AudioOutputOpcode::SinkChanged)) {
                if (rebuild_aout && *rebuild_aout) (*rebuild_aout)();
                return;
            }

            // Screen timeout changed from Settings — sync QS preset buttons
            if (std::strcmp(msg.service_name_, "com.freebsdx.ScreenTimeout") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScreenTimeoutOpcode::TimeoutChanged)) {
                ScreenTimeoutState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_stout_ui(st);
                return;
            }

            // Auto-brightness toggled or sensor reading updated — sync QS row
            if (std::strcmp(msg.service_name_, "com.freebsdx.AutoBrightness") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AutoBrightnessOpcode::StateChanged)) {
                AutoBrightnessState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_auto_br_ui(st);
                return;
            }

            // SSH key removed — rebuild AC list
            if (std::strcmp(msg.service_name_, "com.freebsdx.SshKeys") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SshKeyOpcode::KeysChanged)) {
                if (rebuild_sshkey && *rebuild_sshkey) (*rebuild_sshkey)();
                return;
            }

            // Display refresh rate changed (hotplug or Settings) — sync preset buttons
            if (std::strcmp(msg.service_name_, "com.freebsdx.RefreshRate") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(RefreshRateOpcode::RateChanged)) {
                RefreshRateState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_rrate_ui(st);
                return;
            }

            // Env var set/unset — rebuild AC list
            if (std::strcmp(msg.service_name_, "com.freebsdx.EnvVars") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(EnvVarOpcode::EnvChanged)) {
                if (rebuild_envvar && *rebuild_envvar) (*rebuild_envvar)();
                return;
            }

            // Scroll direction changed from Settings — sync QS switch
            if (std::strcmp(msg.service_name_, "com.freebsdx.ScrollDir") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScrollDirOpcode::DirChanged)) {
                ScrollDirState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_scroll_dir_ui(st);
                return;
            }

            // Presentation mode changed (e.g. ended by compositor on screen unlock)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Presentation") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PresentationOpcode::StateChanged)) {
                PresentationState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_pres_ui(st);
                return;
            }

            // Touchpad enabled/disabled externally (e.g. keyboard shortcut)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Touchpad") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TouchpadOpcode::StateChanged)) {
                TouchpadState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_touchpad_ui(st);
                return;
            }

            // Print job state changed — re-poll queue
            if (std::strcmp(msg.service_name_, "com.freebsdx.PrintQueue") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PrintOpcode::QueueChanged)) {
                if (rebuild_print_queue && *rebuild_print_queue) (*rebuild_print_queue)();
                return;
            }

            // Reduce motion toggled externally (e.g. Accessibility settings)
            if (std::strcmp(msg.service_name_, "com.freebsdx.ReduceMotion") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ReduceMotionOpcode::StateChanged)) {
                ReduceMotionState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_reduce_motion_ui(st);
                return;
            }

            // Package database refreshed — re-poll update list
            if (std::strcmp(msg.service_name_, "com.freebsdx.PkgUpdates") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PkgOpcode::UpdatesChanged)) {
                if (rebuild_pkg_upd && *rebuild_pkg_upd) (*rebuild_pkg_upd)();
                return;
            }

            // Cursor size changed externally (e.g. Accessibility settings)
            if (std::strcmp(msg.service_name_, "com.freebsdx.CursorSize") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CursorSizeOpcode::SizeChanged)) {
                CursorSizeState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_cursor_size_ui(st);
                return;
            }

            // ZFS pool state changed — re-poll
            if (std::strcmp(msg.service_name_, "com.freebsdx.ZfsPools") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ZfsOpcode::PoolsChanged)) {
                if (rebuild_zfs && *rebuild_zfs) (*rebuild_zfs)();
                return;
            }

            // Hotspot state changed (client joined/left or toggled externally)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Hotspot") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(HotspotToggleOpcode::StateChanged)) {
                HotspotState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_hotspot_ui(st);
                return;
            }

            // Jail started or stopped — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Jails") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(JailOpcode::JailsChanged)) {
                if (rebuild_jails && *rebuild_jails) (*rebuild_jails)();
                return;
            }

            // Display scale changed (e.g. from Settings or external display hotplug)
            if (std::strcmp(msg.service_name_, "com.freebsdx.DisplayScale") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DisplayScaleOpcode::ScaleChanged)) {
                DisplayScaleState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_disp_scale_ui(st);
                return;
            }

            // App permission granted or revoked
            if (std::strcmp(msg.service_name_, "com.freebsdx.AppPerms") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AppPermOpcode::PermsChanged)) {
                if (rebuild_perms && *rebuild_perms) (*rebuild_perms)();
                return;
            }

            // Dev overlay mode changed externally
            if (std::strcmp(msg.service_name_, "com.freebsdx.DevOverlay") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DevOverlayOpcode::StateChanged)) {
                DevOverlayState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_dev_overlay_ui(st);
                return;
            }

            // Color profile changed externally (e.g. via Settings app)
            if (std::strcmp(msg.service_name_, "com.freebsdx.ColorProfile") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ColorProfileOpcode::ProfileChanged)) {
                ColorProfileState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_color_prof_ui(st);
                return;
            }

            // Input method switched — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.InputMethod") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ImOpcode::MethodChanged)) {
                if (rebuild_im && *rebuild_im) (*rebuild_im)();
                return;
            }

            // Stylus connected/disconnected or settings changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.Stylus") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(StylusOpcode::StateChanged)) {
                StylusState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_stylus_ui(st);
                return;
            }

            // New screenshot/recording taken or capture deleted — re-poll
            if (std::strcmp(msg.service_name_, "com.freebsdx.Captures") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CaptureOpcode::CapturesChanged)) {
                if (rebuild_captures && *rebuild_captures) (*rebuild_captures)();
                return;
            }

            // NTP sync state changed (stratum gained/lost, server switched)
            if (std::strcmp(msg.service_name_, "com.freebsdx.NTP") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NtpOpcode::StateChanged)) {
                NtpState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_ntp_ui(st);
                return;
            }

            // Boot environment list changed — re-poll
            if (std::strcmp(msg.service_name_, "com.freebsdx.BootEnv") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(BeOpcode::BEChanged)) {
                if (rebuild_be && *rebuild_be) (*rebuild_be)();
                return;
            }

            // Update channel changed (may carry new pending count)
            if (std::strcmp(msg.service_name_, "com.freebsdx.UpdateChannel") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(UpdateChannelOpcode::ChannelChanged)) {
                UpdateChannelState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_upd_ch_ui(st);
                return;
            }

            // Text size changed externally
            if (std::strcmp(msg.service_name_, "com.freebsdx.TextSize") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TextSizeOpcode::SizeChanged)) {
                TextSizeState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_text_size_ui(st);
                return;
            }

            // Kernel module loaded or unloaded — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.KernelModules") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(KmodOpcode::ModulesChanged)) {
                if (rebuild_kmod && *rebuild_kmod) (*rebuild_kmod)();
                return;
            }

            // Time zone changed (user pick or system event)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Timezone") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TimezoneOpcode::TZChanged)) {
                TimezoneState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_tz_ui(st);
                return;
            }

            // sshd started, stopped, or session count changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.RemoteLogin") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(RemoteLoginOpcode::StateChanged)) {
                RemoteLoginState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_rl_ui(st);
                return;
            }

            // Accessibility settings changed (here, in Settings, or by a shortcut).
            // (Not com.freebsdx.Accessibility's 0x03 — that's its Announce
            // broadcast, which used to be read as a state change and flipped
            // these buttons with every spoken announcement.)
            if (std::strcmp(msg.service_name_, "com.freebsdx.A11ySettings") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AccessibilitySettingsOpcode::SettingsChanged)) {
                AccessibilityQuickSettings qs{};
                std::memcpy(&qs, msg.payload_, sizeof(qs));
                A11yState st{};
                st.reader_ = qs.screen_reader_; st.contrast_ = qs.high_contrast_; st.magnify_ = qs.magnifier_;
                update_a11y_ui_b(st);
                return;
            }

            // Proxy config changed (network switch, user edit, or MDM push)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Proxy") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ProxyOpcode::ConfigChanged)) {
                ProxyConfig st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_proxy_ui(st);
                return;
            }

            // DNS resolver changed (DHCP update, user pick, or network switch)
            if (std::strcmp(msg.service_name_, "com.freebsdx.DNS") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DnsOpcode::ConfigChanged)) {
                DnsConfig st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_dns_ui(st);
                return;
            }

            // Audio routing changed — app started/stopped or device plugged/unplugged
            if (std::strcmp(msg.service_name_, "com.freebsdx.AudioRouting") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AudioRoutingOpcode::RoutesChanged)) {
                if (rebuild_audio_routes && *rebuild_audio_routes) (*rebuild_audio_routes)();
                return;
            }

            // Wake lock expired or was cancelled externally
            if (std::strcmp(msg.service_name_, "com.freebsdx.WakeLock") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(WakeLockOpcode::StateChanged)) {
                WakeLockState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_wl_ui(st);
                return;
            }

            // Network share mounted or unmounted — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.NetworkShares") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NetworkShareOpcode::SharesChanged)) {
                if (rebuild_shares && *rebuild_shares) (*rebuild_shares)();
                return;
            }

            // VM state transition — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.VirtualMachines") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(VmOpcode::VMChanged)) {
                if (rebuild_vms && *rebuild_vms) (*rebuild_vms)();
                return;
            }

            // Container state transition — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Containers") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ContainerOpcode::ContainersChanged)) {
                if (rebuild_containers && *rebuild_containers) (*rebuild_containers)();
                return;
            }

            // Dock position or auto-hide changed (e.g. from Settings app)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Dock") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DockOpcode::SettingsChanged)) {
                DockSettings st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_dock_ui(st);
                return;
            }

            // Async disk usage scan finished — re-fetch sorted results
            if (std::strcmp(msg.service_name_, "com.freebsdx.DiskUsage") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DiskUsageOpcode::ScanComplete)) {
                if (rebuild_disk_usage && *rebuild_disk_usage) (*rebuild_disk_usage)();
                return;
            }

            // System locale changed (login.conf update or user preference)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Locale") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(LocaleOpcode::LocaleChanged)) {
                LocaleState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_locale_ui(st);
                return;
            }

            // File sync job state change (sync started/finished/paused/errored)
            if (std::strcmp(msg.service_name_, "com.freebsdx.FileSync") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(FileSyncOpcode::JobsChanged)) {
                if (rebuild_file_sync && *rebuild_file_sync) (*rebuild_file_sync)();
                return;
            }

            // Screen saver config changed (another client called SetConfig)
            if (std::strcmp(msg.service_name_, "com.freebsdx.ScreenSaver") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScreenSaverOpcode::StateChanged)) {
                ScreenSaverState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_ss_ui(st.timeout_secs_, st.mode_);
                return;
            }

            // Keychain item added, updated, or removed
            if (std::strcmp(msg.service_name_, "com.freebsdx.Keychain") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(KeychainOpcode::Changed)) {
                if (rebuild_keychain && *rebuild_keychain) (*rebuild_keychain)();
                return;
            }

            // XWayland started or stopped
            if (std::strcmp(msg.service_name_, "com.freebsdx.XWayland") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(XWaylandOpcode::StatusChanged)) {
                XWaylandStatus st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_xwl_ui(st);
                return;
            }

            // Camera device hotplug (new device connected or removed)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Camera") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CameraOpcode::DeviceChanged)) {
                if (rebuild_cam_dev && *rebuild_cam_dev) (*rebuild_cam_dev)();
                return;
            }

            // Package signing policy or trust store changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.PackageSigning") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PackageSigningOpcode::SigningChanged)) {
                g_shell_xbus->Call("com.freebsdx.PackageSigning",
                    static_cast<uint32_t>(PackageSigningOpcode::GetPolicy), {},
                    [update_pkgsign_ui](const XBusMessage& r) {
                        PkgSigningPolicy pol{};
                        std::memcpy(&pol, r.payload_, sizeof(pol));
                        update_pkgsign_ui(pol);
                    });
                return;
            }

            // App catalog refreshed — rebuild store list
            if (std::strcmp(msg.service_name_, "com.freebsdx.AppManager") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AppManagerCatalogOpcode::CatalogUpdated)) {
                if (rebuild_store && *rebuild_store)
                    (*rebuild_store)(store_search_input->getText());
                return;
            }

            // Terminal session list changed (create/destroy/rename)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Terminal") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TerminalOpcode::SessionChanged)) {
                if (rebuild_term_sess && *rebuild_term_sess) (*rebuild_term_sess)();
                return;
            }

            // Font list changed after install or remove
            if (std::strcmp(msg.service_name_, "com.freebsdx.FontManager") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(FontManagerOpcode::FontsChanged)) {
                if (rebuild_fonts && *rebuild_fonts) (*rebuild_fonts)();
                return;
            }

            // App install / uninstall progress
            if (std::strcmp(msg.service_name_, "com.freebsdx.AppManager") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AppManagerOpcode::InstallProgress)) {
                AppInstallProgress prog{};
                std::memcpy(&prog, msg.payload_, sizeof(prog));
                if (prog.status_ == 0) {
                    // In-progress — show progress banner
                    iapps_prog_lbl->setText(
                        std::format("{} {}%", std::string(prog.message_), prog.percent_));
                    iapps_prog_lbl->setVisibility(true);
                } else {
                    // Completed or failed — rebuild the list
                    iapps_prog_lbl->setVisibility(false);
                    if (rebuild_iapps && *rebuild_iapps) (*rebuild_iapps)();
                }
                return;
            }

            // Seat topology changed (new seat attached or removed)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Seat") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SeatOpcode::SeatChanged)) {
                if (rebuild_seats && *rebuild_seats) (*rebuild_seats)();
                return;
            }

            // Archive operation completed
            if (std::strcmp(msg.service_name_, "com.freebsdx.Archive") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ArchiveOpcode::OperationDone)) {
                ArchiveStatusResponse resp{};
                std::memcpy(&resp, msg.payload_, sizeof(resp));
                if (resp.success_) {
                    arch_status_lbl->setText(
                        std::format("\xe2\x9c\x93 {}", std::string(resp.message_)));
                    arch_status_lbl->setColor({0.30f, 0.75f, 0.45f, 1.0f});
                } else {
                    arch_status_lbl->setText(
                        std::format("\xe2\x9c\x97 {}", std::string(resp.message_)));
                    arch_status_lbl->setColor({0.80f, 0.30f, 0.30f, 1.0f});
                }
                return;
            }

            // Calendar event reminder — refresh upcoming events list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Calendar") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CalendarOpcode::EventReminder)) {
                if (rebuild_uev && *rebuild_uev) (*rebuild_uev)();
                return;
            }

            // GPU stats live update
            if (std::strcmp(msg.service_name_, "com.freebsdx.GpuMonitor") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(GpuMonitorOpcode::StatsChanged)) {
                GpuStatsDetail st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_gpu_ui(st);
                return;
            }

            // New screenshot captured
            if (std::strcmp(msg.service_name_, "com.freebsdx.ScreenCapture") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScreenCaptureOpcode::CaptureAdded)) {
                if (rebuild_scap && *rebuild_scap) (*rebuild_scap)();
                return;
            }

            // Display layout changed (monitor hotplug or primary switched)
            if (std::strcmp(msg.service_name_, "com.freebsdx.DisplayArrange") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DisplayArrangeOpcode::LayoutChanged)) {
                if (rebuild_disp_arr && *rebuild_disp_arr) (*rebuild_disp_arr)();
                return;
            }

            // Clipboard history: new entry added
            if (std::strcmp(msg.service_name_, "com.freebsdx.ClipboardHistory") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ClipHistoryOpcode::EntryAdded)) {
                if (rebuild_clip && *rebuild_clip) (*rebuild_clip)();
                return;
            }

            // Gesture config changed remotely
            if (std::strcmp(msg.service_name_, "com.freebsdx.Gesture") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(GestureOpcode::ConfigChanged)) {
                GestureConfig cfg{};
                std::memcpy(&cfg, msg.payload_, sizeof(cfg));
                update_gest_ui(cfg);
                return;
            }

            // Input device hotplug
            if (std::strcmp(msg.service_name_, "com.freebsdx.InputDevice") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(InputDeviceOpcode::DeviceChanged)) {
                if (rebuild_indev && *rebuild_indev) (*rebuild_indev)();
                return;
            }

            // Accessibility settings changed remotely
            if (std::strcmp(msg.service_name_, "com.freebsdx.A11yPanel") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AccessibilityPanelOpcode::SettingsChanged)) {
                AccessibilitySettings cfg{};
                std::memcpy(&cfg, msg.payload_, sizeof(cfg));
                update_a11y_ui_c(cfg);
                return;
            }

            // Remote desktop session list changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.RemoteDesktop") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(RemoteDesktopSessionOpcode::SessionChanged)) {
                if (rebuild_rdp && *rebuild_rdp) (*rebuild_rdp)();
                rdp_status_lbl->setText("");
                return;
            }

            // New system log entry — prepend to list
            if (std::strcmp(msg.service_name_, "com.freebsdx.SysLog") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SysLogOpcode::LogAdded)) {
                // Rebuild to show newest entries; level filter still applies
                if (rebuild_slog && *rebuild_slog) (*rebuild_slog)();
                return;
            }
            // DNS config updated by another process
            if (std::strcmp(msg.service_name_, "com.freebsdx.DnsConfig") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DnsConfigOpcode::ConfigChanged)) {
                DnsResolverConfig cfg{};
                std::memcpy(&cfg, msg.payload_, sizeof(cfg));
                update_dns_cfg_ui(cfg);
                return;
            }
            // Network share mounted / unmounted elsewhere
            if (std::strcmp(msg.service_name_, "com.freebsdx.NetworkShare") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NetworkShareMountOpcode::ShareChanged)) {
                if (rebuild_ns && *rebuild_ns) (*rebuild_ns)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Thermal") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ThermalOpcode::ZonesChanged)) {
                if (rebuild_tz && *rebuild_tz) (*rebuild_tz)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.UsbDevice") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(UsbDeviceOpcode::DeviceChanged)) {
                if (rebuild_usbdev && *rebuild_usbdev) (*rebuild_usbdev)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.NotifSchedule") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NotifScheduleOpcode::ScheduleChanged)) {
                NotifSchedule s{};
                std::memcpy(&s, msg.payload_, sizeof(s));
                update_nsch_ui(s);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.AppSandbox") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AppSandboxOpcode::PermsChanged)) {
                if (rebuild_sandbox && *rebuild_sandbox) (*rebuild_sandbox)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.ScreenLock") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScreenLockOpcode::PolicyChanged)) {
                ScreenLockPolicy p{};
                std::memcpy(&p, msg.payload_, sizeof(p));
                update_slk_ui(p);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.BtAudio") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(BtAudioOpcode::DeviceChanged)) {
                if (rebuild_btaudio && *rebuild_btaudio) (*rebuild_btaudio)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Location") == 0 &&
                (msg.opcode_ == static_cast<uint32_t>(LocationOpcode::StateChanged) ||
                 msg.opcode_ == static_cast<uint32_t>(LocationOpcode::PolicyChanged))) {
                // Re-fetch state and app list
                if (!g_shell_xbus) return;
                g_shell_xbus->Call("com.freebsdx.Location",
                    static_cast<uint32_t>(LocationOpcode::GetState), {},
                    [update_loc_policy](const XBusMessage& r) {
                        LocationState s{};
                        std::memcpy(&s, r.payload_, sizeof(s));
                        update_loc_policy(s);
                    });
                if (rebuild_loc && *rebuild_loc) (*rebuild_loc)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Totp") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TotpOpcode::AccountAdded)) {
                if (rebuild_totp && *rebuild_totp) (*rebuild_totp)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.AudioEq") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AudioEqOpcode::EqChanged)) {
                AudioEqConfig cfg{};
                std::memcpy(&cfg, msg.payload_, sizeof(cfg));
                update_eq_ui(cfg);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Backup") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(BackupOpcode::JobStateChanged)) {
                if (rebuild_bkp && *rebuild_bkp) (*rebuild_bkp)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Pointer") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PointerOpcode::ConfigChanged)) {
                PointerConfig cfg{};
                std::memcpy(&cfg, msg.payload_, sizeof(cfg));
                update_ptr_ui(cfg);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Reminder") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ReminderOpcode::ReminderChanged)) {
                if (rebuild_rem && *rebuild_rem) (*rebuild_rem)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.SpatialAudio") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SpatialAudioOpcode::ConfigChanged)) {
                SpatialAudioConfig cfg{};
                std::memcpy(&cfg, msg.payload_, sizeof(cfg));
                update_sa_ui(cfg);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Transfer") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TransferOpcode::TransferChanged)) {
                if (rebuild_xfr && *rebuild_xfr) (*rebuild_xfr)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.SystemSounds") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SystemSoundsOpcode::StateChanged)) {
                SystemSoundsState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_sndfx_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.GameController") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(GameControllerOpcode::ControllerChanged)) {
                if (rebuild_gc && *rebuild_gc) (*rebuild_gc)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.LiveCaptions") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(LiveCaptionsOpcode::StateChanged)) {
                LiveCaptionsState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_captions_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.SavedWifi") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SavedWifiOpcode::SavedListChanged)) {
                if (rebuild_swifi && *rebuild_swifi) (*rebuild_swifi)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.DataSaver") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DataSaverOpcode::StateChanged)) {
                DataSaverState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_dsaver_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.SecurityKey") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SecurityKeyOpcode::KeysChanged)) {
                if (rebuild_seckey && *rebuild_seckey) (*rebuild_seckey)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.FastCharging") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(FastChargingOpcode::StateChanged)) {
                FastChargingState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_fastchg_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.TextReplacement") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TextReplacementOpcode::ShortcutsChanged)) {
                if (rebuild_txtrep && *rebuild_txtrep) (*rebuild_txtrep)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.DoNotDisturb") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DoNotDisturbOpcode::StateChanged)) {
                DoNotDisturbState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_silence_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.AppTimeLimit") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AppTimeLimitOpcode::LimitsChanged)) {
                if (rebuild_applimit && *rebuild_applimit) (*rebuild_applimit)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.AppSandbox") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AppSandboxOpcode::ConsentRequested)) {
                AppSandboxConsentRequest r{};
                std::memcpy(&r, msg.payload_, sizeof(r));
                if (g_show_consent) g_show_consent(r);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.AppTimeLimit") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AppTimeLimitOpcode::LimitReached)) {
                AppTimeLimitReached r{};
                std::memcpy(&r, msg.payload_, sizeof(r));
                if (g_show_times_up) g_show_times_up(r);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.BatteryHealth") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(BatteryHealthOpcode::StateChanged)) {
                BatteryHealthState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_bhealth_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.NotifSound") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NotifSoundOpcode::SoundsChanged)) {
                if (rebuild_notifsnd && *rebuild_notifsnd) (*rebuild_notifsnd)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.TrueTone") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TrueToneOpcode::StateChanged)) {
                TrueToneState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_ttone_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.LoginItems") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(LoginItemOpcode::ItemsChanged)) {
                if (rebuild_litem && *rebuild_litem) (*rebuild_litem)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.OptStorage") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(OptStorageOpcode::StateChanged)) {
                OptStorageState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_ostore_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.TrustedDevices") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TrustedDeviceOpcode::DevicesChanged)) {
                if (rebuild_tdev && *rebuild_tdev) (*rebuild_tdev)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.MonoAudio") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(MonoAudioOpcode::StateChanged)) {
                MonoAudioState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_monoaud_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.KeyboardShortcuts") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(KeyboardShortcutOpcode::ShortcutsChanged)) {
                if (rebuild_kbsc && *rebuild_kbsc) (*rebuild_kbsc)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.StageManager") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(StageManagerOpcode::StateChanged)) {
                StageManagerState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_stagemgr_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.DefaultApps") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DefaultAppsOpcode::DefaultsChanged)) {
                if (rebuild_dapp && *rebuild_dapp) (*rebuild_dapp)();
                return;
            }
}
