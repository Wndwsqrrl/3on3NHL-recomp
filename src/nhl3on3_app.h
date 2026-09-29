// nhl3on3 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>

class Nhl3on3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Nhl3on3App>(new Nhl3on3App(ctx, "nhl3on3",
        PPCImageConfig));
  }

  // Use the Xenos GPU plugin unless --gpu_plugin says otherwise.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
  }

  // Find the game files when --game_data_root is not given: a "game" folder next to the
  // executable (release layout), else "game/extracted" in any parent folder (repo layout).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    ApplyDefaultSettings();
    if (!paths.game_data_root.empty()) {
      return;
    }
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    if (std::filesystem::exists(exe_dir / "game" / "default.xex")) {
      paths.game_data_root = exe_dir / "game";
      return;
    }
    for (auto dir = exe_dir; dir.has_parent_path() && dir != dir.parent_path();
         dir = dir.parent_path()) {
      if (std::filesystem::exists(dir / "game" / "extracted" / "default.xex")) {
        paths.game_data_root = dir / "game" / "extracted";
        return;
      }
    }
  }

  // Settings this game needs, applied unless set on the command line or in a config file.
  static void ApplyDefaultSettings() {
    struct Default {
      const char* name;
      const char* value;
    };
    static constexpr Default kDefaults[] = {
        {"license_mask", "1"},                    // full XBLA license, not the trial
        {"async_shader_compilation", "false"},    // player select hangs while shaders compile
        {"gpu_allow_invalid_fetch_constants", "true"},  // otherwise warns every frame
        {"readback_resolve", "full"},  // text, scoreboard and helmets come from GPU resolves
    };
    for (const auto& d : kDefaults) {
      if (!rex::cvar::HasNonDefaultValue(d.name)) {
        rex::cvar::SetFlagByName(d.name, d.value);
      }
    }
  }

  // Other hooks available for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
};
