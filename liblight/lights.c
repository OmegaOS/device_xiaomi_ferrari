/*
 * Copyright (C) 2014-2015 The CyanogenMod Project
 * Copyright (c) 2017 thewisenerd <thewisenerd@protonmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_NDEBUG 0
#define LOG_TAG "lights"

#include <cutils/log.h>

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <hardware/lights.h>

/******************************************************************************/

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct light_state_t g_notification;
static struct light_state_t g_attention;
static struct light_state_t g_battery;

const char* const BACKLIGHT_FILE
	= "/sys/class/leds/lcd-backlight/brightness";

const char* const BUTTONS_FILE
	= "/sys/class/leds/button-backlight/brightness";

const char* const LED_FILE[] = {
	"/sys/class/leds/red/brightness",
	"/sys/class/leds/green/brightness",
	"/sys/class/leds/blue/brightness",
};

const char* const LED_BLINK[] = {
	"/sys/class/leds/red/blink",
	"/sys/class/leds/green/blink",
	"/sys/class/leds/blue/blink",
};

const char* const LED_RISE[] = {
	"/sys/class/leds/red/risetime",
	"/sys/class/leds/green/risetime",
	"/sys/class/leds/blue/risetime",
};

const char* const LED_FALL[] = {
	"/sys/class/leds/red/falltime",
	"/sys/class/leds/green/falltime",
	"/sys/class/leds/blue/falltime",
};

const size_t LED_SIZE
	= sizeof(LED_FILE) / sizeof(LED_FILE[0]);

/**
 * device methods
 */

void init_globals(void)
{
	// init the mutex
	pthread_mutex_init(&g_lock, NULL);
}

static int
write_int(char const* path, int value)
{
	int fd;
	static int already_warned = 0;

	fd = open(path, O_RDWR);
	if (fd >= 0) {
		char buffer[20];
		int bytes = snprintf(buffer, sizeof(buffer), "%d\n", value);
		ssize_t amt = write(fd, buffer, (size_t)bytes);
		close(fd);
		return amt == -1 ? -errno : 0;
	} else {
		if (already_warned == 0) {
			ALOGE("write_int failed to open %s\n", path);
			already_warned = 1;
		}
		return -errno;
	}
}

static int
rgb_to_brightness(const struct light_state_t *state)
{
	int color = state->color & 0x00ffffff;
	return ((77*((color>>16)&0x00ff))
			+ (150*((color>>8)&0x00ff))
			+ (29*(color&0x00ff))) >> 8;
}

static int
is_lit(struct light_state_t const* state)
{
	return state->color & 0xffffffff;
}

void reset_leds(void) {
	size_t i;

	for (i = 0; i < LED_SIZE; i++) {
		write_int(LED_FILE[i], 0);	// switch off
		write_int(LED_BLINK[i], 0);	// no blink
		write_int(LED_RISE[i], 3);	// 1s rise
		write_int(LED_FALL[i], 3);	// 1s fall
	}
}

static int
set_speaker_light_locked(struct light_device_t *dev,
				struct light_state_t const *state)
{
	unsigned int colorRGB;
	int rgb[3];
	int onMS, offMS;
	size_t i;

	if (!dev) {
		reset_leds();
		return -1;
	}

	if (state == NULL) {
		return 0;
	}

	colorRGB = state->color;
	alpha = (colorRGB >> 24) & 0xFF;

	rgb[0] = ((colorRGB >> 16) & 0xFF) * alpha / 255;	// red
	rgb[1] = ((colorRGB >> 8) & 0xFF) * alpha / 255;	// green
	rgb[2] = (colorRGB & 0xFF) * alpha / 255;		// blue

	switch (state->flashMode) {
	case LIGHT_FLASH_TIMED:
	case LIGHT_FLASH_HARDWARE:
		onMS = state->flashOnMS;
		offMS = state->flashOffMS;
		break;
	case LIGHT_FLASH_NONE:
	default:
		onMS = 0;
		offMS = 0;
		break;
	}

	for (i = 0; i < LED_SIZE; i++) {
		write_int(LED_FILES[i], rgb[i]);
	}

	if (onMS == 0 || onMS == 1) { // LED always ON
		;
	} else { // LED blink
		;
	}
}

static void
handle_speaker_light_locked(struct light_device_t *dev)
{
	if (is_lit(&g_attention)) {
		set_speaker_light_locked(dev, &g_attention);
	} else if (is_lit(&g_notification)) {
		set_speaker_light_locked(dev, &g_notification);
	} else {
		set_battery_light_locked(dev, &g_battery);
	}
}

static int
set_light_backlight(struct light_device_t *dev,
			const struct light_state_t *state)
{
	int err = 0;
	int brightness = rgb_to_brightness(state);

	pthread_mutex_lock(&g_lock);

	err = write_int(BACKLIGHT_FILE, brightness);

	pthread_mutex_unlock(&g_lock);

	return err;
}

static int
set_light_buttons(struct light_device_t *dev,
			const struct light_state_t *state)
{
	int err = 0;
	int brightness = rgb_to_brightness(state);

	pthread_mutex_lock(&g_lock);

	err = write_int(BUTTONS_FILE, brightness);

	pthread_mutex_unlock(&g_lock);

	return err;
}

static int
set_light_battery(struct light_device_t *dev,
			const struct light_state_t *state)
{
	pthread_mutex_lock(&g_lock);

	g_battery = *state;

	handle_speaker_light_locked(dev);

	pthread_mutex_unlock(&g_lock);

	return 0;
}

static int
set_light_notifications(struct light_device_t *dev,
			const struct light_state_t *state)
{
	pthread_mutex_lock(&g_lock);

	g_notification = *state;

	handle_speaker_light_locked(dev);

	pthread_mutex_unlock(&g_lock);

	return 0;
}

static int
set_light_attention(struct light_device_t *dev,
			const struct light_state_t *state)
{
	pthread_mutex_lock(&g_lock);

	g_attention = *state;

	handle_speaker_light_locked(dev);

	pthread_mutex_unlock(&g_lock);

	return 0;
}

/** Close the lights device */
static int
close_lights(struct light_device_t *dev)
{
	if (dev) {
		free(dev);
	}
	return 0;
}

/******************************************************************************/

/**
 * module methods
 */

/** Open a new instance of a lights device using name */
static int open_lights(const struct hw_module_t *module, const char *name,
			struct hw_device_t **device)
{
	int (*set_light)(struct light_device_t *dev,
		const struct light_state_t *state);

	if (0 == strcmp(LIGHT_ID_BACKLIGHT, name))
		set_light = set_light_backlight;
	else if (0 == strcmp(LIGHT_ID_BUTTONS, name))
		set_light = set_light_buttons;
	else if (0 == strcmp(LIGHT_ID_BATTERY, name))
		set_light = set_light_battery;
	else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name))
		set_light = set_light_notifications;
	else if (0 == strcmp(LIGHT_ID_ATTENTION, name))
		set_light = set_light_attention;
	else
		return -EINVAL;

	pthread_once(&g_init, init_globals);

	struct light_device_t *dev = malloc(sizeof(struct light_device_t));
	memset(dev, 0, sizeof(*dev));

	dev->common.tag = HARDWARE_DEVICE_TAG;
	dev->common.version = 0;
	dev->common.module = (struct hw_module_t*)module;
	dev->common.close = (int (*)(struct hw_device_t*))close_lights;
	dev->set_light = set_light;

	*device = (struct hw_device_t*)dev;
	return 0;
}

static struct hw_module_methods_t lights_module_methods = {
	.open =  open_lights,
};

/*
 * The lights Module
 */
struct hw_module_t HAL_MODULE_INFO_SYM = {
	.tag = HARDWARE_MODULE_TAG,
	.version_major = 1,
	.version_minor = 0,
	.id = LIGHTS_HARDWARE_MODULE_ID,
	.name = "Xiaomi Lights Module",
	.author = "The CyanogenMod Project",
	.methods = &lights_module_methods,
};
