#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char rpm_path[256];
    char pwm_path[256];
    char pwm_enable_path[256];
    char cpu_temp_path[256];

    char label[128];
    char status_text[128];
    char gpu_text[128];
    char sys_text[128];

    bool turbo_on;
    bool auto_turbo;
    bool quit;
    uint32_t menu_rev;
} AppState;

static bool read_line(const char* path, char* out, size_t sz) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    if (!fgets(out, (int)sz, f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ')) {
        out[--len] = '\0';
    }
    return true;
}

static void write_str(const char* path, const char* val) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", val);
    fclose(f);
}

static void discover_hw(AppState* s) {
    DIR* d = opendir("/sys/class/hwmon");
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char dir[256], path[300], buf[128];
        snprintf(dir, sizeof(dir), "/sys/class/hwmon/%s", ent->d_name);

        for (int i = 1; i <= 10; i++) {
            snprintf(path, sizeof(path), "%s/fan%d_label", dir, i);
            if (read_line(path, buf, sizeof(buf)) && strcmp(buf, "Chassis Fan 3") == 0) {
                snprintf(s->rpm_path, sizeof(s->rpm_path), "%s/fan%d_input", dir, i);
            }
        }

        snprintf(path, sizeof(path), "%s/name", dir);
        if (read_line(path, buf, sizeof(buf))) {
            if (strncmp(buf, "it86", 4) == 0 || strncmp(buf, "it87", 4) == 0 || strncmp(buf, "nct67", 5) == 0) {
                snprintf(path, sizeof(path), "%s/pwm4", dir);
                if (access(path, F_OK) == 0) {
                    snprintf(s->pwm_path, sizeof(s->pwm_path), "%s", path);
                    snprintf(s->pwm_enable_path, sizeof(s->pwm_enable_path), "%s/pwm4_enable", dir);
                }
            } else if (strcmp(buf, "k10temp") == 0) {
                snprintf(s->cpu_temp_path, sizeof(s->cpu_temp_path), "%s/temp1_input", dir);
            }
        }
    }
    closedir(d);

    char buf[32];
    if (s->pwm_path[0] && read_line(s->pwm_path, buf, sizeof(buf))) {
        s->turbo_on = (atoi(buf) >= 240);
    }
}

static void apply_pwm(AppState* s, bool turbo) {
    s->turbo_on = turbo;
    if (!s->pwm_path[0]) return;
    if (s->pwm_enable_path[0]) write_str(s->pwm_enable_path, "1");
    write_str(s->pwm_path, turbo ? "255" : "135");
    s->menu_rev++;
}

static void update_stats(AppState* s) {
    char rpm[32] = "--";
    if (s->rpm_path[0]) read_line(s->rpm_path, rpm, sizeof(rpm));

    int temp = -1, mem_used = 0, mem_total = 0, pwr = 0, util = 0;
    bool gpu_ok = false;
    FILE* p = popen("nvidia-smi --query-gpu=temperature.gpu,memory.used,memory.total,power.draw,utilization.gpu --format=csv,noheader,nounits 2>/dev/null", "r");
    if (p) {
        char line[128];
        if (fgets(line, sizeof(line), p)) {
            double dpwr = 0;
            if (sscanf(line, "%d, %d, %d, %lf, %d", &temp, &mem_used, &mem_total, &dpwr, &util) == 5) {
                pwr = (int)dpwr;
                gpu_ok = true;
            }
        }
        pclose(p);
    }

    if (gpu_ok && s->auto_turbo && s->pwm_path[0]) {
        if (temp >= 60 && !s->turbo_on) apply_pwm(s, true);
        else if (temp <= 55 && s->turbo_on) apply_pwm(s, false);
    }

    if (gpu_ok) {
        snprintf(s->label, sizeof(s->label), "CHA3: %s RPM | V100: %d°C | %.0f/%.0fG",
                 rpm, temp, mem_used / 1024.0, mem_total / 1024.0);
        snprintf(s->gpu_text, sizeof(s->gpu_text), "V100: %d°C | %dW | Util: %d%% | VRAM: %.1f/%.1f GB",
                 temp, pwr, util, mem_used / 1024.0, mem_total / 1024.0);
    } else {
        snprintf(s->label, sizeof(s->label), "CHA3: %s RPM", rpm);
        snprintf(s->gpu_text, sizeof(s->gpu_text), "V100: --");
    }
    snprintf(s->status_text, sizeof(s->status_text), "Chassis Fan 3: %s RPM", rpm);

    long total_kb = 0, avail_kb = 0;
    FILE* mf = fopen("/proc/meminfo", "r");
    if (mf) {
        char k[64], u[16];
        long v;
        while (fscanf(mf, "%63s %ld %15s", k, &v, u) == 3) {
            if (strcmp(k, "MemTotal:") == 0) total_kb = v;
            else if (strcmp(k, "MemAvailable:") == 0) avail_kb = v;
            if (total_kb && avail_kb) break;
        }
        fclose(mf);
    }
    char cbuf[32];
    if (s->cpu_temp_path[0] && read_line(s->cpu_temp_path, cbuf, sizeof(cbuf))) {
        snprintf(s->sys_text, sizeof(s->sys_text), "System RAM: %.1f/%.1f GB | CPU: %d°C",
                 (total_kb - avail_kb) / 1048576.0, total_kb / 1048576.0, atoi(cbuf) / 1000);
    } else {
        snprintf(s->sys_text, sizeof(s->sys_text), "System RAM: %.1f/%.1f GB",
                 (total_kb - avail_kb) / 1048576.0, total_kb / 1048576.0);
    }
}

static int prop_str(sd_bus* b, const char* p, const char* i, const char* prop, sd_bus_message* reply, void* ud, sd_bus_error* e) {
    (void)b; (void)p; (void)i; (void)e;
    AppState* s = (AppState*)ud;
    if (strcmp(prop, "Id") == 0) return sd_bus_message_append(reply, "s", "chassis3-fan-tray-lite");
    if (strcmp(prop, "Category") == 0) return sd_bus_message_append(reply, "s", "Hardware");
    if (strcmp(prop, "Status") == 0) return sd_bus_message_append(reply, "s", "Active");
    if (strcmp(prop, "IconName") == 0) return sd_bus_message_append(reply, "s", "sensors-fan-symbolic");
    if (strcmp(prop, "XAyatanaLabel") == 0) return sd_bus_message_append(reply, "s", s->label);
    if (strcmp(prop, "XAyatanaLabelGuide") == 0) return sd_bus_message_append(reply, "s", "CHA3: 9999 RPM | V100: 99°C | 32/32G");
    if (strcmp(prop, "Menu") == 0) return sd_bus_message_append(reply, "o", "/MenuBar");
    return 0;
}

static int append_menu_item(sd_bus_message* m, int32_t id, const char* label, bool enabled, const char* toggle_type, int toggle_state) {
    int r = sd_bus_message_open_container(m, 'v', "(ia{sv}av)");
    if (r < 0) return r;
    r = sd_bus_message_open_container(m, 'r', "ia{sv}av");
    if (r < 0) return r;
    sd_bus_message_append(m, "i", id);
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_append(m, "{sv}", "label", "s", label);
    sd_bus_message_append(m, "{sv}", "enabled", "b", enabled);
    if (toggle_type) {
        sd_bus_message_append(m, "{sv}", "toggle-type", "s", toggle_type);
        sd_bus_message_append(m, "{sv}", "toggle-state", "i", toggle_state);
    }
    sd_bus_message_close_container(m);
    sd_bus_message_open_container(m, 'a', "v");
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    return sd_bus_message_close_container(m);
}

static int method_get_layout(sd_bus_message* m, void* ud, sd_bus_error* e) {
    (void)e;
    AppState* s = (AppState*)ud;
    sd_bus_message* reply = NULL;
    sd_bus_message_new_method_return(m, &reply);
    sd_bus_message_append(reply, "u", s->menu_rev);
    sd_bus_message_open_container(reply, 'r', "ia{sv}av");
    sd_bus_message_append(reply, "i", 0);
    sd_bus_message_open_container(reply, 'a', "{sv}");
    sd_bus_message_close_container(reply);
    sd_bus_message_open_container(reply, 'a', "v");

    append_menu_item(reply, 1, s->status_text, false, NULL, 0);
    append_menu_item(reply, 2, s->gpu_text, false, NULL, 0);
    append_menu_item(reply, 3, s->sys_text, false, NULL, 0);
    append_menu_item(reply, 4, "Turbo Mode (100% Fan)", s->pwm_path[0] != '\0', "checkmark", s->turbo_on ? 1 : 0);
    append_menu_item(reply, 5, "Auto Turbo (V100 >= 60°C)", s->pwm_path[0] != '\0', "checkmark", s->auto_turbo ? 1 : 0);
    append_menu_item(reply, 6, "fan-tray-lite v1.1.0 (sd-bus)", false, NULL, 0);
    append_menu_item(reply, 7, "Quit", true, NULL, 0);

    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);
    int r = sd_bus_send(sd_bus_message_get_bus(m), reply, NULL);
    sd_bus_message_unref(reply);
    return r;
}

static int method_event(sd_bus_message* m, void* ud, sd_bus_error* e) {
    (void)e;
    AppState* s = (AppState*)ud;
    int32_t id = 0;
    const char* type = NULL;
    sd_bus_message_read(m, "is", &id, &type);
    if (type && strcmp(type, "clicked") == 0) {
        if (id == 4) apply_pwm(s, !s->turbo_on);
        else if (id == 5) { s->auto_turbo = !s->auto_turbo; s->menu_rev++; }
        else if (id == 7) s->quit = true;
    }
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable sni_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Id", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Category", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Status", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IconName", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("XAyatanaLabel", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("XAyatanaLabelGuide", "s", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Menu", "o", prop_str, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_SIGNAL("XAyatanaNewLabel", "ss", 0),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable menu_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetLayout", "iias", "u(ia{sv}av)", method_get_layout, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Event", "isvu", "", method_event, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

int main(void) {
    AppState state = {.auto_turbo = true, .menu_rev = 1};
    discover_hw(&state);
    update_stats(&state);

    sd_bus* bus = NULL;
    if (sd_bus_open_user(&bus) < 0) return 1;

    sd_bus_add_object_vtable(bus, NULL, "/StatusNotifierItem", "org.kde.StatusNotifierItem", sni_vtable, &state);
    sd_bus_add_object_vtable(bus, NULL, "/MenuBar", "com.canonical.dbusmenu", menu_vtable, &state);

    char svc[64];
    snprintf(svc, sizeof(svc), "org.kde.StatusNotifierItem-%d-1", getpid());
    sd_bus_request_name(bus, svc, 0);
    sd_bus_call_method(bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                       "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                       NULL, NULL, "s", svc);

    time_t last = time(NULL);
    while (!state.quit) {
        sd_bus_process(bus, NULL);
        sd_bus_wait(bus, 500000);
        time_t now = time(NULL);
        if (now - last >= 2) {
            update_stats(&state);
            sd_bus_emit_signal(bus, "/StatusNotifierItem", "org.kde.StatusNotifierItem",
                               "XAyatanaNewLabel", "ss", state.label, "CHA3: 9999 RPM | V100: 99°C | 32/32G");
            last = now;
        }
    }

    sd_bus_unref(bus);
    return 0;
}
