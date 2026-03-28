#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef KEY_POTATO
#define KEY_POTATO KEY_RIGHTCTRL
#endif

#ifndef TOGGLE
#define TOGGLE false
#endif

static void list_devices() {
    DIR *dir;
    struct dirent *ent;
    char path[1024];
    printf("Searching for input devices:\n");
    if ((dir = opendir("/dev/input/")) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (strncmp(ent->d_name, "event", 5) == 0) {
                sprintf(path, "/dev/input/%s", ent->d_name);
                int fd = open(path, O_RDONLY);
                if(fd < 0) {
                    continue;
                }
                // Get device name
                char name[256];
                if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
                    sprintf(name, "Unknown Device");
                    close(fd);
                    continue;
                }

                printf("%s: %s \n", path, name);
                close(fd);
            }
        }
        closedir(dir);
    }
}

typedef struct {
    int x;
    int y;
    int type;
    int code;
    int value;
    bool shift;
    bool mouse;
} Event;

static Event ev;
pthread_mutex_t lock;
static int uinput_fd;

static void do_event_fn(int type, int code, int value) {
    struct input_event ev;
    gettimeofday(&ev.time, NULL);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    write(uinput_fd, &ev, sizeof(ev));
}

static void do_event(int type, int code, int value) {
    #ifdef DEBUG
    printf("type:%d code:%d value:%d\n", type, code, value);
    #endif
    do_event_fn(type, code, value);
    do_event_fn(EV_SYN, SYN_REPORT, 0);
    usleep(50);
}

static bool loop_enabled = false;
static void* loop(void* arg) {
    pthread_mutex_lock(&lock);
    if(loop_enabled) {
        return NULL;
    }
    loop_enabled = true;
    pthread_mutex_unlock(&lock);
    (void) arg;
    while(1) {
        int slow = ev.shift ? 5 : 1;
        usleep(1337 * slow);
        if (!ev.mouse) {
            pthread_mutex_lock(&lock);
            loop_enabled = false;
            pthread_mutex_unlock(&lock);
            break;
        }
        if (ev.x != 0 || ev.y != 0) {
            if (ev.x != 0) {
                do_event_fn(EV_REL, REL_X, ev.x);
            }
            if (ev.y != 0) {
                do_event_fn(EV_REL, REL_Y, ev.y);
            }
            do_event_fn(EV_SYN, SYN_REPORT, 0);
        }
        if(ev.code != 0) {
            do_event(ev.type, ev.code, ev.value);
            ev.code = 0;
        }
    }
    return NULL;
}

static void process_event(struct input_event e) {
    // X axis
    if (e.code == KEY_A) {
        ev.x = -1 * e.value;
    } else if (e.code == KEY_D) {
        ev.x = e.value;
    } else if (e.code == KEY_W) {
        ev.y = -1 * e.value;
    } else if (e.code == KEY_S) {
        ev.y = e.value;
    }
    // Clicks
    ev.code = 0;
    if (e.value == 1 || e.value == 0) {
        ev.value = e.value;
        ev.type = EV_KEY;
        if (e.code == KEY_Q) {
            ev.code = BTN_LEFT;
        } else if (e.code == KEY_E) {
            ev.code = BTN_RIGHT;
        } else if (e.code == KEY_R) {
            ev.code = BTN_MIDDLE;
        }else if(e.code == KEY_HOME){
            ev.code = BTN_EXTRA;
        }else if(e.code == KEY_END){
            ev.code = BTN_SIDE;
        }
    }
    if(e.code == KEY_PAGEDOWN){
        ev.type = EV_REL;
        ev.code = REL_WHEEL_HI_RES;
        ev.value = -120*(e.value > 0);
    }else if(e.code == KEY_PAGEUP){
        ev.type = EV_REL;
        ev.code = REL_WHEEL_HI_RES;
        ev.value = 120*(e.value > 0);
    }

}

static int buttons_status[512];

int main(int argc, char** argv) {
    struct input_event e;

    pthread_mutex_init(&lock, NULL);
    char dev_path[PATH_MAX];
    if (argc < 2) {
        list_devices();
        return 0;
    } else {
        strncpy(dev_path, argv[1], sizeof(dev_path));
    }

    // Open the input device
    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open device");
        exit(1);
    }

    // Setup uinput device
    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) {
        perror("Failed to open uinput");
        close(fd);
        exit(1);
    }

    // Enable events for the virtual device
    ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(uinput_fd, UI_SET_EVBIT, EV_REL);
    ioctl(uinput_fd, UI_SET_RELBIT, REL_X);
    ioctl(uinput_fd, UI_SET_RELBIT, REL_Y);
    ioctl(uinput_fd, UI_SET_RELBIT, REL_WHEEL_HI_RES);

    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_EXTRA);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_SIDE);

    for (int i=0; i<245;i++) {
        ioctl(uinput_fd, UI_SET_KEYBIT, i);
    }

    // Set up the uinput device
    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    strncpy(uidev.name, "Amogus Mouse Emulator", UINPUT_MAX_NAME_SIZE);
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor = 0x1453;
    uidev.id.product = 0x1299;
    uidev.id.version = 1;

    if (write(uinput_fd, &uidev, sizeof(uidev)) < 0) {
        perror("Failed to write uinput device");
        close(uinput_fd);
        close(fd);
        exit(1);
    }

    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        perror("Failed to create uinput device");
        close(uinput_fd);
        close(fd);
        exit(1);
    }

    // Sleep for some time to ensure device is ready
    usleep(300000);

    // Grab the original device
    ioctl(fd, EVIOCGRAB, 1);

    // initial event status
    ev.mouse = false;
    ev.shift = false;

    do {
        int rc = read(fd, &e, sizeof(e));
        if (rc < (int)sizeof(e)) {
            fprintf(stderr, "Failed to read event!\n");
            break;
        }

        // Process the event
        if (e.type == EV_KEY) {
            if (e.code == KEY_POTATO) {
                for (size_t i = 0; i < 512; i++) {
                    if (buttons_status[i]) {
                        do_event(EV_KEY, i, 0);
                        buttons_status[i] = 0;
                    }
                }
                ev.shift = false;
                #ifdef DEBUG
                printf("toggle: %d %d\n", e.value, ev.mouse);
                #endif
                if(TOGGLE){
                    if(e.value == 1){
                        ev.mouse = !ev.mouse;
                        if(ev.mouse){
                            pthread_t thread;
                            pthread_create(&thread, NULL, loop, NULL);
                        }
                    }
                } else {
                    bool m = ev.mouse;
                    ev.mouse = (e.value > 0);
                    if (ev.mouse && !m) {
                        pthread_t thread;
                        pthread_create(&thread, NULL, loop, NULL);
                    }
                }
            }
            if (e.code == KEY_LEFTSHIFT || e.code == KEY_RIGHTSHIFT) {
                ev.shift = (e.value > 0);
            }
            if (ev.mouse) {
                process_event(e);
                if (e.code == KEY_LEFTCTRL || e.code == KEY_LEFTALT || e.code == KEY_LEFTSHIFT || e.code == KEY_LEFTMETA) {
                    do_event(EV_KEY, e.code, e.value);
                }
            } else {
                buttons_status[e.code] = e.value;
                do_event(EV_KEY, e.code, e.value);
            }
        }
    } while (1);

    // Cleanup
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    close(fd);
    pthread_mutex_destroy(&lock);

    return 0;
}
