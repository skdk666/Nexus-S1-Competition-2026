// Host adapter only: the product interface is compiled from ../src/ui.cpp.
#define SDL_MAIN_HANDLED
#include "ui.h"
#include "hid_actions.h"
#include "sim_support.h"
#include <SDL2/SDL.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

static SDL_Window* g_window = nullptr;
static SDL_Renderer* g_renderer = nullptr;
static SDL_Texture* g_texture = nullptr;
static lv_color_t g_fb[sim::width * sim::height];
static sim::TickClock g_clock;
static sim::Viewport g_view;
static int g_window_w = 0, g_window_h = 0, g_output_w = 0, g_output_h = 0;
static int g_mouse_x = 0, g_mouse_y = 0, g_enc_diff = 0;
static bool g_mouse_down = false, g_encoder_down = false, g_running = true;
static int g_scene = 0, g_brightness = -1;
static std::vector<int> g_macro_ids;
static std::vector<std::string> g_actions;
static std::string g_artifacts;
static const char* g_scenes[] = {"NORMAL", "CODING", "BROWSER", "MEDIA", "PC OFFLINE", "CODING TEMP N/A"};

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void geometry() {
    SDL_GetWindowSize(g_window, &g_window_w, &g_window_h);
    require(SDL_GetRendererOutputSize(g_renderer, &g_output_w, &g_output_h) == 0, SDL_GetError());
    g_view = sim::viewport(g_output_w, g_output_h);
}
static sim::Point mouse_point(int x, int y) {
    return sim::map_mouse(x, y, g_window_w, g_window_h, g_output_w, g_output_h, g_view);
}
static void flush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* pixels) {
    for (int y = area->y1; y <= area->y2; ++y) {
        const size_t count = size_t(area->x2 - area->x1 + 1);
        std::memcpy(&g_fb[y * sim::width + area->x1], pixels, count * sizeof(lv_color_t));
        pixels += count;
    }
    lv_disp_flush_ready(drv);
}
static void mouse_read(lv_indev_drv_t*, lv_indev_data_t* data) {
    const auto point = mouse_point(g_mouse_x, g_mouse_y);
    data->point.x = lv_coord_t(point.x);
    data->point.y = lv_coord_t(point.y);
    data->state = g_mouse_down ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}
static void encoder_read(lv_indev_drv_t*, lv_indev_data_t* data) {
    data->enc_diff = int16_t(g_enc_diff);
    data->state = g_encoder_down ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    g_enc_diff = 0;
}
static void macro_intent(int id) {
    const auto* action=nexus::hidAction(id);
    require(action!=nullptr,"Invalid simulated action ID");
    g_macro_ids.push_back(id);
    char message[48];
    std::snprintf(message, sizeof(message), "SIM %d %s", id, action->name);
    ui_set_macro_status(message);
    std::printf("SIMULATION macro ID=%d intent=%s; NO system key sent\n", id, action->name);
    std::fflush(stdout);
}
static unsigned g_scene_changes=0;
static void navigation_log(const nexus::NavState& s,const char* reason) {
    if(!std::strcmp(reason,"SCENE_CHANGE")) ++g_scene_changes;
    std::printf("[NAV] detected=%s stable=%s active=%s page=%s parent=%s/%s standby=%d pc_online=%d reason=%s\n",
        nexus::sceneName(s.detected_scene),nexus::sceneName(s.stable_scene),nexus::sceneName(s.active_scene),
        nexus::pageName(s.current_page),nexus::sceneName(s.return_parent.scene),nexus::pageName(s.return_parent.page),
        int(s.standby),int(s.pc_online),reason);
    std::fflush(stdout);
}
static void hardware_action(const char* action) {
    g_actions.emplace_back(action ? action : "");
    std::printf("SIMULATION hardware action '%s' ignored; no device operation\n", action ? action : "");
}
static void brightness_intent(int value) {
    g_brightness = value;
    std::printf("SIMULATION brightness=%d; no hardware backlight\n", value);
}
static void scene(int id) {
    require(id >= 0 && id < 6, "Invalid scene");
    g_scene = id;
    static const float cpu[] = {20, 75, 35, 12, 0, 30};
    static const float mem[] = {40, 62, 55, 48, 0, 55};
    static const float temp[] = {45, 62, 51, 44, 0, 0};
    ui_set_pc_state(id != 4, id != 4, id != 4, id != 4 && id != 5);
    ui_set_mode(id < 4 ? id : (id == 5 ? 1 : 0));
    ui_update_sensor(24.5f, 42.0f, temp[id], cpu[id], mem[id]);
    ui_set_environment_valid(true); // Explicit simulation, never real sensor data.
    // Never pretend this host preview has real WiFi/MQTT connectivity.
    ui_set_wifi_status(false, "");
    ui_set_mqtt_status(false);
    const std::string title = std::string("SIMULATION | Nexus S1 | ") + g_scenes[id]
        + " | 1-4 Context, 5 Offline, 6 N/A | Mouse; arrows/Space | Esc quits";
    SDL_SetWindowTitle(g_window, title.c_str());
    std::printf("SIMULATION scene=%s cpu=%.0f%% mem=%.0f%% temperature=%s\n",
                g_scenes[id], cpu[id], mem[id], id == 4 || id == 5 ? "N/A" : "simulated");
}
static void handle_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_QUIT: g_running = false; break;
    case SDL_WINDOWEVENT:
        if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) geometry();
        if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            g_mouse_down = g_encoder_down = false;
            g_enc_diff = 0;
        }
        break;
    case SDL_MOUSEMOTION:
        g_mouse_x = event.motion.x; g_mouse_y = event.motion.y;
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (event.button.button != SDL_BUTTON_LEFT) break;
        g_mouse_x = event.button.x; g_mouse_y = event.button.y;
        if (event.type == SDL_MOUSEBUTTONUP) g_mouse_down = false;
        else g_mouse_down = mouse_point(g_mouse_x, g_mouse_y).inside;
        break;
    case SDL_KEYDOWN:
        if (event.key.repeat) break;
        if (event.key.keysym.sym >= SDLK_1 && event.key.keysym.sym <= SDLK_6)
            scene(int(event.key.keysym.sym - SDLK_1));
        else if (event.key.keysym.sym == SDLK_m) scene((g_scene + 1) % 4);
        else if (event.key.keysym.sym == SDLK_ESCAPE) g_running = false;
        else if (event.key.keysym.sym == SDLK_UP) ++g_enc_diff;
        else if (event.key.keysym.sym == SDLK_DOWN) --g_enc_diff;
        else if (event.key.keysym.sym == SDLK_SPACE) g_encoder_down = true;
        break;
    case SDL_KEYUP:
        if (event.key.keysym.sym == SDLK_SPACE) g_encoder_down = false;
        break;
    default: break;
    }
}
static void draw_simulation_banner() {
    // Host-only safety label outside the 320x240 product viewport; no new font dependency.
    static const uint8_t letters[10][7] = {
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,31}, {17,27,21,21,17,17,17},
        {17,17,17,17,17,17,14}, {16,16,16,16,16,16,31}, {14,17,17,31,17,17,17},
        {31,4,4,4,4,4,4}, {31,4,4,4,4,4,31}, {14,17,17,17,17,17,14},
        {17,25,21,19,17,17,17}
    };
    SDL_SetRenderDrawColor(g_renderer, 255, 190, 40, 255);
    for (int letter = 0; letter < 10; ++letter)
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (letters[letter][row] & (1 << (4-col))) {
                    SDL_Rect pixel{8 + letter*12 + col*2, 7 + row*2, 2, 2};
                    SDL_RenderFillRect(g_renderer, &pixel);
                }
}
static void render(bool present = true) {
    require(SDL_UpdateTexture(g_texture, nullptr, g_fb, sim::width * int(sizeof(lv_color_t))) == 0, SDL_GetError());
    SDL_SetRenderDrawColor(g_renderer, 5, 5, 8, 255);
    SDL_RenderClear(g_renderer);
    SDL_Rect area{g_view.x, g_view.y, g_view.w, g_view.h};
    if (area.w > 0 && area.h > 0) SDL_RenderCopy(g_renderer, g_texture, nullptr, &area);
    draw_simulation_banner();
    if (present) SDL_RenderPresent(g_renderer);
}
static void step() {
    geometry();
    SDL_Event event;
    while (SDL_PollEvent(&event)) handle_event(event);
    lv_tick_inc(g_clock.advance(SDL_GetTicks()));
    lv_timer_handler();
    render();
}
static void save_screenshot(const char* name) {
    if (g_artifacts.empty()) return;
    SDL_Surface* image = SDL_CreateRGBSurfaceWithFormat(0, g_output_w, g_output_h, 32, SDL_PIXELFORMAT_ARGB8888);
    require(image != nullptr, SDL_GetError());
    render(false); // Read the current backbuffer before Present (required by SDL renderers).
    const int read = SDL_RenderReadPixels(g_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, image->pixels, image->pitch);
    const auto path = (std::filesystem::path(g_artifacts) / (std::string(name) + ".bmp")).string();
    const int saved = read == 0 ? SDL_SaveBMP(image, path.c_str()) : -1;
    SDL_FreeSurface(image);
    SDL_RenderPresent(g_renderer);
    require(read == 0 && saved == 0, "Could not save SDL screenshot");
}
static void initialize() {
    SDL_SetMainReady();
    require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0, SDL_GetError());
    g_window = SDL_CreateWindow("SIMULATION | Nexus S1", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                640, 508, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    require(g_window != nullptr, SDL_GetError());
    SDL_SetWindowMinimumSize(g_window, 320, 268);
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    require(g_renderer != nullptr, SDL_GetError());
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                  sim::width, sim::height);
    require(g_texture != nullptr, SDL_GetError());
    static_assert(sizeof(lv_color_t) == 2, "Preview requires the current RGB565 configuration");
    lv_init();
    static lv_color_t pixels[sim::width * 10];
    static lv_disp_draw_buf_t draw;
    lv_disp_draw_buf_init(&draw, pixels, nullptr, sim::width * 10);
    static lv_disp_drv_t display;
    lv_disp_drv_init(&display);
    display.hor_res = sim::width; display.ver_res = sim::height;
    display.draw_buf = &draw; display.flush_cb = flush;
    lv_disp_drv_register(&display);
    ui_set_navigation_callback(navigation_log);
    ui_init();
    ui_set_macro_callback(macro_intent);
    ui_set_action_callback(hardware_action);
    ui_set_brightness_callback(brightness_intent);
    static lv_indev_drv_t mouse, encoder;
    lv_indev_drv_init(&mouse); mouse.type = LV_INDEV_TYPE_POINTER; mouse.read_cb = mouse_read;
    lv_indev_drv_register(&mouse);
    lv_indev_drv_init(&encoder); encoder.type = LV_INDEV_TYPE_ENCODER; encoder.read_cb = encoder_read;
    lv_indev_set_group(lv_indev_drv_register(&encoder), ui_get_encoder_group());
    scene(0);
    geometry();
    g_clock.advance(SDL_GetTicks()); // The first sample establishes a baseline, not elapsed time.
    SDL_RendererInfo info{}; SDL_GetRendererInfo(g_renderer, &info);
    std::printf("SIMULATION window shown: video=%s renderer=%s; no serial/USB/Windows key output\n",
                SDL_GetCurrentVideoDriver(), info.name);
}

#include "self_test.h"

int main(int argc, char** argv) {
    bool test = false;
    double duration = 0;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--self-test") test = true;
            else if (arg == "--artifacts" && i+1 < argc) g_artifacts = argv[++i];
            else if (arg == "--duration" && i+1 < argc) {
                duration = std::stod(argv[++i]);
                require(std::isfinite(duration) && duration > 0, "Invalid duration");
            } else throw std::runtime_error("Usage: nexus_s1_sim [--self-test] [--duration seconds] [--artifacts directory]");
        }
        initialize();
        if (test) run_self_test();
        else {
            const auto start = std::chrono::steady_clock::now();
            while (g_running) {
                step(); SDL_Delay(5);
                if (duration > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count() >= duration)
                    break;
            }
            save_screenshot("interactive_simulation");
        }
        SDL_DestroyTexture(g_texture); SDL_DestroyRenderer(g_renderer); SDL_DestroyWindow(g_window); SDL_Quit();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SIMULATION FAILED: %s\n", error.what());
        SDL_DestroyTexture(g_texture); SDL_DestroyRenderer(g_renderer); SDL_DestroyWindow(g_window); SDL_Quit();
        return 1;
    }
}
