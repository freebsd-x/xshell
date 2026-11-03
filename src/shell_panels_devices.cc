// ── xshell panel builders: devices and power: USB, printing, Bluetooth, audio, input, battery, power ──
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
