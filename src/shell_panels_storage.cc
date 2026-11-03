// ── xshell panel builders: storage: volumes, ZFS, disks, shares, backups, transfers ──
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
                    const uint32_t count = std::min<uint32_t>(vp.count_, std::size(vp.entries_));
                    for (uint32_t i = 0; i < count; ++i) {
                        const auto& e = vp.entries_[i];
                        // Usage percentage and bar (10 blocks)
                        double pct = (e.total_kb_ > 0)
                            ? std::clamp(100.0 * static_cast<double>(e.used_kb_)
                                             / static_cast<double>(e.total_kb_), 0.0, 100.0)
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
