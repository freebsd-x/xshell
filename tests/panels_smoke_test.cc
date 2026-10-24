// Smoke tests for every xshell panel builder (shell_panels.cc).
//
// The main() decomposition carved 149 inline panel blocks into free
// buildXPanel(ShellContext&) functions. Each builds a widget tree into the
// shared ShellContext; all XBus traffic is deferred into callbacks that
// early-out when g_shell_xbus is null. These tests exercise construction of
// every builder with g_shell_xbus == nullptr, catching null derefs, bad
// fixed-array indexing, and exceptions at build time — the failure modes a
// refactor of this size is most likely to introduce. Behavioural (XBus
// round-trip) coverage is out of scope; this is a construction safety net.

#include "shell_panels.h"
#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace {
// A fresh, disconnected world for each builder: null XBus + empty context.
struct SmokeEnv {
    SmokeEnv() { g_shell_xbus = nullptr; }
    ShellContext ctx;
};
std::shared_ptr<SwitchWidget> sw() { return std::make_shared<SwitchWidget>(false); }
}  // namespace

TEST(PanelsSmokeTest, CronJobsPanel) {
    SmokeEnv e;
    buildCronJobsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, MountedVolumesPanel) {
    SmokeEnv e;
    buildMountedVolumesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, StartupItemsPanel) {
    SmokeEnv e;
    buildStartupItemsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, NetConnsPanel) {
    SmokeEnv e;
    buildNetConnsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, SshKeysPanel) {
    SmokeEnv e;
    buildSshKeysPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, EnvVarsPanel) {
    SmokeEnv e;
    buildEnvVarsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, SysLogPanel) {
    SmokeEnv e;
    buildSysLogPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, UsbDevicesPanel) {
    SmokeEnv e;
    buildUsbDevicesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, LoginSessionsPanel) {
    SmokeEnv e;
    buildLoginSessionsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, BatteryDrainPanel) {
    SmokeEnv e;
    buildBatteryDrainPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, FileSyncPanel) {
    SmokeEnv e;
    buildFileSyncPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, AudioRoutingPanel) {
    SmokeEnv e;
    buildAudioRoutingPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, NetworkSharesPanel) {
    SmokeEnv e;
    buildNetworkSharesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, PrinterQueuePanel) {
    SmokeEnv e;
    buildPrinterQueuePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, PackageUpdatesPanel) {
    SmokeEnv e;
    buildPackageUpdatesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ZfsPoolsPanel) {
    SmokeEnv e;
    buildZfsPoolsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ActiveJailsPanel) {
    SmokeEnv e;
    buildActiveJailsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, BootEnvPanel) {
    SmokeEnv e;
    buildBootEnvPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ContainersPanel) {
    SmokeEnv e;
    buildContainersPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, VirtualMachinesPanel) {
    SmokeEnv e;
    buildVirtualMachinesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, KernelModulesPanel) {
    SmokeEnv e;
    buildKernelModulesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, CapturesPanel) {
    SmokeEnv e;
    buildCapturesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, KeychainPanel) {
    SmokeEnv e;
    buildKeychainPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, SystemServicesPanel) {
    SmokeEnv e;
    buildSystemServicesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, StoragePanel) {
    SmokeEnv e;
    buildStoragePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, DiskIoPanel) {
    SmokeEnv e;
    buildDiskIoPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, SmartHealthPanel) {
    SmokeEnv e;
    buildSmartHealthPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, DiskUsagePanel) {
    SmokeEnv e;
    buildDiskUsagePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ArchivePanel) {
    SmokeEnv e;
    buildArchivePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, PrintSpoolerPanel) {
    SmokeEnv e;
    buildPrintSpoolerPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, UsbMonitorPanel) {
    SmokeEnv e;
    buildUsbMonitorPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, BookmarksPanel) {
    SmokeEnv e;
    buildBookmarksPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ActiveTransfersPanel) {
    SmokeEnv e;
    buildActiveTransfersPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, GameControllersPanel) {
    SmokeEnv e;
    buildGameControllersPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, SavedWifiPanel) {
    SmokeEnv e;
    buildSavedWifiPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, SecurityKeysPanel) {
    SmokeEnv e;
    buildSecurityKeysPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, TextReplacementPanel) {
    SmokeEnv e;
    buildTextReplacementPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, AppTimeLimitsPanel) {
    SmokeEnv e;
    buildAppTimeLimitsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, NotifSoundsPanel) {
    SmokeEnv e;
    buildNotifSoundsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, LoginItemsPanel) {
    SmokeEnv e;
    buildLoginItemsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, TrustedDevicesPanel) {
    SmokeEnv e;
    buildTrustedDevicesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, KeyboardShortcutsPanel) {
    SmokeEnv e;
    buildKeyboardShortcutsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, DefaultAppsPanel) {
    SmokeEnv e;
    buildDefaultAppsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, RemindersPanel) {
    SmokeEnv e;
    buildRemindersPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ScheduledBackupsPanel) {
    SmokeEnv e;
    buildScheduledBackupsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, TotpPanel) {
    SmokeEnv e;
    buildTotpPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, BtAudioPanel) {
    SmokeEnv e;
    buildBtAudioPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, AppSandboxPanel) {
    SmokeEnv e;
    buildAppSandboxPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, UsbDeviceDetailPanel) {
    SmokeEnv e;
    buildUsbDeviceDetailPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, NetworkSharesMountPanel) {
    SmokeEnv e;
    buildNetworkSharesMountPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, SystemLogPanel) {
    SmokeEnv e;
    buildSystemLogPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, RemoteDesktopPanel) {
    SmokeEnv e;
    buildRemoteDesktopPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, InputDevicesPanel) {
    SmokeEnv e;
    buildInputDevicesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ClipboardHistoryPanel) {
    SmokeEnv e;
    buildClipboardHistoryPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ScreenshotGalleryPanel) {
    SmokeEnv e;
    buildScreenshotGalleryPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, UpcomingEventsPanel) {
    SmokeEnv e;
    buildUpcomingEventsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, AcStubSections) {
    SmokeEnv e;
    buildAcStubSections(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, LocationServicesTogglePanel) {
    SmokeEnv e;
    buildLocationServicesTogglePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, AudioOutputPanel) {
    SmokeEnv e;
    buildAudioOutputPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, NtpTogglePanel) {
    SmokeEnv e;
    buildNtpTogglePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, MousePointerPanel) {
    SmokeEnv e;
    buildMousePointerPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, BluetoothDevicePanel) {
    SmokeEnv e;
    buildBluetoothDevicePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, VpnPanel) {
    SmokeEnv e;
    buildVpnPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, HotspotConfigPanel) {
    SmokeEnv e;
    buildHotspotConfigPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ScreenCastPanel) {
    SmokeEnv e;
    buildScreenCastPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, RemoteDesktopPassForm) {
    SmokeEnv e;
    buildRemoteDesktopPassForm(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, MediaPlaybackPanel) {
    SmokeEnv e;
    buildMediaPlaybackPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ColorFilterPanel) {
    SmokeEnv e;
    buildColorFilterPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, AudioEqPanel) {
    SmokeEnv e;
    buildAudioEqPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, PowerMenuPanel) {
    SmokeEnv e;
    buildPowerMenuPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, GameModePanel) {
    SmokeEnv e;
    buildGameModePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, PrivacyPanel) {
    SmokeEnv e;
    buildPrivacyPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, WakeLockPanel) {
    SmokeEnv e;
    buildWakeLockPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, RemoteLoginPanel) {
    SmokeEnv e;
    buildRemoteLoginPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, LocalePanel) {
    SmokeEnv e;
    buildLocalePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, A11yQuickPanel) {
    SmokeEnv e;
    buildA11yQuickPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ProxyPanel) {
    SmokeEnv e;
    buildProxyPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, DnsConfigPanel) {
    SmokeEnv e;
    buildDnsConfigPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, UpdateChannelPanel) {
    SmokeEnv e;
    buildUpdateChannelPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, TimeZonePanel) {
    SmokeEnv e;
    buildTimeZonePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, PackageSigningPanel) {
    SmokeEnv e;
    buildPackageSigningPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, BatteryHealthPanel) {
    SmokeEnv e;
    buildBatteryHealthPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ScreenLockPanel) {
    SmokeEnv e;
    buildScreenLockPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, NotifSchedulePanel) {
    SmokeEnv e;
    buildNotifSchedulePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, AutoLockPanel) {
    SmokeEnv e;
    buildAutoLockPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, KeyboardBacklightPanel) {
    SmokeEnv e;
    buildKeyboardBacklightPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, PowerProfilePanel) {
    SmokeEnv e;
    buildPowerProfilePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, NightShiftPanel) {
    SmokeEnv e;
    buildNightShiftPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ScreenSaverPanel) {
    SmokeEnv e;
    buildScreenSaverPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, DockLayoutPanel) {
    SmokeEnv e;
    buildDockLayoutPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, XWaylandPanel) {
    SmokeEnv e;
    buildXWaylandPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, DisplaySettingsPanel) {
    SmokeEnv e;
    buildDisplaySettingsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, FocusModePanel) {
    SmokeEnv e;
    buildFocusModePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, BatteryIndicatorPanel) {
    SmokeEnv e;
    buildBatteryIndicatorPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, NotifBadgePanel) {
    SmokeEnv e;
    buildNotifBadgePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, OsdOverlayPanel) {
    SmokeEnv e;
    buildOsdOverlayPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, SystemTrayPanel) {
    SmokeEnv e;
    buildSystemTrayPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ContactSearchPanel) {
    SmokeEnv e;
    buildContactSearchPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, NotesMiniPanel) {
    SmokeEnv e;
    buildNotesMiniPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ClipboardMiniPanel) {
    SmokeEnv e;
    buildClipboardMiniPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, SparklinesPanel) {
    SmokeEnv e;
    buildSparklinesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, LicensePanel) {
    SmokeEnv e;
    buildLicensePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, MemPressurePanel) {
    SmokeEnv e;
    buildMemPressurePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, FrameTimingPanel) {
    SmokeEnv e;
    buildFrameTimingPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, SysInfoPanel) {
    SmokeEnv e;
    buildSysInfoPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, GpuStatsPanel) {
    SmokeEnv e;
    buildGpuStatsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, GpuMonitorPanel) {
    SmokeEnv e;
    buildGpuMonitorPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, NightLightPanel) {
    SmokeEnv e;
    buildNightLightPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, GestureConfigPanel) {
    SmokeEnv e;
    buildGestureConfigPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, StylusPanel) {
    SmokeEnv e;
    buildStylusPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ColorProfilePanel) {
    SmokeEnv e;
    buildColorProfilePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, CursorSizePanel) {
    SmokeEnv e;
    buildCursorSizePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, DisplayScalePanel) {
    SmokeEnv e;
    buildDisplayScalePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, RefreshRatePanel) {
    SmokeEnv e;
    buildRefreshRatePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, TextSizePanel) {
    SmokeEnv e;
    buildTextSizePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, AutoBrightnessPanel) {
    SmokeEnv e;
    buildAutoBrightnessPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ScreenTimeoutPanel) {
    SmokeEnv e;
    buildScreenTimeoutPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, RotationLockPanel) {
    SmokeEnv e;
    buildRotationLockPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, MicMutePanel) {
    SmokeEnv e;
    buildMicMutePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, AdminUnlockPanel) {
    SmokeEnv e;
    buildAdminUnlockPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ThermalZonesPanel) {
    SmokeEnv e;
    buildThermalZonesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, LocationServicesPanel) {
    SmokeEnv e;
    buildLocationServicesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, CameraDevicesPanel) {
    SmokeEnv e;
    buildCameraDevicesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, MultiSeatPanel) {
    SmokeEnv e;
    buildMultiSeatPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, CrashReporterPanel) {
    SmokeEnv e;
    buildCrashReporterPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, SecurityEventsPanel) {
    SmokeEnv e;
    buildSecurityEventsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, CalendarQuickAddPanel) {
    SmokeEnv e;
    buildCalendarQuickAddPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, BatterySaverPanel) {
    SmokeEnv e;
    buildBatterySaverPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, PomodoroPanel) {
    SmokeEnv e;
    buildPomodoroPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, UpdatesButtonPanel) {
    SmokeEnv e;
    std::vector<AppMetadata> apps;
    buildUpdatesButtonPanel(e.ctx, apps);
    SUCCEED();
}

TEST(PanelsSmokeTest, IdleLockPanel) {
    SmokeEnv e;
    buildIdleLockPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, StartupAppsPanel) {
    SmokeEnv e;
    buildStartupAppsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ScratchPadPanel) {
    SmokeEnv e;
    buildScratchPadPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, WeatherForecastPanel) {
    SmokeEnv e;
    buildWeatherForecastPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, QuickTimerPanel) {
    SmokeEnv e;
    buildQuickTimerPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, PomodoroLogPanel) {
    SmokeEnv e;
    buildPomodoroLogPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, AppHeatmapPanel) {
    SmokeEnv e;
    buildAppHeatmapPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ScreenshotPanel) {
    SmokeEnv e;
    buildScreenshotPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, RecentFilesPanel) {
    SmokeEnv e;
    buildRecentFilesPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, ClipboardSlotsPanel) {
    SmokeEnv e;
    buildClipboardSlotsPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, NetworkWifiPanel) {
    SmokeEnv e;
    buildNetworkWifiPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, DndSchedulePanel) {
    SmokeEnv e;
    buildDndSchedulePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, NightShiftSchedulePanel) {
    SmokeEnv e;
    buildNightShiftSchedulePanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, WorkspaceWallpaperPanel) {
    SmokeEnv e;
    buildWorkspaceWallpaperPanel(e.ctx);
    SUCCEED();
}

TEST(PanelsSmokeTest, AirplaneModePanel) {
    SmokeEnv e;
    buildAirplaneModePanel(e.ctx, sw(), sw());
    SUCCEED();
}

TEST(PanelsSmokeTest, FocusToggleButton) {
    SmokeEnv e;
    buildFocusToggleButton(e.ctx, sw());
    SUCCEED();
}

TEST(PanelsSmokeTest, ColorThemePanel) {
    SmokeEnv e;
    buildColorThemePanel(e.ctx, sw());
    SUCCEED();
}

TEST(PanelsSmokeTest, SessionRestorePanel) {
    SmokeEnv e;
    std::vector<AppMetadata> apps;
    buildSessionRestorePanel(e.ctx, apps);
    SUCCEED();
}

TEST(PanelsSmokeTest, KeyboardLayoutPanel) {
    SmokeEnv e;
    buildKeyboardLayoutPanel(e.ctx);
    SUCCEED();
}
