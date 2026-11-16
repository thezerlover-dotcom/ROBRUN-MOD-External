/* rr_esp.c - Overlay entry point, window, input and worker threads.
 *
 * The reader thread samples the game through rr_game.c and publishes
 * double-buffered snapshots. The render thread consumes the newest snapshot,
 * runs aim assist, and asks the window to repaint when the frame changes.
 * The window thread owns presentation and forwards input to the shared
 * settings in rr_config.c. menu_webview.cpp hosts the settings UI.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "menu_bridge.h"
#include "gpu.h"
#include "rr_mem.h"
#include "rr_unity.h"
#include "rr_game.h"
#include "rr_draw.h"
#include "rr_aim.h"
#include "rr_los.h"
#include "robrun_offsets.h"
#include "version.h"

#pragma comment(lib, "winmm.lib")

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#define GAME_EXE "ROB RUN.exe"

static HWND  g_hwnd = NULL;
static int   g_sw = 0, g_sh = 0;
static volatile LONG g_quit = 0;

/* Snapshot published by the reader and consumed by the renderer. */
static CRITICAL_SECTION g_cs;
static RR_PLAYER g_buf[2][RR_MAX_PLAYERS];
static RR_CAMERA g_cam[2];
static int       g_nbuf[2];
static int       g_front = 0;
static unsigned  g_snapshot_seq;
static DWORD     g_snapshot_at;
static volatile LONG g_repaint_pending;
static volatile LONG g_present_retry;
static int       g_nrender = 0;
static RR_PLAYER g_render[RR_MAX_PLAYERS];
static RR_CAMERA g_rcam;

/* A focus-return event asks the reader to re-check scene pointers. */
static volatile LONG g_rescan_now = 0;

/* Hotkeys arrive through WM_HOTKEY, Raw Input, the low-level hook, and a
 * polled fallback. The physical key state and a short debounce collapse
 * duplicate deliveries into one action. */
static HHOOK g_kb_hook = NULL;
static DWORD g_hk_last[4] = { 0, 0, 0, 0 };
static volatile LONG g_hk_down[4] = { 0, 0, 0, 0 };

static void hk_action(int id) {
    BR_Config* config = br_get();
    if (id == 1) {
        br_request_toggle();
    } else if (id == 2) {
        config->enabled = !config->enabled;
    } else if (id == 3) {
        InterlockedExchange(&g_quit, 1);
        if (g_hwnd) PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
    }
}

static int vk_to_id(int vk) {
    if (vk == VK_F1) return 1;
    if (vk == VK_INSERT) return 2;
    if (vk == VK_END) return 3;
    return 0;
}

static void hk_release(int id) {
    if (id >= 1 && id <= 3) InterlockedExchange(&g_hk_down[id], 0);
}

static void hk_deliver(int id) {
    if (id < 1 || id > 3) return;

    DWORD now = GetTickCount();
    int was_down = (int)InterlockedCompareExchange(&g_hk_down[id], 1, 0);
    DWORD previous = (DWORD)InterlockedExchange((volatile LONG*)&g_hk_last[id], (LONG)now);
    DWORD age = now - previous;

    if (was_down && age < 2000) return;
    if (age < 300) return;
    hk_action(id);
}

/* Low-level hook: catches keys while the game owns the foreground and when
 * RegisterHotKey was refused. */
static LRESULT CALLBACK kb_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && g_hwnd) {
        const KBDLLHOOKSTRUCT* key = (const KBDLLHOOKSTRUCT*)lparam;
        int id = vk_to_id((int)key->vkCode);
        if (id) {
            if (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)
                PostMessageA(g_hwnd, WM_APP + 2, (WPARAM)id, 0);
            else if (wparam == WM_KEYUP || wparam == WM_SYSKEYUP)
                PostMessageA(g_hwnd, WM_APP + 3, (WPARAM)id, 0);
        }
    }
    return CallNextHookEx(NULL, code, wparam, lparam);
}

static int game_focused(void) {
    DWORD pid = 0;
    HWND foreground = GetForegroundWindow();
    if (foreground) GetWindowThreadProcessId(foreground, &pid);
    return g_rr_pid && pid == g_rr_pid;
}

typedef struct { DWORD pid; HWND window; long area; } GAME_WINDOW_SEARCH;

static BOOL CALLBACK find_game_window(HWND window, LPARAM param) {
    GAME_WINDOW_SEARCH* search = (GAME_WINDOW_SEARCH*)param;
    DWORD pid = 0;
    RECT client;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid || !IsWindowVisible(window) || IsIconic(window) ||
        GetWindow(window, GW_OWNER) || !GetClientRect(window, &client))
        return TRUE;
    long width = client.right - client.left;
    long height = client.bottom - client.top;
    if (width > 100 && height > 100 && width * height > search->area) {
        search->area = width * height;
        search->window = window;
    }
    return TRUE;
}

static void sync_game_window(void) {
    if (!g_rr_pid || !g_hwnd) return;

    GAME_WINDOW_SEARCH search = { g_rr_pid, NULL, 0 };
    EnumWindows(find_game_window, (LPARAM)&search);
    if (!search.window) return;

    RECT client, overlay;
    POINT origin = { 0, 0 };
    if (!GetClientRect(search.window, &client) ||
        !ClientToScreen(search.window, &origin) ||
        !GetWindowRect(g_hwnd, &overlay))
        return;

    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (overlay.left != origin.x || overlay.top != origin.y ||
        overlay.right - overlay.left != width ||
        overlay.bottom - overlay.top != height) {
        SetWindowPos(g_hwnd, HWND_TOPMOST, origin.x, origin.y, width, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

static int raw_input_on(void) {
    RAWINPUTDEVICE device;
    device.usUsagePage = 0x01;
    device.usUsage = 0x06;
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = g_hwnd;
    return RegisterRawInputDevices(&device, 1, sizeof(device)) ? 1 : 0;
}

static void aa_move_mouse(int dx, int dy) {
    if (!dx && !dy) return;
    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(input));
}

static int aa_clear_path(RR_V3 start, RR_V3 end) {
    return rr_los_check(start, end) == RR_LOS_CLEAR;
}

static void aa_tick(unsigned snapshot_seq, DWORD snapshot_at) {
    static unsigned last_snapshot;
    static DWORD last_update;
    static uint64_t locked_addr;
    static int32_t locked_id;
    static int last_group;
    static RR_V3 last_position;
    static DWORD last_position_at;
    static RR_AIM_STEER steer;

    BR_Config* config = br_get();
    DWORD now = timeGetTime();
    int held = config->aa_on && (GetAsyncKeyState(config->aa_key) & 0x8000);
    if (!held) locked_addr = 0;

    if (!held || config->menu_open || !game_focused() ||
        !g_rcam.valid || !g_nrender || now - snapshot_at > 50) {
        steer = (RR_AIM_STEER){ 0 };
        last_position_at = 0;
        return;
    }

    /* Let the game apply the previous input before issuing a correction. */
    DWORD elapsed = now - last_update;
    if (snapshot_seq == last_snapshot || elapsed < 15) return;
    last_snapshot = snapshot_seq;
    last_update = now;

    int group = config->aa_bone;
    RECT client;
    int vw = g_sw, vh = g_sh;
    if (g_hwnd && GetClientRect(g_hwnd, &client)) {
        vw = client.right;
        vh = client.bottom;
    }
    float center_x = vw * 0.5f, center_y = vh * 0.5f;

    RR_AIM_PICK pick = rr_aim_pick(g_render, g_nrender, &g_rcam, vw, vh, config,
                                   locked_addr, locked_id,
                                   config->aa_los_on ? aa_clear_path : NULL);
    if (!pick.player) {
        steer = (RR_AIM_STEER){ 0 };
        last_position_at = 0;
        return;
    }

    const RR_PLAYER* player = pick.player;
    if (!locked_addr || last_group != group) {
        steer = (RR_AIM_STEER){ 0 };
    } else {
        RR_V3 predicted;
        float sx, sy;
        if (last_position_at && !player->inveh &&
            rr_aim_motion_point(pick.point, player->pos, last_position,
                                snapshot_at - last_position_at, now - snapshot_at,
                                config->aa_smooth, &predicted) &&
            game_w2s(&g_rcam, &predicted, vw, vh, &sx, &sy)) {
            float dx = sx - center_x, dy = sy - center_y;
            if (isfinite(dx) && isfinite(dy) &&
                (!config->aa_los_on || aa_clear_path(g_rcam.pos, predicted))) {
                pick.dx = dx;
                pick.dy = dy;
            }
        }
    }

    locked_addr = player->addr;
    locked_id = player->id;
    last_group = group;
    last_position = player->pos;
    last_position_at = snapshot_at;

    int mx, my;
    rr_aim_step(&steer, pick.dx, pick.dy, g_rcam.fov, vh, config->aa_smooth,
                config->aa_aim_cal, elapsed,
                (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, &mx, &my);
    aa_move_mouse(mx, my);
}

/* Exit the overlay without requesting driver unload. rr_drv_close() closes
 * this client handle only; the mapped image stays resident until reboot. */
static void overlay_exit(int code) {
    br_config_save();
    ExitProcess((UINT)code);
}

static DWORD WINAPI ReadThread(LPVOID unused) {
    (void)unused;
    timeBeginPeriod(1);
    int tick = 0;
    int dead_ticks = 0;
    int game_gone = 0;

    /* Two full player arrays do not fit comfortably on this thread's stack
     * while rig traversal is running. */
    static RR_PLAYER snapshot[RR_MAX_PLAYERS];

    for (;;) {
        if (g_rr_pid == 0) {
            if (rr_drv_open()) rr_attach(GAME_EXE);
            if (g_rr_pid) game_init();
        }
        if (InterlockedCompareExchange(&g_quit, 0, 0)) break;

        RR_CAMERA camera;
        memset(&camera, 0, sizeof(camera));
        int count = 0;
        __try {
            count = game_snapshot(snapshot, RR_MAX_PLAYERS, &camera);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            count = 0;
        }

        /* Re-run discovery when focus returned or the world anchor vanished.
         * A nonzero entry count alone does not prove discovery succeeded. */
        if (InterlockedExchange(&g_rescan_now, 0) && !game_discovery_active()) {
            game_init();
            count = game_snapshot(snapshot, RR_MAX_PLAYERS, &camera);
        }

        /* Check the image signature for attachment liveness. Three failed
         * checks trigger reattachment while tolerating a brief read failure. */
        if ((tick % 20) == 0) {
            uint8_t mz[2];
            uint64_t image = rr_image_base();
            if (image && rr_read(image, mz, sizeof(mz))) {
                dead_ticks = 0;
            } else if (++dead_ticks >= 3) {
                dead_ticks = 0;
                game_reattach_reset();
                if (rr_attach(GAME_EXE)) {
                    game_init();
                    count = game_snapshot(snapshot, RR_MAX_PLAYERS, &camera);
                } else {
                    g_rr_pid = 0;
                }
            }
        }

        /* Two consecutive successful process snapshots that omit the game
         * confirm that it exited. Enumeration errors do not count. */
        if ((tick % 240) == 0 && g_rr_pid) {
            if (rr_process_visible(g_rr_pid) == 0) {
                if (++game_gone >= 2) overlay_exit(0);
            } else {
                game_gone = 0;
            }
        }

        if (!game_discovery_active() && (tick % 40) == 0 &&
            (count == 0 || game_world_addr() == 0)) {
            game_init();
            count = game_snapshot(snapshot, RR_MAX_PLAYERS, &camera);
        }

        EnterCriticalSection(&g_cs);
        int back = g_front ^ 1;
        memcpy(g_buf[back], snapshot, sizeof(RR_PLAYER) * (size_t)count);
        g_nbuf[back] = count;
        g_cam[back] = camera;
        g_front = back;
        ++g_snapshot_seq;
        g_snapshot_at = timeGetTime();
        LeaveCriticalSection(&g_cs);

        tick++;

        /* A high-resolution waitable timer gives ~1 ms periods without the
         * CPU cost of a spin loop. */
        {
            static HANDLE tick_timer;
            if (!tick_timer) {
                tick_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002,
                                                    TIMER_ALL_ACCESS);
                if (!tick_timer) tick_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
                LARGE_INTEGER due;
                due.QuadPart = -10000;
                SetWaitableTimer(tick_timer, &due, 1, NULL, NULL, FALSE);
            }
            WaitForSingleObject(tick_timer, 3);
        }
    }
    return 0;
}

/* Hash frame inputs to skip presenting an unchanged frame. */
static uint64_t frame_signature(void) {
    uint64_t hash = 1469598103934665603ULL;
#define MIXB(v) do { uint32_t _bits; memcpy(&_bits, &(v), 4); hash ^= _bits; hash *= 1099511628211ULL; } while (0)
#define MIX(v)  do { hash ^= (uint64_t)(v); hash *= 1099511628211ULL; } while (0)

    MIX(g_nrender);
    MIXB(g_rcam.pos.x); MIXB(g_rcam.pos.y); MIXB(g_rcam.pos.z);
    MIXB(g_rcam.fwd.x); MIXB(g_rcam.fwd.y); MIXB(g_rcam.fwd.z);
    MIXB(g_rcam.right.x); MIXB(g_rcam.right.y); MIXB(g_rcam.right.z);
    MIXB(g_rcam.up.x); MIXB(g_rcam.up.y); MIXB(g_rcam.up.z); MIXB(g_rcam.fov);
    MIXB(g_rcam.rot.x); MIXB(g_rcam.rot.y); MIXB(g_rcam.rot.z); MIXB(g_rcam.rot.w);

    BR_Config* config = br_get();
    MIX(config->enabled); MIX(config->show_box); MIX(config->show_name);
    MIX(config->show_distance); MIX(config->show_head); MIX(config->show_self);
    MIX(config->show_skeleton); MIX(config->show_weapon); MIX(config->show_armor);
    MIX(config->show_crouch); MIX(config->show_health);
    MIX(config->show_vehicle_markers); MIX(config->show_vehicle_occupants);
    MIX(config->cham_on); MIX(config->radar_on); MIX(config->radar_range);
    MIX(config->radar_world); MIX(config->show_containers); MIX(config->show_doors);
    MIX(config->show_dropped_loot); MIX(config->show_vehicle_loot);
    MIX(config->line_w); MIX(config->aa_on); MIX(config->aa_fov_px);
    MIX(config->menu_open); MIX(config->max_distance); MIX(config->skel_distance);
    MIX(config->show_clan); MIX(config->show_police); MIX(config->show_loot);
    MIX(config->show_loot_nodes); MIX(config->vehicle_chams);
    MIX(config->loot_tier_none); MIX(config->loot_tier_common);
    MIX(config->loot_tier_uncommon); MIX(config->loot_tier_rare);
    MIX(config->loot_tier_epic); MIX(config->loot_tier_legendary);
    MIX(config->loot_tier_exotic);

    int limit = g_nrender;
    for (int i = 0; i < limit; ++i) {
        const RR_PLAYER* player = &g_render[i];
        MIXB(player->pos.x); MIXB(player->pos.y); MIXB(player->pos.z);
        MIXB(player->head.x); MIXB(player->head.y); MIXB(player->head.z);
        MIXB(player->dist);
        MIX(player->relationships); MIX(player->full_rig);
        if (config->radar_on && player->local) MIXB(player->rot_yaw);
        MIX(player->id); MIX(player->local); MIX(player->hidden_in_object);
        MIX(player->dead); MIX(player->armor); MIX(player->crouch);
        MIX(player->health); MIX(player->is_vehicle); MIX(player->is_loot);
        MIX(player->loot_rarity); MIX(player->occupants);
        MIX(player->occupants_known);
        if (config->vehicle_chams && player->is_vehicle) {
            MIX(player->vehicle_cham_valid);
            if (player->vehicle_cham_valid)
                for (int j = 0; j < 12; ++j) MIXB(player->vehicle_pose[j]);
        }
        if (config->cham_on) {
            MIX(player->cham_valid);
            MIX(player->cham_rig);
            if (player->cham_valid)
                for (int j = 0; j < RR_CHAM_BONES; ++j)
                    for (int k = 0; k < 12; ++k) MIXB(player->cham_skin[j][k]);
        }
        for (size_t j = 0; j < sizeof(player->name); ++j) MIX((unsigned char)player->name[j]);
        for (size_t j = 0; j < sizeof(player->weapon); ++j) MIX((unsigned char)player->weapon[j]);
        for (size_t j = 0; j < sizeof(player->role); ++j) MIX((unsigned char)player->role[j]);
        if ((config->show_skeleton || config->cham_on) &&
            player->nbones > 0 && player->nbones <= RR_MAX_BONES) {
            MIX(player->nbones);
            for (int k = 0; k < player->nbones; ++k) {
                MIXB(player->bone[k][0]);
                MIXB(player->bone[k][1]);
                MIXB(player->bone[k][2]);
            }
        }
    }
#undef MIX
#undef MIXB
    return hash;
}

static int key_pressed(int vk, int* was_down) {
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int pressed = down && !*was_down;
    *was_down = down;
    return pressed;
}

static DWORD WINAPI RenderThread(LPVOID unused) {
    (void)unused;
    timeBeginPeriod(1);
    int f1_down = 0, insert_down = 0, end_down = 0;
    uint64_t last_signature = 0;
    DWORD last_post = 0;

    for (;;) {
        if (InterlockedCompareExchange(&g_quit, 0, 0)) break;

        if (key_pressed(VK_F1, &f1_down)) hk_deliver(1);
        else if (!f1_down) hk_release(1);
        if (key_pressed(VK_INSERT, &insert_down)) hk_deliver(2);
        else if (!insert_down) hk_release(2);
        if (key_pressed(VK_END, &end_down)) {
            hk_deliver(3);
            break;
        } else if (!end_down) {
            hk_release(3);
        }

        EnterCriticalSection(&g_cs);
        g_nrender = g_nbuf[g_front];
        memcpy(g_render, g_buf[g_front], sizeof(RR_PLAYER) * (size_t)g_nrender);
        g_rcam = g_cam[g_front];
        unsigned snapshot_seq = g_snapshot_seq;
        DWORD snapshot_at = g_snapshot_at;
        LeaveCriticalSection(&g_cs);

        __try {
            aa_tick(snapshot_seq, snapshot_at);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /* A bad game read must not terminate the overlay. */
        }

        BR_Config* config = br_get();
        DWORD now = timeGetTime();
        int period = config->menu_open ? 16 : 1;
        if (g_nrender == 0) period = 200;
        if (g_hwnd && now - last_post >= (DWORD)period &&
            !InterlockedCompareExchange(&g_repaint_pending, 0, 0)) {
            uint64_t signature = frame_signature();
            if ((signature != last_signature || now - last_post > 500 ||
                 InterlockedCompareExchange(&g_present_retry, 0, 0)) &&
                gpu_frame_ready()) {
                InterlockedExchange(&g_repaint_pending, 1);
                if (PostMessageA(g_hwnd, WM_APP + 1, 0, 0)) {
                    last_signature = signature;
                    last_post = now;
                } else {
                    InterlockedExchange(&g_repaint_pending, 0);
                }
            }
        }
        Sleep(g_nrender > 0 ? 1 : 48);
    }
    return 0;
}

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_TIMER:
        if (wparam == 18) sync_game_window();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
    case WM_SETFOCUS:
        InterlockedExchange(&g_rescan_now, 1);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_APP + 1:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_HOTKEY:
        hk_deliver((int)wparam);
        return 0;
    case WM_APP + 2:
        hk_deliver((int)wparam);
        return 0;
    case WM_APP + 3:
        hk_release((int)wparam);
        return 0;
    case WM_INPUT: {
        static BYTE buffer[128];
        UINT size = sizeof(buffer);
        if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, buffer, &size,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            RAWINPUT* input = (RAWINPUT*)buffer;
            if (input->header.dwType == RIM_TYPEKEYBOARD) {
                const RAWKEYBOARD* key = &input->data.keyboard;
                int id = vk_to_id((int)key->VKey);
                if (id && (key->Flags & RI_KEY_BREAK)) hk_release(id);
                else if (id) hk_deliver(id);
            }
        }
        return DefWindowProc(hwnd, message, wparam, lparam);
    }
    case WM_PAINT: {
        PAINTSTRUCT paint;
        static RR_PLAYER players[RR_MAX_PLAYERS];
        RR_CAMERA camera;
        int count;

        EnterCriticalSection(&g_cs);
        count = g_nrender;
        if (count < 0) count = 0;
        if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;
        memcpy(players, g_render, sizeof(RR_PLAYER) * (size_t)count);
        camera = g_rcam;
        LeaveCriticalSection(&g_cs);

        BeginPaint(hwnd, &paint);
        RECT client;
        GetClientRect(hwnd, &client);
        int width = client.right - client.left;
        int height = client.bottom - client.top;

        if (width > 0 && height > 0) {
            if (!gpu_ready() && !gpu_init(hwnd, width, height)) {
                MessageBoxA(NULL, "The graphics backend (D3D11/DComp) is unavailable.",
                            "RobRun", MB_ICONERROR);
                EndPaint(hwnd, &paint);
                InterlockedExchange(&g_repaint_pending, 0);
                return 0;
            }
            gpu_size(width, height);
            game_refresh_render_camera(&camera);

            __try {
                gpu_begin();
                __try {
                    draw_esp(players, count, &camera, width, height);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    /* Keep the presentation path alive if a snapshot is bad. */
                }
                InterlockedExchange(&g_present_retry, !gpu_present());
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedExchange(&g_present_retry, 1);
            }
        }

        EndPaint(hwnd, &paint);
        InterlockedExchange(&g_repaint_pending, 0);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProc(hwnd, message, wparam, lparam);
}

static int print_version(void) {
    BOOL attached = AttachConsole(ATTACH_PARENT_PROCESS);
    if (!attached && GetLastError() != ERROR_ACCESS_DENIED)
        return 0;

    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    int close_output = 0;
    if (!output || output == INVALID_HANDLE_VALUE) {
        output = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
        close_output = 1;
    }
    if (output && output != INVALID_HANDLE_VALUE) {
        const char text[] = RR_PRODUCT_NAME " " RR_VERSION_STRING "\r\n";
        DWORD written = 0;
        WriteFile(output, text, (DWORD)(sizeof(text) - 1), &written, NULL);
        if (close_output) CloseHandle(output);
    }
    if (attached) FreeConsole();
    return 0;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show) {
    (void)previous;
    (void)command_line;
    (void)show;

    if (wcsstr(GetCommandLineW(), L"--version"))
        return print_version();

    HANDLE single_instance = CreateMutexW(NULL, TRUE, L"Local\\RobRunOverlay");
    if (!single_instance) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(single_instance);
        return 0;
    }

    g_sw = GetSystemMetrics(SM_CXSCREEN);
    g_sh = GetSystemMetrics(SM_CYSCREEN);

    InitializeCriticalSection(&g_cs);
    br_get();
    br_config_load();

    if (!rr_attach(GAME_EXE)) {
        MessageBoxA(NULL, "ROB RUN is not running.", "RobRun", MB_ICONERROR);
        return 1;
    }
    if (!rr_drv_open()) {
        MessageBoxA(NULL, "The kernel backend is unavailable. Map RobRun.sys with RobRunMapper.exe first.",
                    "RobRun", MB_ICONERROR);
        return 2;
    }

    br_set_menu_visible(0);
    br_start_menu();

    WNDCLASSA window_class;
    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = OverlayProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = "RobRunOverlay";
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&window_class);

    g_hwnd = CreateWindowExA(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "RobRunOverlay", "RobRun", WS_POPUP | WS_VISIBLE, 0, 0, g_sw, g_sh,
        NULL, NULL, instance, NULL);
    if (!g_hwnd) {
        br_stop_menu();
        rr_drv_close();
        return 4;
    }

    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);

    RegisterHotKey(g_hwnd, 1, MOD_CONTROL | MOD_NOREPEAT, VK_F1);
    RegisterHotKey(g_hwnd, 2, MOD_CONTROL | MOD_NOREPEAT, VK_INSERT);
    RegisterHotKey(g_hwnd, 3, MOD_CONTROL | MOD_NOREPEAT, VK_END);
    g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_hook_proc,
                                  GetModuleHandleW(NULL), 0);
    sync_game_window();
    SetTimer(g_hwnd, 18, 500, NULL);
    raw_input_on();

    rr_los_start();
    CreateThread(NULL, 0, ReadThread, NULL, 0, NULL);
    CreateThread(NULL, 0, RenderThread, NULL, 0, NULL);

    MSG message;
    while (GetMessage(&message, NULL, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    InterlockedExchange(&g_quit, 1);
    br_stop_menu();
    br_config_save();
    rr_los_stop();
    rr_drv_close();

    if (g_kb_hook) UnhookWindowsHookEx(g_kb_hook);
    UnregisterHotKey(g_hwnd, 1);
    UnregisterHotKey(g_hwnd, 2);
    UnregisterHotKey(g_hwnd, 3);
    CloseHandle(single_instance);
    return 0;
}
