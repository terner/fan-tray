#include <libayatana-appindicator/app-indicator.h>
#include <gtk/gtk.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct FanPaths {
    std::string rpm_input;
    std::string pwm_path;
    std::string pwm_enable_path;
};

std::optional<std::string> read_trimmed(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return std::nullopt;
    std::string line;
    std::getline(ifs, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

bool write_value(const std::string& path, const std::string& value) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) return false;
    ofs << value << '\n';
    return ofs.good();
}

FanPaths discover_chassis3() {
    FanPaths paths;
    const std::string hwmon_root = "/sys/class/hwmon";
    if (!fs::exists(hwmon_root)) return paths;

    // 1. Find Chassis Fan 3 RPM input (typically in asus_wmi_sensors)
    for (const auto& entry : fs::directory_iterator(hwmon_root)) {
        const std::string dir = entry.path().string();
        for (int i = 1; i <= 10; ++i) {
            auto label = read_trimmed(dir + "/fan" + std::to_string(i) + "_label");
            if (label && *label == "Chassis Fan 3") {
                std::string input = dir + "/fan" + std::to_string(i) + "_input";
                if (fs::exists(input)) {
                    paths.rpm_input = input;
                }
            }
        }
        // 2. Check if Super I/O (nct6775 or it8665) exposes pwm4 (Chassis Fan 3)
        auto name = read_trimmed(dir + "/name");
        if (name && (name->rfind("nct67", 0) == 0 || name->rfind("it86", 0) == 0 || name->rfind("it87", 0) == 0)) {
            std::string pwm = dir + "/pwm4";
            std::string pwm_en = dir + "/pwm4_enable";
            if (fs::exists(pwm)) {
                paths.pwm_path = pwm;
                if (fs::exists(pwm_en)) paths.pwm_enable_path = pwm_en;
            }
        }
    }
    return paths;
}

std::optional<int> read_v100_temp() {
    std::array<char, 64> buf{};
    std::unique_ptr<FILE, decltype(&pclose)> pipe(
        popen("nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits 2>/dev/null", "r"),
        pclose);
    if (!pipe) return std::nullopt;
    if (fgets(buf.data(), static_cast<int>(buf.size()), pipe.get()) != nullptr) {
        try {
            return std::stoi(buf.data());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

struct AppState {
    AppIndicator* indicator = nullptr;
    GtkWidget* status_item = nullptr;
    GtkWidget* turbo_item = nullptr;
    GtkWidget* auto_turbo_item = nullptr;
    bool turbo_on = false;
    bool auto_turbo_enabled = true;
    bool updating_ui = false;
    FanPaths paths;
};

void apply_fan_pwm(AppState* state, bool turbo) {
    state->turbo_on = turbo;
    if (state->paths.pwm_path.empty()) return;
    if (!state->paths.pwm_enable_path.empty()) {
        write_value(state->paths.pwm_enable_path, "1");  // Manual PWM
    }
    write_value(state->paths.pwm_path, turbo ? "255" : "135");
}

void toggle_turbo(GtkCheckMenuItem* item, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (state->updating_ui) return;
    apply_fan_pwm(state, gtk_check_menu_item_get_active(item));
}

void toggle_auto_turbo(GtkCheckMenuItem* item, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    state->auto_turbo_enabled = gtk_check_menu_item_get_active(item);
}

gboolean update_tray(gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);

    std::string rpm_str = "--";
    if (!state->paths.rpm_input.empty()) {
        if (auto val = read_trimmed(state->paths.rpm_input)) {
            rpm_str = *val;
        }
    }

    auto v100_temp = read_v100_temp();
    if (v100_temp && state->auto_turbo_enabled && !state->paths.pwm_path.empty()) {
        if (*v100_temp >= 60 && !state->turbo_on) {
            apply_fan_pwm(state, true);
            if (state->turbo_item) {
                state->updating_ui = true;
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(state->turbo_item), TRUE);
                state->updating_ui = false;
            }
        } else if (*v100_temp <= 55 && state->turbo_on) {
            apply_fan_pwm(state, false);
            if (state->turbo_item) {
                state->updating_ui = true;
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(state->turbo_item), FALSE);
                state->updating_ui = false;
            }
        }
    }

    std::string label = "CHA3: " + rpm_str + " RPM";
    if (v100_temp) {
        label += " | V100: " + std::to_string(*v100_temp) + "°C";
    }

    app_indicator_set_label(state->indicator, label.c_str(), "CHA3: 9999 RPM | V100: 99°C");

    std::string status_text = "Chassis Fan 3: " + rpm_str + " RPM";
    if (v100_temp) {
        status_text += "  |  V100: " + std::to_string(*v100_temp) + "°C";
    }
    gtk_menu_item_set_label(GTK_MENU_ITEM(state->status_item), status_text.c_str());

    return G_SOURCE_CONTINUE;
}

void on_quit(GtkMenuItem* /*item*/, gpointer /*user_data*/) {
    gtk_main_quit();
}

}  // namespace

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);

    AppState state;
    state.paths = discover_chassis3();

    state.indicator = app_indicator_new(
        "chassis3-fan-tray",
        "sensors-fan-symbolic",
        APP_INDICATOR_CATEGORY_HARDWARE);
    app_indicator_set_status(state.indicator, APP_INDICATOR_STATUS_ACTIVE);

    GtkWidget* menu = gtk_menu_new();

    state.status_item = gtk_menu_item_new_with_label("Chassis Fan 3: -- RPM");
    gtk_widget_set_sensitive(state.status_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), state.status_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    const bool has_pwm = !state.paths.pwm_path.empty();
    if (has_pwm) {
        if (auto cur_pwm = read_trimmed(state.paths.pwm_path)) {
            try {
                state.turbo_on = (std::stoi(*cur_pwm) >= 240);
            } catch (...) {}
        }
    }
    if (!has_pwm) {
        GtkWidget* ro_item = gtk_menu_item_new_with_label("Turbo Mode: Read-Only (nct6775/it87 not loaded)");
        gtk_widget_set_sensitive(ro_item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), ro_item);
    } else {
        state.turbo_item = gtk_check_menu_item_new_with_label("Turbo Mode (100% Fan)");
        if (state.turbo_on) {
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(state.turbo_item), TRUE);
        }
        g_signal_connect(state.turbo_item, "toggled", G_CALLBACK(toggle_turbo), &state);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), state.turbo_item);

        state.auto_turbo_item = gtk_check_menu_item_new_with_label("Auto Turbo (V100 ≥ 60°C)");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(state.auto_turbo_item), TRUE);
        g_signal_connect(state.auto_turbo_item, "toggled", G_CALLBACK(toggle_auto_turbo), &state);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), state.auto_turbo_item);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget* ver_item = gtk_menu_item_new_with_label("fan-tray v1.0.0");
    gtk_widget_set_sensitive(ver_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), ver_item);

    GtkWidget* quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);
    app_indicator_set_menu(state.indicator, GTK_MENU(menu));

    update_tray(&state);
    g_timeout_add_seconds(2, update_tray, &state);

    gtk_main();
    return 0;
}
