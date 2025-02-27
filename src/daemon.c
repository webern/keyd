#include "keyd.h"

#define VKBD_NAME "keyd virtual keyboard"

struct config_ent {
	struct config config;
	struct keyboard *kbd;
	struct config_ent *next;
};

static int ipcfd = -1;
static struct vkbd *vkbd = NULL;
static struct config_ent *configs;

static uint8_t keystate[256];

static int listeners[32];
static size_t nr_listeners = 0;

static void free_configs()
{
	struct config_ent *ent = configs;
	while (ent) {
		struct config_ent *tmp = ent;
		ent = ent->next;
		free(tmp->kbd);
		free(tmp);
	}

	configs = NULL;
}

static void cleanup()
{
	free_configs();
	free_vkbd(vkbd);
}

static void clear_vkbd()
{
	size_t i;

	for (i = 0; i < 256; i++)
		if (keystate[i]) {
			vkbd_send_key(vkbd, i, 0);
			keystate[i] = 0;
		}
}

static void send_key(uint8_t code, uint8_t state)
{
	keystate[code] = state;
	vkbd_send_key(vkbd, code, state);
}

static void add_listener(int con)
{
	struct timeval tv;

	/*
	 * In order to avoid blocking the main event loop, allow up to 50ms for
	 * slow clients to relieve back pressure before dropping them.
	 */
	tv.tv_usec = 50000;
	tv.tv_sec = 0;

	if (nr_listeners == ARRAY_SIZE(listeners)) {
		char s[] = "Max listeners exceeded\n";
		xwrite(con, &s, sizeof s);

		close(con);
		return;
	}

	setsockopt(con, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

	listeners[nr_listeners++] = con;
}

static void layer_observer(struct keyboard *kbd, const char *name, int state)
{
	size_t i;
	char buf[MAX_LAYER_NAME_LEN+2];
	ssize_t bufsz;

	int keep[ARRAY_SIZE(listeners)];
	size_t n = 0;

	if (kbd->config.layer_indicator) {
		for (i = 0; i < device_table_sz; i++)
			if (device_table[i].data == kbd)
				device_set_led(&device_table[i], 1, state);
	}

	if (!nr_listeners)
		return;

	bufsz = snprintf(buf, sizeof(buf), "%c%s\n", state ? '+' : '-', name);
	for (i = 0; i < nr_listeners; i++) {
		ssize_t nw = write(listeners[i], buf, bufsz);

		if (nw == bufsz)
			keep[n++] = listeners[i];
		else
			close(listeners[i]);
	}

	if (n != nr_listeners) {
		nr_listeners = n;
		memcpy(listeners, keep, n * sizeof(int));
	}
}

static void load_configs()
{
	DIR *dh = opendir(CONFIG_DIR);
	struct dirent *dirent;

	if (!dh) {
		perror("opendir");
		exit(-1);
	}

	configs = NULL;

	while ((dirent = readdir(dh))) {
		char path[1024];
		int len;

		if (dirent->d_type == DT_DIR)
			continue;

		len = snprintf(path, sizeof path, "%s/%s", CONFIG_DIR, dirent->d_name);

		if (len >= 5 && !strcmp(path + len - 5, ".conf")) {
			struct config_ent *ent = calloc(1, sizeof(struct config_ent));

			printf("CONFIG: parsing %s\n", path);

			if (!config_parse(&ent->config, path)) {
				ent->kbd = new_keyboard(&ent->config, send_key, layer_observer);
				ent->next = configs;
				configs = ent;
			} else {
				free(ent);
				warn("failed to parse %s", path);
			}

		}
	}

	closedir(dh);
}

static struct config_ent *lookup_config_ent(uint16_t vendor,
					    uint16_t product,
					    uint8_t flags)
{
	struct config_ent *ent = configs;
	struct config_ent *match = NULL;
	int rank = 0;

	while (ent) {
		int r = config_check_match(&ent->config, vendor, product, flags);

		if (r > rank) {
			match = ent;
			rank = r;
		}

		ent = ent->next;
	}

	/* The wildcard should not match mice. */
	if (rank == 1 && (flags == ID_MOUSE))
		return NULL;
	else
		return match;
}

static void manage_device(struct device *dev)
{
	uint8_t flags = 0;
	struct config_ent *ent;

	if (!strcmp(dev->name, VKBD_NAME))
		return;

	if (dev->capabilities & CAP_KEYBOARD)
		flags |= ID_KEYBOARD;
	if (dev->capabilities & (CAP_MOUSE|CAP_MOUSE_ABS))
		flags |= ID_MOUSE;

	if ((ent = lookup_config_ent(dev->vendor_id, dev->product_id, flags))) {
		if (device_grab(dev)) {
			warn("Failed to grab %s", dev->path);
			dev->data = NULL;
			return;
		}

		printf("DEVICE: \033[32;1mmatch   \033[0m %04hx:%04hx  %s\t(%s)\n",
		     dev->vendor_id, dev->product_id,
		     ent->config.path,
		     dev->name);

		dev->data = ent->kbd;
	} else {
		dev->data = NULL;
		device_ungrab(dev);
		printf("DEVICE: \033[31;1mignoring\033[0m %04hx:%04hx  (%s)\n",
		     dev->vendor_id, dev->product_id, dev->name);
	}
}

static void reload()
{
	size_t i;

	free_configs();
	load_configs();

	for (i = 0; i < device_table_sz; i++)
		manage_device(&device_table[i]);

	clear_vkbd();
}

static void send_success(int con)
{
	struct ipc_message msg = {0};

	msg.type = IPC_SUCCESS;;
	msg.sz = 0;

	xwrite(con, &msg, sizeof msg);
	close(con);
}

static void send_fail(int con, const char *fmt, ...)
{
	struct ipc_message msg = {0};
	va_list args;

	va_start(args, fmt);

	msg.type = IPC_FAIL;
	msg.sz = vsnprintf(msg.data, sizeof(msg.data), fmt, args);

	xwrite(con, &msg, sizeof msg);
	close(con);

	va_end(args);
}

static int input(char *buf, size_t sz, uint32_t timeout)
{
	size_t i;
	uint32_t codepoint;
	uint8_t codes[4];

	int csz;

	while ((csz = utf8_read_char(buf, &codepoint))) {
		int found = 0;
		char s[2];

		if (csz == 1) {
			uint8_t code, mods;
			s[0] = (char)codepoint;
			s[1] = 0;

			found = 1;
			if (!parse_key_sequence(s, &code, &mods)) {
				if (mods & MOD_SHIFT) {
					vkbd_send_key(vkbd, KEYD_LEFTSHIFT, 1);
					vkbd_send_key(vkbd, code, 1);
					vkbd_send_key(vkbd, code, 0);
					vkbd_send_key(vkbd, KEYD_LEFTSHIFT, 0);
				} else {
					vkbd_send_key(vkbd, code, 1);
					vkbd_send_key(vkbd, code, 0);
				}
			} else if ((char)codepoint == ' ') {
				vkbd_send_key(vkbd, KEYD_SPACE, 1);
				vkbd_send_key(vkbd, KEYD_SPACE, 0);
			} else if ((char)codepoint == '\n') {
				vkbd_send_key(vkbd, KEYD_ENTER, 1);
				vkbd_send_key(vkbd, KEYD_ENTER, 0);
			} else if ((char)codepoint == '\t') {
				vkbd_send_key(vkbd, KEYD_TAB, 1);
				vkbd_send_key(vkbd, KEYD_TAB, 0);
			} else {
				found = 0;
			}
		}

		if (!found) {
			int idx = unicode_lookup_index(codepoint);
			if (idx < 0) {
				err("ERROR: could not find code for \"%.*s\"", csz, buf);
				return -1;
			}

			unicode_get_sequence(idx, codes);

			for (i = 0; i < 4; i++) {
				vkbd_send_key(vkbd, codes[i], 1);
				vkbd_send_key(vkbd, codes[i], 0);
			}
		}
		buf+=csz;

		if (timeout)
			usleep(timeout);
	}

	return 0;
}

static void handle_client(int con)
{
	struct ipc_message msg;

	xread(con, &msg, sizeof msg);

	if (msg.sz == sizeof(msg.data)) {
		send_fail(con, "maximum message size exceeded");
		return;
	}
	msg.data[msg.sz] = 0;

	if (msg.timeout > 1000000) {
		send_fail(con, "timeout cannot exceed 1000 ms");
		return;
	}

	switch (msg.type) {
		struct config_ent *ent;
		int success;
		struct macro macro;

	case IPC_MACRO:
		while (msg.sz && msg.data[msg.sz-1] == '\n')
			msg.data[--msg.sz] = 0;

		if (macro_parse(msg.data, &macro)) {
			send_fail(con, "%s", errstr);
			return;
		}

		macro_execute(send_key, &macro, msg.timeout);
		send_success(con);

		break;
	case IPC_INPUT:
		if (input(msg.data, msg.sz, msg.timeout))
			send_fail(con, "%s", errstr);
		else
			send_success(con);
		break;
	case IPC_RELOAD:
		reload();
		send_success(con);
		break;
	case IPC_LAYER_LISTEN:
		add_listener(con);
		break;
	case IPC_BIND:
		success = 0;

		if (msg.sz == sizeof(msg.data)) {
			send_fail(con, "bind expression size exceeded");
			return;
		}

		msg.data[msg.sz] = 0;

		for (ent = configs; ent; ent = ent->next) {
			if (!kbd_eval(ent->kbd, msg.data))
				success = 1;
		}

		if (success)
			send_success(con);
		else
			send_fail(con, "%s", errstr);


		break;
	default:
		send_fail(con, "Unknown command");
		break;
	}
}

static int event_handler(struct event *ev)
{
	static int last_time = 0;
	static int timeout = 0;
	static struct keyboard *timeout_kbd = NULL;
	struct key_event kev = {0};

	timeout -= ev->timestamp - last_time;
	last_time = ev->timestamp;

	timeout = timeout < 0 ? 0 : timeout;

	switch (ev->type) {
	case EV_TIMEOUT:
		if (!timeout_kbd)
			return 0;

		kev.code = 0;
		kev.timestamp = ev->timestamp;

		timeout = kbd_process_events(timeout_kbd, &kev, 1);
		break;
	case EV_DEV_EVENT:
		if (ev->dev->data) {
			timeout_kbd = ev->dev->data;
			switch (ev->devev->type) {
			case DEV_KEY:
				dbg("input %s %s", KEY_NAME(ev->devev->code), ev->devev->pressed ? "down" : "up");

				kev.code = ev->devev->code;
				kev.pressed = ev->devev->pressed;
				kev.timestamp = ev->timestamp;

				timeout = kbd_process_events(ev->dev->data, &kev, 1);
				break;
			case DEV_MOUSE_MOVE:
				vkbd_mouse_move(vkbd, ev->devev->x, ev->devev->y);
				break;
			case DEV_MOUSE_MOVE_ABS:
				vkbd_mouse_move_abs(vkbd, ev->devev->x, ev->devev->y);
				break;
			default:
				break;
			case DEV_MOUSE_SCROLL:
				/*
				 * Treat scroll events as mouse buttons so oneshot and the like get
				 * cleared.
				 */
				if (timeout_kbd) {
					kev.code = KEYD_EXTERNAL_MOUSE_BUTTON;
					kev.pressed = 1;
					kev.timestamp = ev->timestamp;

					kbd_process_events(ev->dev->data, &kev, 1);

					kev.pressed = 0;
					timeout = kbd_process_events(ev->dev->data, &kev, 1);
				}

				vkbd_mouse_scroll(vkbd, ev->devev->x, ev->devev->y);
				break;
			}
		}

		break;
	case EV_DEV_ADD:
		manage_device(ev->dev);
		break;
	case EV_DEV_REMOVE:
		printf("DEVICE: \033[31;1mremoved\033[0m\t%04hx:%04hx %s\n",
			ev->dev->vendor_id,
			ev->dev->product_id,
			ev->dev->name);
		break;
	case EV_FD_ACTIVITY:
		if (ev->fd == ipcfd) {
			int con = accept(ipcfd, NULL, 0);
			if (con < 0) {
				perror("accept");
				exit(-1);
			}

			handle_client(con);
		}
		break;
	default:
		break;
	}

	return timeout;
}

int run_daemon(int argc, char *argv[])
{
	ipcfd = ipc_create_server(SOCKET_PATH);
	if (ipcfd < 0)
		die("failed to create %s (another instance already running?)", SOCKET_PATH);

	vkbd = vkbd_init(VKBD_NAME);

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);

	if (nice(-20) == -1) {
		perror("nice");
		exit(-1);
	}

	evloop_add_fd(ipcfd);

	reload();

	atexit(cleanup);

	printf("Starting keyd "VERSION"\n");
	evloop(event_handler);

	return 0;
}
