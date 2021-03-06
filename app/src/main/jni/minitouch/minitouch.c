#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <libevdev.h>
#include <sys/types.h>
#include <asm/types.h>
#include <linux/netlink.h>

#define MAX_SUPPORTED_CONTACTS 10
#define VERSION 1
#define DEFAULT_SOCKET_NAME "minitouch"
#define EVENT_NUM 12


static int g_verbose = 0;


static
char *event_str[EVENT_NUM] =
        {
                "IN_ACCESS",
                "IN_MODIFY",
                "IN_ATTRIB",
                "IN_CLOSE_WRITE",
                "IN_CLOSE_NOWRITE",
                "IN_OPEN",
                "IN_MOVED_FROM",
                "IN_MOVED_TO",
                "IN_CREATE",
                "IN_DELETE",
                "IN_DELETE_SELF",
                "IN_MOVE_SELF"
        };



static void usage(const char *pname) {
    fprintf(stderr,
            "Usage: %s [-h] [-d <device>] [-n <name>] [-v] [-i] [-f <file>]\n"
            "  -d <device>: Use the given touch device. Otherwise autodetect.\n"
            "  -n <name>:   Change the name of of the abtract unix domain socket. (%s)\n"
            "  -v:          Verbose output.\n"
            "  -i:          Uses STDIN and doesn't start socket.\n"
            "  -f <file>:   Runs a file with a list of commands, doesn't start socket.\n"
            "  -h:          Show help.\n",
            pname, DEFAULT_SOCKET_NAME
    );
}

typedef struct {
    int enabled;
    int tracking_id;
    int x;
    int y;
    int pressure;
} contact_t; //用来表示触控点的结构体

typedef struct {
    int fd; //文件描述符
    int score; //触摸设备的匹配分值
    char path[100]; //设备的event路径 dev/input/device4
    struct libevdev *evdev;
    int has_mtslot; //type B
    int has_tracking_id; // type B
    int has_key_btn_touch;
    int has_touch_major; //真实接触面积的横轴
    int has_width_major; //触控工具（手指，触控）的接触面积的横轴
    int has_pressure;
    int min_pressure; //最小的压力值 >0
    int max_pressure; //最大的压力值 <=1
    int max_x; //x轴最大的触控范围 1080
    int max_y; //y轴最大的触控范围 1920
    int max_contacts;  //最大的触控点数 多点触控  软件（多点触控具体实现）  屏幕硬件支持（10  小米8（8））
    int max_tracking_id;
    int tracking_id; //type b协议中使用的用来区分触控点的 tracking_id  type B 有状态的多点触控协议
    contact_t contacts[MAX_SUPPORTED_CONTACTS]; // 多点触控点数的数组，最多支持10个触控点
    int active_contacts; //可用的触控点击
} internal_state_touchpad_t; // 记录触控设备的结构体


typedef struct {
    int fd;
    char path[100]; // dev/input/event10 键盘设备
    struct libevdev *evdev;
} internal_state_keyboard_t; //表示键盘设备的结构体


typedef struct {
    internal_state_touchpad_t touchpad;
    internal_state_keyboard_t keyboard;
} internal_state_warper;

static void mappingKeyboardEvent(struct input_event *pEvent, internal_state_touchpad_t *ptr);

static void dealWithActionUp(struct input_event *pEvent, internal_state_touchpad_t *touchpad);

static void dealWithActionDown(struct input_event *pEvent, internal_state_touchpad_t *touchpad);




/**
 *
 * 宏定义   文件类型
 * S_ISREG()   普通文件
 * S_ISDIR()   目录文件
 * S_ISCHR()   字符设备文件
 * S_ISBLK()   块设备文件
 * S_ISFIFO()  有名管道文件
 * S_ISLNK()   软连接(符号链接)文件
 * S_ISSOCK()  套接字文件
 *
 * 判断是否是字符输入设备
 * @param devpath
 * @return
 */
static int is_character_device(const char *devpath) { //https://cnbin.github.io/blog/2015/06/24/linux-wen-jian-io-jie-shao-(er-)/
    struct stat statbuf;

    if (stat(devpath, &statbuf) == -1) {
        perror("stat");
        return 0;
    }

    if (!S_ISCHR(statbuf.st_mode)) {
        return 0;
    }

    return 1;
}

/**
 * 判断是否是触控设备
 * @param evdev
 * @return
 */
static int is_multitouch_device(struct libevdev *evdev) {
    return libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_X);
}

/**
 * 判断是否是键盘设备
 * @param evdev
 * @return
 */
static int is_keyboard_device(struct libevdev *evdev) { // 键盘 A S D F  https://zhidao.baidu.com/question/715096289382651925.html
    int hasA = libevdev_has_event_code(evdev, EV_KEY, KEY_A);
    int hasS = libevdev_has_event_code(evdev, EV_KEY, KEY_S);
    int hasD = libevdev_has_event_code(evdev, EV_KEY, KEY_D);
    int hasF = libevdev_has_event_code(evdev, EV_KEY, KEY_F);
    return hasA && hasS && hasD && hasF;
}

/**
 * 判断是否是鼠标设备
 * @param evdev
 * @return
 */
static int is_mouse_device(struct libevdev *evdev) {
    return libevdev_has_event_code(evdev, EV_KEY, BTN_LEFT) &&
           libevdev_has_event_code(evdev, EV_KEY, BTN_RIGHT);
}

/**
 * 检测当前设备是否是键盘设备
 * @param devpath
 * @param state
 * @return
 */
static int consider_keyboard_device(const char *devpath, internal_state_keyboard_t *state) {
    int fd = -1;
    struct libevdev *evdev = NULL;
    if (!is_character_device(devpath)) {
        goto mismatch;
    }

    if ((fd = open(devpath, O_RDWR)) < 0) { //O_RDWR，以可读可写的方式打开
        perror("open");
        fprintf(stderr, "Unable to open device %s for inspection", devpath);
        goto mismatch;
    }

    if (libevdev_new_from_fd(fd, &evdev) < 0) {
        fprintf(stderr, "Note: device %s is not supported by libevdev\n", devpath);
        goto mismatch;
    }


    libevdev_grab(evdev, LIBEVDEV_UNGRAB);

    if (libevdev_grab(evdev, LIBEVDEV_GRAB) < 0)
        libevdev_free(evdev);

    if (is_keyboard_device(evdev)) {
        state->evdev = evdev;
        state->fd = fd;
        strncpy(state->path, devpath, sizeof(state->path)); //将devpath拷贝到字符数组 state->path 中
        fprintf(stderr, "Find keyboard device:%s,and the path is %s\n", libevdev_get_name(evdev),
                devpath);
        return fd;
    }

    mismatch: //不是需要的设备
    libevdev_grab(evdev, LIBEVDEV_UNGRAB);
    libevdev_free(evdev);
    if (fd >= 0) {
        close(fd); //关闭文件流
    }

    return 0; //返回 0
}

/**
 * 检测当前设备是否是鼠标设备
 * @return
 */
//static int consider_mouse_device(){
//    //todo
//
//}

/**
 * 检测当前设备是否是触控设备
 * @param devpath
 * @param state
 * @return
 */
static int
consider_touch_device(const char *devpath, internal_state_touchpad_t *state) { //state其实是evdev的包装类
    int fd = -1;
    struct libevdev *evdev = NULL; //对libevdev初始化为NULL

    if (!is_character_device(devpath)) { //判断是否是字节设备
        goto mismatch; //无条件转移语句
    }

    if ((fd = open(devpath, O_RDWR)) < 0) {   //监听触控输入设备
        perror("open");
        fprintf(stderr, "Unable to open device %s for inspection", devpath);
        goto mismatch;
    }

    if (libevdev_new_from_fd(fd, &evdev) < 0) { //对 * evdev 指针进行初始化
        fprintf(stderr, "Note: device %s is not supported by libevdev\n", devpath);
        goto mismatch;
    }

    if (!is_multitouch_device(evdev)) { //判断是否是多点触控设备
        goto mismatch;
    }


    int score = 10000; //这个score是做什么用的？？？ 适配工作

    if (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_TOOL_TYPE)) { // evdev 是输入设备的结构体指针
        int tool_min = libevdev_get_abs_minimum(evdev, ABS_MT_TOOL_TYPE);
        int tool_max = libevdev_get_abs_maximum(evdev, ABS_MT_TOOL_TYPE);

        if (tool_min > MT_TOOL_FINGER || tool_max < MT_TOOL_FINGER) {  //判断支持的触控点数
            fprintf(stderr, "Note: device %s is a touch device, but doesn't"
                            " support fingers\n", devpath);
            goto mismatch;
        }

        score -= tool_max - MT_TOOL_FINGER;
    }

    if (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_SLOT)) {
        score += 1000;

        // Some devices, e.g. Blackberry PRIV (STV100) have more than one surface
        // you can touch. On the PRIV, the keypad also acts as a touch screen
        // that you can swipe and scroll with. The only differences between the
        // touch devices are that one is named "touch_display" and the other
        // "touch_keypad", the keypad only supports 3 contacts and the display
        // up to 9, and the keypad has a much lower resolution. Therefore
        // increasing the score by the number of contacts should be a relatively
        // safe bet, though we may also want to decrease the score by, say, 1,
        // if the device name contains "key" just in case they decide to start
        // supporting more contacts on both touch surfaces in the future.
        int num_slots = libevdev_get_abs_maximum(evdev, ABS_MT_SLOT);
        score += num_slots;
    }

    // For Blackberry devices, see above.
    // Also some device like SO-03L it has two touch devices, one is for touch
    // one is for side sense which name is 'sec_touchscreen_side'.
    // So add one more check for '_side'. check issue #45 for more info
    const char *name = libevdev_get_name(evdev); //获取设备名称
    if (strstr(name, "key") != NULL ||
        strstr(name, "_side") != NULL) { //判断 name 中是否包含"key"，或者"_slide"
        score -= 1;
    }

    // Alcatel OneTouch Idol 3 has an `input_mt_wrapper` device in addition
    // to direct input. It seems to be related to accessibility, as it shows
    // a touchpoint that you can move around, and then tap to activate whatever
    // is under the point. That wrapper device lacks the direct property.
    if (libevdev_has_property(evdev, INPUT_PROP_DIRECT)) {  //判断是否包含 INPUT_PROP_DIRECT 属性
        score += 10000;
    }

    // Some devices may have an additional screen. For example, Meizu Pro7 Plus
    // has a small screen on the back side of the device called sub_touch, while
    // the boring screen in the front is called main_touch. The resolution on
    // the sub_touch device is much much lower. It seems like a safe bet
    // to always prefer the larger device, as long as the score adjustment is
    // likely to be lower than the adjustment we do for INPUT_PROP_DIRECT.
    if (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_X)) {
        int x = libevdev_get_abs_maximum(evdev, ABS_MT_POSITION_X);
        int y = libevdev_get_abs_maximum(evdev, ABS_MT_POSITION_Y);
        score += sqrt(x * y);
    }

    if (state->evdev != NULL) {
        if (state->score >= score) {
            fprintf(stderr, "Note: device %s was outscored by %s (%d >= %d)\n",
                    devpath, state->path, state->score, score);
            goto mismatch; //如果计算出值score小于state->score，则说明不匹配（其实是一些特殊设备的匹配问题）
        } else {
            fprintf(stderr, "Note: device %s was outscored by %s (%d >= %d)\n",
                    state->path, devpath, score, state->score);
        }
    }

    libevdev_free(state->evdev); //释放资源

    state->fd = fd;
    state->score = score;
    strncpy(state->path, devpath, sizeof(state->path)); //将devpath拷贝到字符数组 state->path 中
    state->evdev = evdev;

    return 1;

    mismatch: //不是需要的设备
    libevdev_free(evdev);  //释放资源

    if (fd >= 0) {
        close(fd); //关闭文件流
    }

    return 0; //返回 0
}

static int
walk_devices(const char *path, internal_state_touchpad_t *state,
             internal_state_keyboard_t *keyboard_state) { // 对 /dev/input 目录进行遍历，判断是否是可用设备
    DIR *dir;
    struct dirent *ent; //目录
    char touchpad_path[FILENAME_MAX]; //触控设备的设备节点
    char keyboard_path[FILENAME_MAX];  //键盘设备的设备节点

    if ((dir = opendir(path)) == NULL) {
        perror("opendir");
        return -1;
    }

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        snprintf(touchpad_path, FILENAME_MAX, "%s/%s", path, ent->d_name);

        snprintf(keyboard_path, FILENAME_MAX, "%s/%s", path, ent->d_name);

        consider_touch_device(touchpad_path, state);

        consider_keyboard_device(keyboard_path, keyboard_state);

    }

    closedir(dir);

    return 0;
}

#define WRITE_EVENT(state, type, code, value) _write_event(state, type, #type, code, #code, value)

static int _write_event(internal_state_touchpad_t *state,
                        uint16_t type, const char *type_name,
                        uint16_t code, const char *code_name,
                        int32_t value) {
    // It seems that most devices do not require the event timestamps at all.
    // Left here for reference should such a situation arise.
    //
    //   timespec ts;
    //   clock_gettime(CLOCK_MONOTONIC, &ts);
    //   input_event event = {{ts.tv_sec, ts.tv_nsec / 1000}, type, code, value};

    struct input_event event = {{0, 0}, type, code, value}; //对 input_event 进行赋值
    ssize_t result;
    ssize_t length = (ssize_t) sizeof(event);

    if (g_verbose)
        fprintf(stderr, "%-12s %-20s %08x\n", type_name, code_name, value); //输出日志

    result = write(state->fd, &event, length); //写入事件
    return result - length;
}

static int next_tracking_id(internal_state_touchpad_t *state) {
    if (state->tracking_id < INT_MAX) {
        state->tracking_id += 1;
    } else {
        state->tracking_id = 0;
    }

    return state->tracking_id;
}

static int type_a_commit(internal_state_touchpad_t *state) {
    int contact;
    int found_any = 0;

    for (contact = 0; contact < state->max_contacts; ++contact) {
        switch (state->contacts[contact].enabled) { //判断 enabled不为0
            case 1: // WENT_DOWN
                found_any = 1;

                state->active_contacts += 1;

                if (state->has_tracking_id)
                    WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID, contact);

                // Send BTN_TOUCH on first contact only.
                if (state->active_contacts == 1 && state->has_key_btn_touch)
                    WRITE_EVENT(state, EV_KEY, BTN_TOUCH, 1);

                if (state->has_touch_major)
                    WRITE_EVENT(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

                if (state->has_width_major)
                    WRITE_EVENT(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

                if (state->has_pressure)
                    WRITE_EVENT(state, EV_ABS, ABS_MT_PRESSURE, state->contacts[contact].pressure);

                WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_X, state->contacts[contact].x);
                WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_Y, state->contacts[contact].y);

                WRITE_EVENT(state, EV_SYN, SYN_MT_REPORT, 0);

                state->contacts[contact].enabled = 2;
                break;
            case 2: // MOVED
                found_any = 1;

                if (state->has_tracking_id)
                    WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID, contact);

                if (state->has_touch_major)
                    WRITE_EVENT(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

                if (state->has_width_major)
                    WRITE_EVENT(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

                if (state->has_pressure)
                    WRITE_EVENT(state, EV_ABS, ABS_MT_PRESSURE, state->contacts[contact].pressure);

                WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_X, state->contacts[contact].x);
                WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_Y, state->contacts[contact].y);

                WRITE_EVENT(state, EV_SYN, SYN_MT_REPORT, 0);
                break;
            case 3: // WENT_UP
                found_any = 1;

                state->active_contacts -= 1;

                if (state->has_tracking_id)
                    WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID, contact);

                // Send BTN_TOUCH only when no contacts remain.
                if (state->active_contacts == 0 && state->has_key_btn_touch)
                    WRITE_EVENT(state, EV_KEY, BTN_TOUCH, 0);

                WRITE_EVENT(state, EV_SYN, SYN_MT_REPORT, 0);

                state->contacts[contact].enabled = 0;
                break;
        }
    }

    if (found_any)
        WRITE_EVENT(state, EV_SYN, SYN_REPORT, 0);

    return 1;
}

static int type_a_touch_panic_reset_all(internal_state_touchpad_t *state) {
    int contact;

    for (contact = 0; contact < state->max_contacts; ++contact) {
        switch (state->contacts[contact].enabled) {
            case 1: // WENT_DOWN
            case 2: // MOVED
                // Force everything to WENT_UP
                state->contacts[contact].enabled = 3;
                break;
        }
    }

    return type_a_commit(state);
}

static int
type_a_touch_down(internal_state_touchpad_t *state, int contact, int x, int y, int pressure) {
    if (contact >= state->max_contacts) {
        return 0;
    }

    if (state->contacts[contact].enabled) {
        type_a_touch_panic_reset_all(state);
    }

    state->contacts[contact].enabled = 1;
    state->contacts[contact].x = x;
    state->contacts[contact].y = y;
    state->contacts[contact].pressure = pressure;

    return 1;
}

static int
type_a_touch_move(internal_state_touchpad_t *state, int contact, int x, int y, int pressure) {
    if (contact >= state->max_contacts || !state->contacts[contact].enabled) {
        return 0;
    }

    state->contacts[contact].enabled = 2;
    state->contacts[contact].x = x;
    state->contacts[contact].y = y;
    state->contacts[contact].pressure = pressure;

    return 1;
}

static int type_a_touch_up(internal_state_touchpad_t *state, int contact) {
    if (contact >= state->max_contacts || !state->contacts[contact].enabled) {
        return 0;
    }

    state->contacts[contact].enabled = 3;

    return 1;
}

static int type_b_commit(internal_state_touchpad_t *state) {
    WRITE_EVENT(state, EV_SYN, SYN_REPORT, 0);

    return 1;
}

static int type_b_touch_panic_reset_all(internal_state_touchpad_t *state) {
    int contact;
    int found_any = 0;

    for (contact = 0; contact < state->max_contacts; ++contact) {
        if (state->contacts[contact].enabled) {
            state->contacts[contact].enabled = 0;
            found_any = 1;
        }
    }

    return found_any ? type_b_commit(state) : 1;
}

static int
type_b_touch_down(internal_state_touchpad_t *state, int contact, int x, int y, int pressure) {
    if (contact >= state->max_contacts) {
        return 0;
    }

    if (state->contacts[contact].enabled) {
        type_b_touch_panic_reset_all(state);
    }

    state->contacts[contact].enabled = 1;
    state->contacts[contact].tracking_id = next_tracking_id(state);
    state->active_contacts += 1;

    WRITE_EVENT(state, EV_ABS, ABS_MT_SLOT, contact);
    WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID,
                state->contacts[contact].tracking_id);

    // Send BTN_TOUCH on first contact only.
    if (state->active_contacts == 1 && state->has_key_btn_touch)
        WRITE_EVENT(state, EV_KEY, BTN_TOUCH, 1);

    if (state->has_touch_major)
        WRITE_EVENT(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

    if (state->has_width_major)
        WRITE_EVENT(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

    if (state->has_pressure)
        WRITE_EVENT(state, EV_ABS, ABS_MT_PRESSURE, pressure);

    WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_X, x);
    WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_Y, y);

    return 1;
}

static int
type_b_touch_move(internal_state_touchpad_t *state, int contact, int x, int y, int pressure) {
    if (contact >= state->max_contacts || !state->contacts[contact].enabled) {
        return 0;
    }

    WRITE_EVENT(state, EV_ABS, ABS_MT_SLOT, contact);

    if (state->has_touch_major)
        WRITE_EVENT(state, EV_ABS, ABS_MT_TOUCH_MAJOR, 0x00000006);

    if (state->has_width_major)
        WRITE_EVENT(state, EV_ABS, ABS_MT_WIDTH_MAJOR, 0x00000004);

    if (state->has_pressure)
        WRITE_EVENT(state, EV_ABS, ABS_MT_PRESSURE, pressure);

    WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_X, x);
    WRITE_EVENT(state, EV_ABS, ABS_MT_POSITION_Y, y);

    return 1;
}

static int type_b_touch_up(internal_state_touchpad_t *state, int contact) {
    if (contact >= state->max_contacts || !state->contacts[contact].enabled) {
        return 0;
    }

    state->contacts[contact].enabled = 0;
    state->contacts[contact].enabled = 0;
    state->active_contacts -= 1;

    WRITE_EVENT(state, EV_ABS, ABS_MT_SLOT, contact);
    WRITE_EVENT(state, EV_ABS, ABS_MT_TRACKING_ID, -1);

    // Send BTN_TOUCH only when no contacts remain.
    if (state->active_contacts == 0 && state->has_key_btn_touch)
        WRITE_EVENT(state, EV_KEY, BTN_TOUCH, 0);

    return 1;
}

static int touch_down(internal_state_touchpad_t *state, int contact, int x, int y, int pressure) {
    if (state->has_mtslot) {
        return type_b_touch_down(state, contact, x, y, pressure);
    } else {
        return type_a_touch_down(state, contact, x, y, pressure);
    }
}

static int touch_move(internal_state_touchpad_t *state, int contact, int x, int y, int pressure) {
    if (state->has_mtslot) {
        return type_b_touch_move(state, contact, x, y, pressure);
    } else {
        return type_a_touch_move(state, contact, x, y, pressure);
    }
}

static int touch_up(internal_state_touchpad_t *state, int contact) {
    if (state->has_mtslot) {
        return type_b_touch_up(state, contact);
    } else {
        return type_a_touch_up(state, contact);
    }
}

static int touch_panic_reset_all(internal_state_touchpad_t *state) {
    if (state->has_mtslot) {
        return type_b_touch_panic_reset_all(state);
    } else {
        return type_a_touch_panic_reset_all(state);
    }
}

static int commit(internal_state_touchpad_t *state) {
    if (state->has_mtslot) {
        return type_b_commit(state);
    } else {
        return type_a_commit(state);
    }
}

static int start_server(char *sockname) {
    int fd = socket(AF_UNIX, SOCK_STREAM, //https://blog.csdn.net/sandware/article/details/40923491
                    0); // AF_UNIX, 典型的本地IPC，类似于管道，依赖路径名标识发送方和接收方,只能用于本机内进程之间的通信

    if (fd < 0) {
        perror("creating socket");
        return fd;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    // 此处的sockname 为 #define DEFAULT_SOCKET_NAME "minitouch"
    strncpy(&addr.sun_path[1], sockname,
            strlen(sockname)); // sun_path为标识本地连接的路径名，也就是socketname的值：minitouch


    int bindResult = bind(fd, (struct sockaddr *) &addr,
                          sizeof(sa_family_t) + strlen(sockname) + 1);// 此处minitouch作为服务端应用，调用bind函数

    if (bindResult < 0) {
        perror("binding socket");
        close(fd);
        return -1;
    }

    listen(fd, 1); //监听socket端口

    return fd;
}

static void parse_input(char *buffer, internal_state_touchpad_t *state) {
    char *cursor;
    long int contact, x, y, pressure, wait;

    cursor = (char *) buffer; //对指针进行遍历
    cursor += 1;

    //Linux内核多点触控协议 https://www.kernel.org/doc/Documentation/input/multi-touch-protocol.txt
    switch (buffer[0]) { //取缓冲行的第一个字符，进行分支判断
        case 'c': // COMMIT
            commit(state);
            break;
        case 'r': // RESET
            touch_panic_reset_all(state);
            break;
        case 'd': // TOUCH DOWN
            contact = strtol(cursor, &cursor, 10); //strtol : string to long
            x = strtol(cursor, &cursor, 10);
            y = strtol(cursor, &cursor, 10);
            pressure = strtol(cursor, &cursor, 10);
            touch_down(state, contact, x, y, pressure);
            break;
        case 'm': // TOUCH MOVE
            contact = strtol(cursor, &cursor, 10);
            x = strtol(cursor, &cursor, 10);
            y = strtol(cursor, &cursor, 10);
            pressure = strtol(cursor, &cursor, 10);
            touch_move(state, contact, x, y, pressure);
            break;
        case 'u': // TOUCH UP
            contact = strtol(cursor, &cursor, 10);
            touch_up(state, contact);
            break;
        case 'w':
            wait = strtol(cursor, &cursor, 10);
            if (g_verbose)
                fprintf(stderr, "Waiting %ld ms\n", wait);
            usleep(wait * 1000);
            break;
        default:
            break;
    }
}

static void io_handler(FILE *input, FILE *output, internal_state_touchpad_t *state) {
    // setvbuf函数
    // 第一个参数为流
    // 第二个参数为NULL，分配一个指定大小的缓冲
    // 第三个参数_IOLBF表示行缓冲，第四个参数指定缓冲的大小
    setvbuf(input, NULL, _IOLBF, 1024);
    setvbuf(output, NULL, _IOLBF, 1024);

    // Tell version
    fprintf(output, "v %d\n", VERSION);

    // Tell limits
    fprintf(output, "^ %d %d %d %d\n",
            state->max_contacts, state->max_x, state->max_y, state->max_pressure);

    // Tell pid
    fprintf(output, "$ %d\n", getpid());

    char read_buffer[80]; //用于读取input数据的缓冲字符数组

    while (fgets(read_buffer, sizeof(read_buffer), input) != NULL) { //fgets函数功能为从指定的流中读取数据，每次读取一行
        read_buffer[strcspn(read_buffer, "\r\n")] = 0; //按行读取缓冲数据
        parse_input(read_buffer, state); //解析缓冲数据
    }
}

static void
print_event(struct input_event *ev,internal_state_touchpad_t *stateTouchpad) {
    if (ev->type == EV_SYN){
        commit(stateTouchpad);
        fprintf(stderr, "Event: time %ld.%06ld, ++++++++++++++++++++ %s +++++++++++++++\n",
                ev->time.tv_sec,
                ev->time.tv_usec,
                libevdev_event_type_get_name(ev->type));
    }else{
        mappingKeyboardEvent(ev,stateTouchpad);
        fprintf(stderr, "Event: time %ld.%06ld, type %d (%s), code %d (%s), value %d\n",
                ev->time.tv_sec,
                ev->time.tv_usec,
                ev->type,
                libevdev_event_type_get_name(ev->type),
                ev->code,
                libevdev_event_code_get_name(ev->type, ev->code),
                ev->value);
    }
}

static void
print_sync_event(struct input_event *ev,internal_state_touchpad_t *stateKeyboard) {
    printf("SYNC: ");
    print_event(ev,stateKeyboard);
}

/**
 * 监听键盘输入信息
 * @param rc
 * @param state_keyboard
 * @return
 */
static void
listen_keyboard_input(internal_state_warper warper) {
    internal_state_keyboard_t state_keyboard = warper.keyboard;
    internal_state_touchpad_t state_touchpad = warper.touchpad;

    int id = pthread_self();
    printf("Thread ID: %x\n", id);
    int rc = 1;
    do {
        struct input_event ev;
        rc = libevdev_next_event(state_keyboard.evdev,
                                 LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING, &ev);
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            printf("::::::::::::::::::::: dropped ::::::::::::::::::::::\n");
            while (rc == LIBEVDEV_READ_STATUS_SYNC) {
                if (g_verbose) {
                    print_sync_event(&ev,&state_touchpad);
                }
                rc = libevdev_next_event(state_keyboard.evdev, LIBEVDEV_READ_FLAG_SYNC, &ev);
            }
            printf("::::::::::::::::::::: re-synced ::::::::::::::::::::::\n");
        } else if (rc == LIBEVDEV_READ_STATUS_SUCCESS)
            print_event(&ev,&state_touchpad);
    } while (rc == LIBEVDEV_READ_STATUS_SYNC || rc == LIBEVDEV_READ_STATUS_SUCCESS ||
             rc == -EAGAIN);
}

static void mappingKeyboardEvent(struct input_event *pEvent, internal_state_touchpad_t *stateTouchpad) {
    //当触发指定按键时，发送相应的多点触控指令，可以做一个WASD画圆的sample
    fprintf(stderr,"mappingKeyboardEvent...");
    int action = pEvent->value;
    fprintf(stderr,"action:%d",action);
    switch (action) {
        case 0: //action_up
            dealWithActionUp(pEvent, stateTouchpad);
            break;
        case 1://action_down
            dealWithActionDown(pEvent, stateTouchpad);
            break;
    }
}


static void dealWithActionUp(struct input_event *pEvent, internal_state_touchpad_t *touchpad) {
    // 定义AS的映射点（可以做成可配置的选项）
    int key_code = pEvent->code;
    switch (key_code) {
        case KEY_A:
            // 触发A对应点的按下事件
            touch_up(touchpad, 0);
            break;
        case KEY_D:
            //触发B对应点的按下事件
            touch_up(touchpad, 1);
            break;
    }
}


/**
 * 处理按下事件
 * @param pEvent
 * @param touchpad
 */
static void dealWithActionDown(struct input_event *pEvent, internal_state_touchpad_t *touchpad) {
    // 定义AS的映射点（可以做成可配置的选项）
    int pointer_A[2] = {230, 491};
    int pointer_S[2] = {230, 732};
    int key_code = pEvent->code;
    switch (key_code) {
        case KEY_A:
            // 触发A对应点的按下事件
            touch_down(touchpad, 0, pointer_A[0], pointer_A[1], 10);
            break;
        case KEY_D:
            // 触发D对应点的按下事件
            touch_down(touchpad, 1, pointer_S[0], pointer_S[1], 10);
            break;
    }
}



char* strJoin(char *s1, char *s2)
{
    char *result = malloc(strlen(s1)+strlen(s2)+1);//+1 for the zero-terminator
    if(result==NULL){
        return "";
    }
    strcpy(result, s1);
    strcat(result, s2);
    return result;
}



static void
on_device_added(internal_state_warper warper, struct inotify_event *pEvent, char *val) {
    // 有新设备添加时，应该判断event是否为键盘设备
    char *name = pEvent->name;
    char *root_path = "dev/input/";
    char *dev_path = strJoin(root_path,name);
    fprintf(stderr,"on device added:%s,",dev_path);
    consider_keyboard_device(dev_path,&warper.keyboard);
    if(warper.keyboard.evdev){ //找到设备
        fprintf(stderr,"and it's keyboard device\n");
        pthread_t keyboardThread;
        pthread_create(&keyboardThread, NULL, (void *) &listen_keyboard_input, (void *) &warper);
    } else{ //未找到设备
        fprintf(stderr,"but it's not a keyboard kevice\n");
    }
}

static void on_device_removed(struct inotify_event *pEvent, char *val) {
    //有设备移除

}



/**
 * 使用inotify方式对文件变化进行监听
 */
static void watch_inotify(internal_state_warper warper) {
    int fd;
    int wd;
    int len;
    int nread;
    char buf[BUFSIZ];
    struct inotify_event *event;
    int i;

    fd = inotify_init();
    if (fd < 0) {
        fprintf(stderr, "inotify_init failed\n");
    }

    wd = inotify_add_watch(fd, "dev/input", (IN_CREATE | IN_DELETE)); //对 dev/input 这个目录进行监听，监听两个事件：IN_CREATE和IN_DELETE
    if (wd < 0) {
        fprintf(stderr, "inotify_add_watch %s failed\n", "dev/input");
    }

    fprintf(stderr,">>> watching device state change...\n");

    buf[sizeof(buf) - 1] = 0;
    while ((len = read(fd, buf, sizeof(buf) - 1)) > 0) {
        nread = 0;
        while (len > 0) {
            event = (struct inotify_event *) &buf[nread];
            for (i = 0; i < EVENT_NUM; i++) {
                if ((event->mask >> i) & 1) {
                    if (event->len > 0) {

                        char *eventVal = event_str[i];
                        fprintf(stdout, "%s --- %s\n", event->name, eventVal);
                        if (strcmp(eventVal,"IN_CREATE") == 0) {
                            fprintf(stdout, "on device added: %s --- %s\n", event->name, eventVal);
                            on_device_added(warper,event,eventVal);//有设备添加
                        }else if (strcmp(eventVal, "IN_DELETE")==0) {
                            fprintf(stdout, "on device removed: %s --- %s\n", event->name, eventVal);
                            on_device_removed(event,eventVal);//有设备移除
                        }
                    } else {
                        fprintf(stdout, "%s --- %s\n", " ", event_str[i]);
                    }
                }
            }
            nread = nread + sizeof(struct inotify_event) + event->len;
            len = len - sizeof(struct inotify_event) - event->len;
        }
    }
}


/**
 * Warning: shell下无权限对该socket进行bind操作
 * 使用netLink对键盘的插入/拔出进行监听
 */
static void monitorNetlinkUevent() {
    int sockfd;
    struct sockaddr_nl sa;
    int len;
    char buf[4096];
    struct iovec iov;
    struct msghdr msg;
    int i;

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = NETLINK_KOBJECT_UEVENT;
    sa.nl_pid = 0;//getpid(); both is ok
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void *) buf;
    iov.iov_len = sizeof(buf);
    msg.msg_name = (void *) &sa;
    msg.msg_namelen = sizeof(sa);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    sockfd = socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
    if (sockfd == -1)
        printf("socket creating failed:%s\n", strerror(errno));
    if (bind(sockfd, (struct sockaddr *) &sa, sizeof(sa)) == -1)
        printf("bind error:%s\n", strerror(errno));

    len = recvmsg(sockfd, &msg, 0);
    if (len < 0)
        printf("receive error\n");
    else if (len < 32 || len > sizeof(buf))
        printf("invalid message");
    for (i = 0; i < len; i++)
        if (*(buf + i) == '\0')
            buf[i] = '\n';
    fprintf(stderr, "received %d bytes\n%s\n", len, buf);
}

int main(int argc, char *argv[]) { //入口函数
    const char *pname = argv[0];
    const char *devroot = "/dev/input"; //设备的输入事件目录
    char *device = NULL;
    char *sockname = DEFAULT_SOCKET_NAME; //宏定义
    char *stdin_file = NULL;
    int use_stdin = 0;

    int opt;
    while ((opt = getopt(argc, argv, "d:n:vif:h")) != -1) { // 命令行参数
        switch (opt) {
            case 'd':
                device = optarg;
                break;
            case 'n':
                sockname = optarg;
                break;
            case 'v':
                g_verbose = 1;
                break;
            case 'i':
                use_stdin = 1;
                break;
            case 'f':
                stdin_file = optarg;
                break;
            case '?':
                usage(pname);
                return EXIT_FAILURE;
            case 'h':
                usage(pname);
                return EXIT_SUCCESS;
        }
    }

    internal_state_touchpad_t state_touchpad = {0}; // 对触摸设备的结构体初始化

    internal_state_keyboard_t state_keyboard = {0}; // 对键盘设备的结构体初始化

    //鼠标 如何判断是否是鼠标设备 x 3 y 3  结合业务需求  CS 转动视角  move（1920 1080）游戏  （0,0） move


    //手柄 如何判断是否是手柄设备


    //...


    internal_state_warper state_waper = {0, 0};  //包装结构体


    //程序第一次运行时，检测是否有可触控设备及键盘设备
    if (device != NULL) { //指定设备
        if (!consider_touch_device(device, &state_touchpad)) {  //判断触控设备是否可用
            fprintf(stderr, "%s is not a supported touch device\n", device);
            return EXIT_FAILURE;
        }
    } else { //非指定设备 //dev/input/eventX
        if (walk_devices(devroot, &state_touchpad, &state_keyboard) != 0) {
            fprintf(stderr, "Unable to crawl %s for touch devices\n", devroot);
            return EXIT_FAILURE; //退出程序
        }
    }

    if (state_touchpad.evdev == NULL) {
        fprintf(stderr, "Unable to find a suitable touch device\n");
        return EXIT_FAILURE;
    }

    state_touchpad.has_mtslot =
            libevdev_has_event_code(state_touchpad.evdev, EV_ABS, ABS_MT_SLOT);
    state_touchpad.has_tracking_id =
            libevdev_has_event_code(state_touchpad.evdev, EV_ABS, ABS_MT_TRACKING_ID);
    state_touchpad.has_key_btn_touch =
            libevdev_has_event_code(state_touchpad.evdev, EV_KEY, BTN_TOUCH);
    state_touchpad.has_touch_major =
            libevdev_has_event_code(state_touchpad.evdev, EV_ABS, ABS_MT_TOUCH_MAJOR);
    state_touchpad.has_width_major =
            libevdev_has_event_code(state_touchpad.evdev, EV_ABS, ABS_MT_WIDTH_MAJOR);

    state_touchpad.has_pressure =
            libevdev_has_event_code(state_touchpad.evdev, EV_ABS, ABS_MT_PRESSURE);
    state_touchpad.min_pressure = state_touchpad.has_pressure ?
                                  libevdev_get_abs_minimum(state_touchpad.evdev, ABS_MT_PRESSURE)
                                                              : 0;
    state_touchpad.max_pressure = state_touchpad.has_pressure ?
                                  libevdev_get_abs_maximum(state_touchpad.evdev, ABS_MT_PRESSURE)
                                                              : 0;

    state_touchpad.max_x = libevdev_get_abs_maximum(state_touchpad.evdev, ABS_MT_POSITION_X);
    state_touchpad.max_y = libevdev_get_abs_maximum(state_touchpad.evdev, ABS_MT_POSITION_Y);

    state_touchpad.max_tracking_id = state_touchpad.has_tracking_id
                                     ? libevdev_get_abs_maximum(state_touchpad.evdev,
                                                                ABS_MT_TRACKING_ID)
                                     : INT_MAX;

    if (!state_touchpad.has_mtslot && state_touchpad.max_tracking_id == 0) {
        // The touch device reports incorrect values. There would be no point
        // in supporting ABS_MT_TRACKING_ID at all if the maximum value was 0
        // (i.e. one contact). This happens on Lenovo Yoga Tablet B6000-F,
        // which actually seems to support ~10 contacts. So, we'll just go with
        // as many as we can and hope that the system will ignore extra contacts.
        state_touchpad.max_tracking_id = MAX_SUPPORTED_CONTACTS - 1;
        fprintf(stderr,
                "Note: type A device reports a max value of 0 for ABS_MT_TRACKING_ID. "
                "This means that the device is most likely reporting incorrect "
                "information. Guessing %d.\n",
                state_touchpad.max_tracking_id
        );
    }

    state_touchpad.max_contacts = state_touchpad.has_mtslot
                                  ? libevdev_get_abs_maximum(state_touchpad.evdev, ABS_MT_SLOT) + 1
                                  : (state_touchpad.has_tracking_id ?
                                     state_touchpad.max_tracking_id + 1 : 2);

    state_touchpad.tracking_id = 0;

    int contact;  //触控点数量
    for (contact = 0; contact < MAX_SUPPORTED_CONTACTS; ++contact) {
        state_touchpad.contacts[contact].enabled = 0;
    }

    fprintf(stderr,
            "%s touch device %s (%dx%d with %d contacts) detected on %s (score %d)\n",
            state_touchpad.has_mtslot ? "Type B" : "Type A", //根据触控槽来判断是什么协议类型
            libevdev_get_name(state_touchpad.evdev),
            state_touchpad.max_x, state_touchpad.max_y, state_touchpad.max_contacts,
            state_touchpad.path, state_touchpad.score
    );

    if (state_touchpad.max_contacts > MAX_SUPPORTED_CONTACTS) {
        fprintf(stderr, "Note: hard-limiting maximum number of contacts to %d\n",
                MAX_SUPPORTED_CONTACTS);
        state_touchpad.max_contacts = MAX_SUPPORTED_CONTACTS;
    }

    FILE *input;
    FILE *output;

    if (use_stdin || stdin_file != NULL) {
        if (stdin_file != NULL) {
            // Reading from a file
            input = fopen(stdin_file, "r");
            if (NULL == input) {
                fprintf(stderr, "Unable to open '%s': %s\n",
                        stdin_file, strerror(errno));
                exit(EXIT_FAILURE);
            } else {
                fprintf(stderr, "Reading commands from '%s'\n",
                        stdin_file);
            }
        } else {
            // Reading from terminal
            input = stdin;
            fprintf(stderr, "Reading from STDIN\n");
        }

        output = stderr;
        io_handler(input, output, &state_touchpad);
        fclose(input);
        fclose(output);
        exit(EXIT_SUCCESS);
    }

    struct sockaddr_un client_addr;
    socklen_t client_addr_length = sizeof(client_addr);

    int server_fd = start_server(sockname); // 开启服务端socket

    if (server_fd < 0) {
        fprintf(stderr, "Unable to start server on %s\n", sockname);
        return EXIT_FAILURE;
    }

    state_waper.keyboard = state_keyboard;
    state_waper.touchpad = state_touchpad;

    if(state_keyboard.evdev!=0){
        pthread_t keyboardThread;

        pthread_create(&keyboardThread, NULL, (void *) &listen_keyboard_input,
                       (void *) &state_waper);
    } else{
        fprintf(stderr,">>> keyboard device not found\n");
    }

    pthread_t watcherThread;
    pthread_create(&watcherThread,NULL,(void *)&watch_inotify,&state_waper);


    while (1) { //监听socket客户端发送的消息
        int client_fd = accept(server_fd, (struct sockaddr *) &client_addr,
                               &client_addr_length);

        if (client_fd < 0) {
            perror("accepting client");
            exit(1);
        }

        fprintf(stderr, "Connection established\n");

        input = fdopen(client_fd, "r");
        if (input == NULL) {
            fprintf(stderr, "%s: fdopen(client_fd,'r')\n", strerror(errno));
            exit(1);
        }

        output = fdopen(dup(client_fd), "w");
        if (output == NULL) {
            fprintf(stderr, "%s: fdopen(client_fd,'w')\n", strerror(errno));
            exit(1);
        }

        io_handler(input, output, &state_touchpad);

        fprintf(stderr, "Connection closed\n");
        fclose(input);
        fclose(output);
        close(client_fd);
    }


    close(server_fd);

    libevdev_free(state_touchpad.evdev);
    close(state_touchpad.fd);

    return EXIT_SUCCESS;
}
