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
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/time.h>
#include <libevdev/libevdev.h>



#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef KEY_POTATO
#define KEY_POTATO KEY_RIGHTCTRL
#endif

#ifndef TOGGLE
#define TOGGLE false
#endif


static int epoll_fd;
static int num_devices = 0;
static struct libevdev *devs[64];
static int device_fds[64];

static void add_keyboard(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0 ) {
        return;
    }

    struct libevdev *dev = NULL;
    int rc = libevdev_new_from_fd(fd, &dev);
    if(rc < 0) {
        close(fd);
        return;
    }

    // checking is the device a keyboard
    if (libevdev_has_event_type(dev, EV_KEY) && libevdev_has_event_code(dev, EV_KEY, KEY_SPACE)){

        ioctl(fd, EVIOCGRAB, 1); // taking the device

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fd;

        if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0) {
            if(num_devices < 64) {
                devs[num_devices] = dev;
                device_fds[num_devices] = fd;
                num_devices ++;
            } else {
                libevdev_free(dev);
                close(fd);
            }
        }else {
            libevdev_free(dev);
            close(fd);
        }
    }else {
        libevdev_free(dev);
        close(fd);
    }
}

static void find_devices() {

    DIR *dir;
    struct dirent *ent;
    char path[1024];

    epoll_fd = epoll_create1(0);
    if(epoll_fd < 0){
        perror("epoll_create1 fail.");
        exit(1);
    }

    // searching for devices
    if((dir= opendir("/dev/input/")) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if(strncmp(ent->d_name, "event", 5) == 0){
                snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
                add_keyboard(path);
            }
        }
        closedir(dir);
    }
}

static void cleanup_devs_list() {
    for (int i = 0; i < num_devices; i++){
        ioctl(device_fds[i], EVIOCGRAB, 0); // unloading
        libevdev_free(devs[i]);
        close(device_fds[i]);
    }
    close(epoll_fd);
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
        int slow = ev.shift ? 5 : 2; // 5x -> normal mode cursor speek : 2x -> shift mode cursor speed
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

int main() {
    struct input_event e;

    pthread_mutex_init(&lock, NULL);

    find_devices();
    if (num_devices == 0) {
        fprintf(stderr, "Could not find a keyboard.");
        return 1;
    }


    // setting up the input device
    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) {
        perror("Failed to open uinput");
        return 1;
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
    uidev.id.version = 31;

    if (write(uinput_fd, &uidev, sizeof(uidev)) < 0) {
        perror("Failed to write uinput device");
        close(uinput_fd);
        exit(1);
    }

    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        perror("Failed to create uinput device");
        close(uinput_fd);
        exit(1);
    }

    // Sleep for some time to ensure device is ready
    usleep(300000);

    // initial event status
    ev.mouse = false;
    ev.shift = false;

    int inotify_fd = inotify_init();
    if (inotify_fd >= 0) {
        inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE);
        struct epoll_event in_ev;
        in_ev.events = EPOLLIN;
        in_ev.data.fd = inotify_fd;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, inotify_fd, &in_ev);
    }

    struct epoll_event events[16];


    // checking all the devices
    do {
        int n = epoll_wait(epoll_fd, events, 16, -1);
               if (n < 0) {
                   if (errno == EINTR) continue;
                   perror("epoll_wait failed");
                   break;
               }
               for (int i = 0; i < n; i++) {
                   int active_fd = events[i].data.fd;

                   // 1. DURUM: İşletim sistemi yeni bir cihaz dosyası ekledi (inotify)
                   if (active_fd == inotify_fd) {
                       char buffer[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
                       ssize_t len = read(inotify_fd, buffer, sizeof(buffer));
                       if (len > 0) {
                           const struct inotify_event *event;
                           for (char *ptr = buffer; ptr < buffer + len; ptr += sizeof(struct inotify_event) + event->len) {
                               event = (const struct inotify_event *) ptr;
                               if ((event->mask & IN_CREATE) && strncmp(event->name, "event", 5) == 0) {
                                   usleep(200000); // Wait for udev to set permissions
                                   char new_path[PATH_MAX];
                                   snprintf(new_path, sizeof(new_path), "/dev/input/%s", event->name);
                                   add_keyboard(new_path);
                               }
                           }
                       }
                       continue;
                   }


                   int rc = read(active_fd, &e, sizeof(e));
                               if (rc < 0) {
                                   if (errno == ENODEV) {
                                       for (int j = 0; j < num_devices; j++) {
                                           if (device_fds[j] == active_fd) {
                                               fprintf(stderr, "\n[!] Klavye bağlantısı koptu: %s\n", libevdev_get_name(devs[j]));
                                               epoll_ctl(epoll_fd, EPOLL_CTL_DEL, active_fd, NULL);
                                               libevdev_free(devs[j]);
                                               close(active_fd);
                                               for (int k = j; k < num_devices - 1; k++) {
                                                   devs[k] = devs[k+1];
                                                   device_fds[k] = device_fds[k+1];
                                               }
                                               num_devices--;
                                               break;
                                           }
                                       }
                                       // Reset toggles in case mouse was active
                                       pthread_mutex_lock(&lock);
                                       loop_enabled = false;
                                       pthread_mutex_unlock(&lock);
                                       ev.mouse = false;
                                       ev.shift = false;
                                       memset(buttons_status, 0, sizeof(buttons_status));
                                   }
                                   continue;
                               } else if (rc < (int)sizeof(e)) {
                                   continue;
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
                           }
                       } while (1);

                       // Cleanup
                       ioctl(uinput_fd, UI_DEV_DESTROY);
                       close(uinput_fd);
                       cleanup_devs_list();
                       if (inotify_fd >= 0) close(inotify_fd);
                       pthread_mutex_destroy(&lock);

                       return 0;
                   }
