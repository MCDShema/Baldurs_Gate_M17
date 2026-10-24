#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>

#define MAX_INPUT_DEVS 4

static int g_running = 1;
static int g_ev_fds[MAX_INPUT_DEVS];
static int g_ev_count = 0;
static int g_uinput_fd = -1;

enum VirtualAction {
    ACT_NONE = 0,
    ACT_BTN_LEFT,
    ACT_BTN_RIGHT,
    ACT_KEY_SPACE,
    ACT_KEY_I,
    ACT_KEY_M,
    ACT_KEY_J,
    ACT_KEY_C,
    ACT_KEY_TAB,
    ACT_KEY_Q,
    ACT_KEY_ESC,
    ACT_KEY_ENTER,
    ACT_KEY_0
};

static void log_mapper(const char *msg) {
    if (!msg) return;
    const char *paths[] = {
        "/storage/roms/ports/gemrb/baldurs_gate.log",
        "/sdcard/roms/ports/gemrb/baldurs_gate.log",
        "/mnt/sdcard/roms/ports/gemrb/baldurs_gate.log",
        "/sdcard/baldurs_gate.log",
        NULL
    };
    for (int i = 0; paths[i]; ++i) {
        int fd = open(paths[i], O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd >= 0) {
            write(fd, msg, strlen(msg));
            close(fd);
            break;
        }
    }
    write(1, msg, strlen(msg));
}

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void emit_ev(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0) {
        // ignore
    }
}

static void emit_sync(int fd) {
    emit_ev(fd, EV_SYN, SYN_REPORT, 0);
}

static void emit_action(int fd, enum VirtualAction act, int is_press) {
    int val = is_press ? 1 : 0;
    switch (act) {
        case ACT_BTN_LEFT:   emit_ev(fd, EV_KEY, BTN_LEFT, val); break;
        case ACT_BTN_RIGHT:  emit_ev(fd, EV_KEY, BTN_RIGHT, val); break;
        case ACT_KEY_SPACE:  emit_ev(fd, EV_KEY, KEY_SPACE, val); break;
        case ACT_KEY_I:      emit_ev(fd, EV_KEY, KEY_I, val); break;
        case ACT_KEY_M:      emit_ev(fd, EV_KEY, KEY_M, val); break;
        case ACT_KEY_J:      emit_ev(fd, EV_KEY, KEY_J, val); break;
        case ACT_KEY_C:      emit_ev(fd, EV_KEY, KEY_C, val); break;
        case ACT_KEY_TAB:    emit_ev(fd, EV_KEY, KEY_TAB, val); break;
        case ACT_KEY_Q:      emit_ev(fd, EV_KEY, KEY_Q, val); break;
        case ACT_KEY_ESC:    emit_ev(fd, EV_KEY, KEY_ESC, val); break;
        case ACT_KEY_ENTER:  emit_ev(fd, EV_KEY, KEY_ENTER, val); break;
        case ACT_KEY_0:      emit_ev(fd, EV_KEY, KEY_0, val); break;
        default: break;
    }
    emit_sync(fd);
}

static int setup_uinput_mouse_keyboard(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        log_mapper("[m17_mapper ERROR] Cannot open /dev/uinput\n");
        return -1;
    }

    // Relative mouse movement
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);

    // Mouse buttons
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);

    // Keyboard keys for Baldur's Gate shortcuts
    ioctl(fd, UI_SET_KEYBIT, KEY_SPACE);   // Pause / Unpause
    ioctl(fd, UI_SET_KEYBIT, KEY_TAB);     // Highlight ground items/doors
    ioctl(fd, UI_SET_KEYBIT, KEY_I);       // Inventory
    ioctl(fd, UI_SET_KEYBIT, KEY_M);       // Map
    ioctl(fd, UI_SET_KEYBIT, KEY_J);       // Journal
    ioctl(fd, UI_SET_KEYBIT, KEY_C);       // Character Sheet
    ioctl(fd, UI_SET_KEYBIT, KEY_ESC);     // Options / Cancel
    ioctl(fd, UI_SET_KEYBIT, KEY_ENTER);   // Confirm
    ioctl(fd, UI_SET_KEYBIT, KEY_Q);       // Quick Save

    // Party selection keys 0..6
    ioctl(fd, UI_SET_KEYBIT, KEY_0);       // Select all
    ioctl(fd, UI_SET_KEYBIT, KEY_1);       // Char 1
    ioctl(fd, UI_SET_KEYBIT, KEY_2);       // Char 2
    ioctl(fd, UI_SET_KEYBIT, KEY_3);       // Char 3
    ioctl(fd, UI_SET_KEYBIT, KEY_4);       // Char 4
    ioctl(fd, UI_SET_KEYBIT, KEY_5);       // Char 5
    ioctl(fd, UI_SET_KEYBIT, KEY_6);       // Char 6

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "M17 BG Virtual Mouse Keyboard");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;

    if (write(fd, &uidev, sizeof(uidev)) < 0) {
        log_mapper("[m17_mapper ERROR] write uinput_user_dev failed\n");
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        log_mapper("[m17_mapper ERROR] ioctl UI_DEV_CREATE failed\n");
        close(fd);
        return -1;
    }

    return fd;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGHUP, sig_handler);

    char log_buf[256];
    snprintf(log_buf, sizeof(log_buf), "[m17_mapper] Initializing v30 PID %d with EVIOCGRAB...\n", getpid());
    log_mapper(log_buf);

    g_uinput_fd = setup_uinput_mouse_keyboard();
    if (g_uinput_fd < 0) {
        log_mapper("[m17_mapper] FAILED to initialize uinput virtual mouse!\n");
        return 1;
    }
    log_mapper("[m17_mapper] Virtual mouse and keyboard created successfully\n");

    const char *target_devs[] = {
        "/dev/input/event1",
        "/dev/input/event2",
        NULL
    };

    for (int i = 0; target_devs[i]; ++i) {
        int fd = open(target_devs[i], O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            char name[64] = "Unknown";
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            if (strstr(name, "Virtual") || strstr(name, "uinput") || strstr(name, "M17 BG")) {
                close(fd);
                continue;
            }

            // CRITICAL FIX: Grab hardware exclusively so SDL2 cannot see raw stick axes
            // and cannot fight or snap the cursor back to center!
            if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                snprintf(log_buf, sizeof(log_buf), "[m17_mapper] GRABBED exclusive access to %s ('%s')\n", target_devs[i], name);
            } else {
                snprintf(log_buf, sizeof(log_buf), "[m17_mapper] WARNING: Could not grab %s (errno=%d)\n", target_devs[i], errno);
            }
            log_mapper(log_buf);

            g_ev_fds[g_ev_count++] = fd;
        }
    }

    if (g_ev_count == 0) {
        log_mapper("[m17_mapper ERROR] No hardware event devices (event1/event2) found!\n");
        return 1;
    }

    // Wake up SDL2 mouse cursor and position it at center screen (320, 240)
    usleep(150000);
    emit_ev(g_uinput_fd, EV_REL, REL_X, 320);
    emit_ev(g_uinput_fd, EV_REL, REL_Y, 240);
    emit_sync(g_uinput_fd);
    usleep(20000);
    emit_ev(g_uinput_fd, EV_REL, REL_X, 2);
    emit_ev(g_uinput_fd, EV_REL, REL_Y, 2);
    emit_sync(g_uinput_fd);
    log_mapper("[m17_mapper] Mouse wake-up pulse sent to center screen\n");

    // Hardware Stick State (0, 128, 255)
    int stick_x = 128;
    int stick_y = 128;

    // Modifiers & Button States
    int l2_held = 0;
    int r2_held = 0;

    // Active Action per physical button (index: 0=A, 1=B, 2=X, 3=Y, 4=SELECT)
    enum VirtualAction btn_active_action[5] = { ACT_NONE, ACT_NONE, ACT_NONE, ACT_NONE, ACT_NONE };

    // Party selection cycle: 1..6
    int current_party_member = 1;

    // D-Pad state
    int dpad_up = 0, dpad_down = 0, dpad_left = 0, dpad_right = 0;
    int dpad_hold_ticks = 0;

    struct pollfd pfds[MAX_INPUT_DEVS];

    while (g_running) {
        for (int i = 0; i < g_ev_count; ++i) {
            pfds[i].fd = g_ev_fds[i];
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }

        int pret = poll(pfds, g_ev_count, 16); // 16ms -> ~60Hz
        if (pret > 0) {
            for (int dev = 0; dev < g_ev_count; ++dev) {
                if (pfds[dev].revents & POLLIN) {
                    struct input_event evs[32];
                    ssize_t n = read(pfds[dev].fd, evs, sizeof(evs));
                    if (n > 0) {
                        size_t count = n / sizeof(struct input_event);
                        for (size_t i = 0; i < count; ++i) {
                            uint16_t type = evs[i].type;
                            uint16_t code = evs[i].code;
                            int32_t val = evs[i].value;

                            if (type == EV_ABS) {
                                if (code == ABS_X) {
                                    stick_x = val;
                                } else if (code == ABS_Y) {
                                    stick_y = val;
                                }
                            } else if (type == EV_KEY) {
                                if (val == 2) continue; // Ignore autorepeat

                                // Modifier L2 (code 312 BTN_TL2)
                                if (code == 312) {
                                    l2_held = (val != 0);
                                    if (val == 1) {
                                        emit_action(g_uinput_fd, ACT_KEY_0, 1);
                                    } else {
                                        emit_action(g_uinput_fd, ACT_KEY_0, 0);
                                    }
                                }
                                // Modifier R2 (code 313 BTN_TR2)
                                else if (code == 313) {
                                    r2_held = (val != 0);
                                }
                                // Button A: Left Click (or R2+A = TAB Highlight Loot & Doors)
                                // KEY_Z(44), KEY_A(30), BTN_SOUTH/BTN_A(304)
                                else if (code == 44 || code == 30 || code == 304) {
                                    if (val == 1) {
                                        enum VirtualAction act = r2_held ? ACT_KEY_TAB : ACT_BTN_LEFT;
                                        btn_active_action[0] = act;
                                        emit_action(g_uinput_fd, act, 1);
                                    } else {
                                        if (btn_active_action[0] != ACT_NONE) {
                                            emit_action(g_uinput_fd, btn_active_action[0], 0);
                                            btn_active_action[0] = ACT_NONE;
                                        }
                                    }
                                }
                                // Button B: Right Click (or R2+B = Quick Save Q)
                                // KEY_X(45), KEY_B(48), BTN_EAST/BTN_B(305)
                                else if (code == 45 || code == 48 || code == 305) {
                                    if (val == 1) {
                                        enum VirtualAction act = r2_held ? ACT_KEY_Q : ACT_BTN_RIGHT;
                                        btn_active_action[1] = act;
                                        emit_action(g_uinput_fd, act, 1);
                                    } else {
                                        if (btn_active_action[1] != ACT_NONE) {
                                            emit_action(g_uinput_fd, btn_active_action[1], 0);
                                            btn_active_action[1] = ACT_NONE;
                                        }
                                    }
                                }
                                // Button X: Pause Space (or R2+X = Character Sheet C)
                                // KEY_C(46), BTN_NORTH/BTN_X(307)
                                else if (code == 46 || code == 307) {
                                    if (val == 1) {
                                        enum VirtualAction act = r2_held ? ACT_KEY_C : ACT_KEY_SPACE;
                                        btn_active_action[2] = act;
                                        emit_action(g_uinput_fd, act, 1);
                                    } else {
                                        if (btn_active_action[2] != ACT_NONE) {
                                            emit_action(g_uinput_fd, btn_active_action[2], 0);
                                            btn_active_action[2] = ACT_NONE;
                                        }
                                    }
                                }
                                // Button Y: Inventory I (or R2+Y = Map M)
                                // KEY_V(47), KEY_Y(21), BTN_WEST/BTN_Y(308)
                                else if (code == 47 || code == 21 || code == 308) {
                                    if (val == 1) {
                                        enum VirtualAction act = r2_held ? ACT_KEY_M : ACT_KEY_I;
                                        btn_active_action[3] = act;
                                        emit_action(g_uinput_fd, act, 1);
                                    } else {
                                        if (btn_active_action[3] != ACT_NONE) {
                                            emit_action(g_uinput_fd, btn_active_action[3], 0);
                                            btn_active_action[3] = ACT_NONE;
                                        }
                                    }
                                }
                                // Shoulder L1: Cycle Previous Party Member (KEY_1..KEY_6)
                                // KEY_L(38), BTN_TL(310)
                                else if (code == 38 || code == 310) {
                                    if (val == 1) {
                                        current_party_member = (current_party_member <= 1) ? 6 : current_party_member - 1;
                                        uint16_t key = KEY_1 + (current_party_member - 1);
                                        emit_ev(g_uinput_fd, EV_KEY, key, 1);
                                        emit_sync(g_uinput_fd);
                                        emit_ev(g_uinput_fd, EV_KEY, key, 0);
                                        emit_sync(g_uinput_fd);
                                    }
                                }
                                // Shoulder R1: Alternate Left Click (BTN_LEFT)
                                // KEY_R(19), BTN_TR(311)
                                else if (code == 19 || code == 311) {
                                    emit_ev(g_uinput_fd, EV_KEY, BTN_LEFT, val ? 1 : 0);
                                    emit_sync(g_uinput_fd);
                                }
                                // SELECT: Options Menu / Cancel (KEY_ESC), or R2+SELECT = Journal (KEY_J)
                                else if (code == 1 || code == 314 /* BTN_SELECT */) {
                                    if (val == 1) {
                                        enum VirtualAction act = r2_held ? ACT_KEY_J : ACT_KEY_ESC;
                                        btn_active_action[4] = act;
                                        emit_action(g_uinput_fd, act, 1);
                                    } else {
                                        if (btn_active_action[4] != ACT_NONE) {
                                            emit_action(g_uinput_fd, btn_active_action[4], 0);
                                            btn_active_action[4] = ACT_NONE;
                                        }
                                    }
                                }
                                // START: Confirm / Enter (KEY_ENTER)
                                else if (code == 28 /* KEY_ENTER */ || code == 315 /* BTN_START */) {
                                    emit_action(g_uinput_fd, ACT_KEY_ENTER, val ? 1 : 0);
                                }
                                // D-Pad buttons: 2-pixel micro-nudge for pixel precision
                                else if (code == 103 /* KEY_UP */ || code == 544 /* BTN_DPAD_UP */) {
                                    dpad_up = (val != 0);
                                    if (val == 1) {
                                        emit_ev(g_uinput_fd, EV_REL, REL_Y, -2);
                                        emit_sync(g_uinput_fd);
                                    }
                                } else if (code == 108 /* KEY_DOWN */ || code == 545 /* BTN_DPAD_DOWN */) {
                                    dpad_down = (val != 0);
                                    if (val == 1) {
                                        emit_ev(g_uinput_fd, EV_REL, REL_Y, 2);
                                        emit_sync(g_uinput_fd);
                                    }
                                } else if (code == 105 /* KEY_LEFT */ || code == 546 /* BTN_DPAD_LEFT */) {
                                    dpad_left = (val != 0);
                                    if (val == 1) {
                                        emit_ev(g_uinput_fd, EV_REL, REL_X, -2);
                                        emit_sync(g_uinput_fd);
                                    }
                                } else if (code == 106 /* KEY_RIGHT */ || code == 547 /* BTN_DPAD_RIGHT */) {
                                    dpad_right = (val != 0);
                                    if (val == 1) {
                                        emit_ev(g_uinput_fd, EV_REL, REL_X, 2);
                                        emit_sync(g_uinput_fd);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Process Analog Stick with progressive smooth acceleration curve:
        int stick_active = (stick_x < 64 || stick_x > 192 || stick_y < 64 || stick_y > 192);
        static int stick_hold_ticks = 0;
        if (stick_active) {
            stick_hold_ticks++;
        } else {
            stick_hold_ticks = 0;
        }

        int step = 3;
        if (r2_held) {
            step = 1; // Precision micro-targeting
        } else if (l2_held) {
            step = 7; // Fast turbo screen traversal
        } else {
            // Smooth progressive acceleration:
            // 0..8 ticks (~130ms): 2 px/tick -> buttery soft initial move, zero jerky overshoot
            // 9..20 ticks (~150..330ms): 3 px/tick -> smooth transition
            // 21+ ticks (>330ms): 4 px/tick -> comfortable cruise speed
            if (stick_hold_ticks <= 8) {
                step = 2;
            } else if (stick_hold_ticks <= 20) {
                step = 3;
            } else {
                step = 4;
            }
        }

        int dir_x = 0;
        int dir_y = 0;
        if (stick_x < 64) dir_x = -1;
        else if (stick_x > 192) dir_x = 1;

        if (stick_y < 64) dir_y = -1;
        else if (stick_y > 192) dir_y = 1;

        int rel_x = 0;
        int rel_y = 0;
        int moved = 0;

        if (dir_x != 0 && dir_y != 0) {
            // Diagonal motion: normalize step so diagonal doesn't feel faster/jittery
            int diag_step = (step >= 4) ? (step - 1) : step;
            if (step <= 2) diag_step = step;
            rel_x = dir_x * diag_step;
            rel_y = dir_y * diag_step;
            moved = 1;
        } else if (dir_x != 0) {
            rel_x = dir_x * step;
            moved = 1;
        } else if (dir_y != 0) {
            rel_y = dir_y * step;
            moved = 1;
        }

        if (moved) {
            if (rel_x != 0) emit_ev(g_uinput_fd, EV_REL, REL_X, rel_x);
            if (rel_y != 0) emit_ev(g_uinput_fd, EV_REL, REL_Y, rel_y);
            emit_sync(g_uinput_fd);
        }

        // Process continuous D-Pad hold: 2-pixel micro-steps every 2nd tick (~30Hz) after initial delay
        if (dpad_up || dpad_down || dpad_left || dpad_right) {
            dpad_hold_ticks++;
            if (dpad_hold_ticks > 12 && (dpad_hold_ticks % 2 == 0)) {
                int dstep_x = 0, dstep_y = 0;
                if (dpad_up) dstep_y -= 2;
                if (dpad_down) dstep_y += 2;
                if (dpad_left) dstep_x -= 2;
                if (dpad_right) dstep_x += 2;
                if (dstep_x != 0) emit_ev(g_uinput_fd, EV_REL, REL_X, dstep_x);
                if (dstep_y != 0) emit_ev(g_uinput_fd, EV_REL, REL_Y, dstep_y);
                emit_sync(g_uinput_fd);
            }
        } else {
            dpad_hold_ticks = 0;
        }
    }

    log_mapper("[m17_mapper] Shutting down mapper, releasing grabs...\n");
    for (int i = 0; i < g_ev_count; ++i) {
        ioctl(g_ev_fds[i], EVIOCGRAB, 0); // Release grab
        close(g_ev_fds[i]);
    }
    if (g_uinput_fd >= 0) {
        ioctl(g_uinput_fd, UI_DEV_DESTROY);
        close(g_uinput_fd);
    }
    return 0;
}
