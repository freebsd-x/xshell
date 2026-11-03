// ── xshell panel builders: everyday desktop: notifications, focus, clipboard, notes, calendar, timers, locale ──
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
    auto envvar_key_input = std::make_shared<TextInputWidget>("");   // hints, not initial text
    envvar_key_input->setPlaceholder("KEY");
    auto envvar_val_input = std::make_shared<TextInputWidget>("");
    envvar_val_input->setPlaceholder("value");
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

    // com.freebsdx.A11ySettings (persisted). These used to go to
    // com.freebsdx.Accessibility, whose opcodes mean something else there
    // (SetFeature = Unsubscribe), so the buttons never did anything.
    auto a11y_send = [](uint32_t feature, uint32_t enabled) {
        if (!g_shell_xbus) return;
        using O = AccessibilitySettingsOpcode;
        const O op = feature == 0 ? O::SetScreenReader : feature == 1 ? O::SetHighContrast : O::SetMagnifier;
        AccessibilityToggleRequest req{};
        req.enabled_ = enabled;
        g_shell_xbus->Call("com.freebsdx.A11ySettings", static_cast<uint32_t>(op),
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
