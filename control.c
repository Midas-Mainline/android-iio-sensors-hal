/*
 * Copyright (C) 2014 Intel Corporation.
 */

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <utils/Log.h>
#include <hardware/sensors.h>
#include "control.h"
#include "enumeration.h"
#include "utils.h"
#include "transform.h"

/* Currently active sensors count, per device */
static int poll_sensors_per_dev[MAX_DEVICES];	/* poll-mode sensors */
static int trig_sensors_per_dev[MAX_DEVICES];	/* trigger, event based */

static int device_fd[MAX_DEVICES];   /* fd on the /dev/iio:deviceX file */

static int poll_fd; /* epoll instance covering all enabled sensors */

static int poll_socket_pair[2];	/* used to unblock the poll loop */

/* Timestamp for the moment when we last exited a poll operation */
static int64_t last_poll_exit_ts;

static int active_poll_sensors; /* Number of enabled poll-mode sensors */

/* Cap the time between poll operations to this, to counter runaway polls */
#define POLL_MIN_INTERVAL 10000 /* uS */

#define INVALID_DEV_NUM ((uint32_t) -1)


static int enable_buffer(int dev_num, int enabled)
{
	char sysfs_path[PATH_MAX];

	sprintf(sysfs_path, ENABLE_PATH, dev_num);

	/* Low level, non-multiplexed, enable/disable routine */
	return sysfs_write_int(sysfs_path, enabled);
}


static int setup_trigger(int dev_num, const char* trigger_val)
{
	char sysfs_path[PATH_MAX];

	sprintf(sysfs_path, TRIGGER_PATH, dev_num);

	return sysfs_write_str(sysfs_path, trigger_val);
}


static void refresh_sensor_report_maps(int dev_num)
{
	/*
	 * Read sysfs files from a iio device's scan_element directory, and
	 * build a couple of tables from that data. These tables will tell, for
	 * each sensor, where to gather relevant data in a device report, i.e.
	 * the structure that we read from the /dev/iio:deviceX file in order to
	 * sensor report, itself being the data that we return to Android when a
	 * sensor poll completes. The mapping should be straightforward in the
	 * case where we have a single sensor active per iio device but, this is
	 * not the general case. In general several sensors can be handled
	 * through a single iio device, and the _en, _index and _type syfs
	 * entries all concur to paint a picture of what the structure of the
	 * device report is.
	 */

	int s;
	int c;
	int n;
	int i;
	int ch_enabled;
	int ch_index;
	char* ch_spec;
	char spec_buf[MAX_TYPE_SPEC_LEN];
	struct datum_info_t* ch_info;
	int size;
	char sysfs_path[PATH_MAX];
	int active_channels;
	int offset;
	int channel_count;
	int channel_size_from_index[MAX_SENSORS * MAX_CHANNELS] = { 0 };
	int sensor_handle_from_index[MAX_SENSORS * MAX_CHANNELS] = { 0 };
	int channel_number_from_index[MAX_SENSORS * MAX_CHANNELS] = { 0 };

	active_channels = 0;

	/* For each sensor that is linked to this device */
	for (s=0; s<sensor_count; s++) {
		if (sensor_info[s].dev_num != dev_num)
			continue;

		i = sensor_info[s].catalog_index;

		/* Read channel status through syfs attributes */
		for (c=0; c<sensor_info[s].num_channels; c++) {

			/* Read _en file */
			sprintf(sysfs_path, CHANNEL_PATH "%s",
				sensor_info[s].dev_num,
				sensor_catalog[i].channel[c].en_path);

			n = sysfs_read_int(sysfs_path, &ch_enabled);

			if (n == -1) {
				ALOGW(	"Failed to read _en flag: %s\n",
				sysfs_path);
				continue;
			}

			if (!ch_enabled != 1) {
				sensor_info[s].channel[c].size = 0;
			}

			/* Read _type file */
			sprintf(sysfs_path, CHANNEL_PATH "%s",
				sensor_info[s].dev_num,
				sensor_catalog[i].channel[c].type_path);

			n = sysfs_read_str(sysfs_path, spec_buf, 
						sizeof(spec_buf));

			if (n == -1) {
					ALOGW(	"Failed to read type: %s\n",
					sysfs_path);
					continue;
				}

			ch_spec = sensor_info[s].channel[c].type_spec;

			memcpy(ch_spec, spec_buf, sizeof(spec_buf));

			ch_info = &sensor_info[s].channel[c].type_info;

			size = decode_type_spec(ch_spec, ch_info);

			/* Read _index file */
			sprintf(sysfs_path, CHANNEL_PATH "%s",
				sensor_info[s].dev_num,
				sensor_catalog[i].channel[c].index_path);

			n = sysfs_read_int(sysfs_path, &ch_index);

			if (n == -1) {
					ALOGW(	"Failed to read index: %s\n",
						sysfs_path);
					continue;
				}

			if (ch_index >= MAX_SENSORS) {
				ALOGE("Index out of bounds!: %s\n", sysfs_path);
				continue;
			}

			/* Record what this index is about */

			sensor_handle_from_index [ch_index] = s;
			channel_number_from_index[ch_index] = c;
			channel_size_from_index  [ch_index] = size;

			active_channels++;
		}
	}

	ALOGI("Found %d enabled channels for iio device %d\n", active_channels,
		dev_num);

	/*
	 * Now that we know which channels are enabled, their sizes and their
	 * ordering, update channels offsets within device report. Note: there
	 * is a possibility that several sensors share the same index, with
	 * their data fields being isolated by masking and shifting as specified
	 * through the real bits and shift values in type attributes. This case
	 * is not currently supported. Also, the code below assumes no hole in
	 * the sequence of indices, so it is dependent on discovery of all
	 * sensors.
	 */
	 offset = 0;
	 for (i=0; i<MAX_SENSORS * MAX_CHANNELS; i++) {
		s =	sensor_handle_from_index[i];
		c =	channel_number_from_index[i];
		size = 	channel_size_from_index[i];

		if (!size)
			continue;

		ALOGI("S%d C%d : offset %d, size %d, type %s\n",
		      s, c, offset, size, sensor_info[s].channel[c].type_spec);

		sensor_info[s].channel[c].offset	= offset;
		sensor_info[s].channel[c].size		= size;

		offset += size;
	 }
}


int adjust_counters (int s, int enabled)
{
	/*
	 * Adjust counters based on sensor enable action. Return values are:
	 * -1 if there's an inconsistency: abort action in this case
	 *  0 if the operation was completed and we're all set
	 *  1 if we toggled the state of the sensor and there's work left
	 */

	int dev_num = sensor_info[s].dev_num;

	/* Refcount per sensor, in terms of enable count */
	if (enabled) {
		ALOGI("Enabling sensor %d (iio device %d: %s)\n",
			s, dev_num, sensor_info[s].friendly_name);

		sensor_info[s].enable_count++;

		if (sensor_info[s].enable_count != 1)
			return 0; /* The sensor was, and remains, in use */
	} else {
		if (sensor_info[s].enable_count == 0)
			return -1; /* Spurious disable call */

		ALOGI("Disabling sensor %d (iio device %d: %s)\n", s, dev_num,
		      sensor_info[s].friendly_name);

		sensor_info[s].enable_count--;

		if (sensor_info[s].enable_count > 0)
			return 0; /* The sensor was, and remains, in use */

		/* Sensor disabled, clear up pending data */

		sensor_info[s].report_pending = 0;
		memset(sensor_info[s].report_buffer, 0, MAX_SENSOR_REPORT_SIZE);
	}

	/* We changed the state of a sensor - adjust per iio device counters */

	/* If this is a regular event-driven sensor */
	if (sensor_info[s].num_channels) {

			if (enabled)
				trig_sensors_per_dev[dev_num]++;
			else
				trig_sensors_per_dev[dev_num]--;

			return 1;
		}

	if (enabled) {
		active_poll_sensors++;
		poll_sensors_per_dev[dev_num]++;
		return 1;
	}

	active_poll_sensors--;
	poll_sensors_per_dev[dev_num]--;
	return 1;
}


int sensor_activate(int s, int enabled)
{
	char sysfs_path[PATH_MAX];
	char device_name[PATH_MAX];
	char trigger_name[MAX_NAME_SIZE + 16];
	int c;
	struct epoll_event ev = {0};
	int dev_fd;
	int ret;
	int dev_num = sensor_info[s].dev_num;
	int i = sensor_info[s].catalog_index;
	int is_poll_sensor = !sensor_info[s].num_channels;

	ret = adjust_counters(s, enabled);

	/* If the operation was neutral in terms of state, we're done */
	if (ret <= 0)
		return ret;

	if (!is_poll_sensor) {
		/* Changes have to be made while the buffer is turned off */
		enable_buffer(dev_num, 0);

		/* Configure trigger */
		switch (trig_sensors_per_dev[dev_num]) {
			case 0:
				setup_trigger(dev_num, "none");
				break;

			case 1:
				sprintf(trigger_name, "%s-dev%d",
					sensor_info[s].internal_name, dev_num);

				setup_trigger(dev_num, trigger_name);
				break;

			default:
				/* The trigger is already set */
				break;
		}

		/*
		 * Turn channels associated to this sensor on or off, and update
		 * the channels maps for all sensors associated to this device.
		 */
		for (c=0;c<sensor_info[s].num_channels; c++) {
			sprintf(sysfs_path, CHANNEL_PATH "%s",
				sensor_info[s].dev_num,
				sensor_catalog[i].channel[c].en_path);

			sysfs_write_int(sysfs_path, enabled);
		}

		/* If there's at least one sensor left */
		if (trig_sensors_per_dev[dev_num]) {
			refresh_sensor_report_maps(dev_num);
			enable_buffer(dev_num, 1);
		}
	}

	/*
	 * Make sure we have a fd on the character device ; conversely, close
	 * the fd if no one is using associated sensor anymore. The assumption
	 * here is that the underlying driver will power on the relevant
	 * hardware block while someone hold a fd on the device.
	 */
	dev_fd = device_fd[dev_num];

	if (!enabled) {
		if (dev_fd != -1 && !poll_sensors_per_dev[dev_num] &&
			!trig_sensors_per_dev[dev_num]) {
				/*
				 * Stop watching this fd. This should be a no-op
				 * in case this fd was not in the poll set.
				 */
				epoll_ctl(poll_fd, EPOLL_CTL_DEL, dev_fd, NULL);

				close(dev_fd);
				device_fd[dev_num] = -1;
			}
		return 0;
	}

	if (dev_fd == -1) {
		/* First enabled sensor on this iio device */
		sprintf(device_name, DEV_FILE_PATH, dev_num);
		dev_fd = open(device_name, O_RDONLY | O_NONBLOCK);

		device_fd[dev_num] = dev_fd;

		if (dev_fd == -1) {
			ALOGE("Could not open fd on %s (%s)\n",
			      device_name, strerror(errno));
			adjust_counters(s, 0);
			return -1;
		}

		ALOGV("Opened %s: fd=%d\n", device_name, dev_fd);

		if (!is_poll_sensor) {

			/* Add this iio device fd to the set of watched fds */
			ev.events = EPOLLIN;
			ev.data.u32 = dev_num;

			ret = epoll_ctl(poll_fd, EPOLL_CTL_ADD, dev_fd, &ev);

			if (ret == -1) {
				ALOGE(	"Failed adding %d to poll set (%s)\n",
					dev_fd, strerror(errno));
				return -1;
			}

			/* Note: poll-mode fds are not readable */
		}
	}

	/* Release the polling loop so an updated timeout gets used */
	write(poll_socket_pair[1], "", 1);

	return 0;
}


static int integrate_device_report(int dev_num)
{
	int len;
	int s,c;
	unsigned char buf[MAX_SENSOR_REPORT_SIZE * MAX_SENSORS] = { 0 };
	int sr_offset;
	unsigned char *target;
	unsigned char *source;
	int size;
	int expected_size = 0;

	/* There's an incoming report on the specified fd */

	if (dev_num < 0 || dev_num >= MAX_DEVICES ||
		!trig_sensors_per_dev[dev_num]) {
		ALOGE("Event reported on unexpected iio device %d\n", dev_num);
		return -1;
	}

	for (s=0; s<MAX_SENSORS; s++)
		if (sensor_info[s].dev_num == dev_num)
			for (c=0; c<sensor_info[s].num_channels; c++)
				expected_size += sensor_info[s].channel[c].size;

	len = read(device_fd[dev_num], buf, expected_size);

	if (len == -1) {
		ALOGE("Could not read report from iio device %d (%s)\n",
		      dev_num, strerror(errno));
		return -1;
	}

	ALOGV("Read %d bytes from iio device %d\n", len, dev_num);

	for (s=0; s<MAX_SENSORS; s++)
		if (sensor_info[s].dev_num == dev_num) {
			sr_offset = 0;

			/* Copy data from device to sensor report buffer */
			for (c=0; c<sensor_info[s].num_channels; c++) {

				target = sensor_info[s].report_buffer +
					sr_offset;

				source = buf + sensor_info[s].channel[c].offset;

				size = sensor_info[s].channel[c].size;

				memcpy(target, source, size);

				sr_offset += size;
			}

			if (sensor_info[s].enable_count) {
				ALOGV("Sensor %d report available (%d bytes)\n",
				      s, sr_offset);

				sensor_info[s].report_pending = 1;
			}
		}

	return 0;
}


static void propagate_sensor_report(int s, struct sensors_event_t* data)
{
	/* There's a sensor report pending for this sensor ; transmit it */

	int catalog_index = sensor_info[s].catalog_index;
	int sensor_type = sensor_catalog[catalog_index].type;
	int num_fields;
	int c;
	unsigned char* current_sample;

	memset(data, 0, sizeof(sensors_event_t));

	data->version = sizeof(sensors_event_t);
	data->sensor = s;
	data->type = sensor_type;
	data->timestamp = get_timestamp();

	switch (sensor_type) {
		case SENSOR_TYPE_ACCELEROMETER:		/* m/s^2	*/
		case SENSOR_TYPE_MAGNETIC_FIELD:	/* micro-tesla	*/
		case SENSOR_TYPE_ORIENTATION:		/* degrees	*/
		case SENSOR_TYPE_GYROSCOPE:		/* radians/s	*/
			num_fields = 3;
			break;

		case SENSOR_TYPE_LIGHT:			/* SI lux units */
		case SENSOR_TYPE_AMBIENT_TEMPERATURE:	/* °C		*/
		case SENSOR_TYPE_TEMPERATURE:		/* °C		*/
		case SENSOR_TYPE_PROXIMITY:		/* centimeters	*/
		case SENSOR_TYPE_PRESSURE:		/* hecto-pascal */
		case SENSOR_TYPE_RELATIVE_HUMIDITY:	/* percent */
			num_fields = 1;
			break;

		case SENSOR_TYPE_ROTATION_VECTOR:
			num_fields = 4;
			break;

		case SENSOR_TYPE_DEVICE_PRIVATE_BASE:	/* hidden for now */
			num_fields = 0;
			break;

		default:
			ALOGE("Unknown sensor type!\n");
			num_fields = 0;
			break;
	}

	ALOGV("Sample on sensor %d (type %d):\n", s, sensor_type);

	/* Take note of current time counter value for rate control purposes */
	sensor_info[s].last_integration_ts = get_timestamp();

	/* If we're dealing with a poll-mode sensor */
	if (!sensor_info[s].num_channels) {

		/* Read values through sysfs rather than from a report buffer */
		for (c=0; c<num_fields; c++) {

			data->data[c] = acquire_immediate_value(s, c);

			ALOGV("\tfield %d: %f\n", c, data->data[c]);
		}

		sensor_info[s].ops.finalize(s, data);
		return;
	}

	/* Convert the data into the expected Android-level format */

	current_sample = sensor_info[s].report_buffer;

	for (c=0; c<num_fields; c++) {

		data->data[c] = sensor_info[s].ops.transform
							(s, c, current_sample);

		ALOGV("\tfield %d: %f\n", c, data->data[c]);
		current_sample += sensor_info[s].channel[c].size;
	}

	sensor_info[s].ops.finalize(s, data);
}


static int get_poll_time (void)
{
	int64_t target_ts;
	int64_t lowest_target_ts;
	int64_t current_ts;
	int s;

	if (!active_poll_sensors)
		return -1;	/* Infinite wait */

	/* Check if we should schedule a poll-mode sensor event delivery */

	lowest_target_ts = INT64_MAX;

	for (s=0; s<sensor_count; s++)
		if (sensor_info[s].enable_count &&
		    sensor_info[s].sampling_rate &&
		    !sensor_info[s].num_channels) {
				target_ts = sensor_info[s].last_integration_ts +
				      1000000000LL/sensor_info[s].sampling_rate;

				if (target_ts < lowest_target_ts)
					lowest_target_ts = target_ts;
			}

	if (lowest_target_ts == INT64_MAX)
		return -1;

	current_ts = get_timestamp();

	if (lowest_target_ts <= current_ts)
		return 0;

	return (lowest_target_ts - current_ts)/1000000; /* ms */
}


static void acknowledge_release (void)
{
	/* A write to our socket circuit was performed to release epoll */
	char buf;
	read(poll_socket_pair[0], &buf, 1);
}


int sensor_poll(struct sensors_event_t* data, int count)
{
	int s;
	int i;
	int nfds;
	int delta;
	struct epoll_event ev[MAX_DEVICES];

	/* Get one or more events from our collection of sensors */

return_first_available_sensor_report:

	/* If there's at least one available report */
	for (s=0; s<sensor_count; s++)
		if (sensor_info[s].report_pending) {

			/* Return that up */
			propagate_sensor_report(s, data);
			sensor_info[s].report_pending = 0;
			ALOGV("Report on sensor %d\n", s);
			return 1;
		}
await_event:

	/* Keep a minimum time interval between poll operations */
	delta = (get_timestamp() - last_poll_exit_ts)/1000;

	if (delta > 0 && delta < POLL_MIN_INTERVAL)
		usleep(POLL_MIN_INTERVAL - delta);

	ALOGV("Awaiting sensor data\n");

	nfds = epoll_wait(poll_fd, ev, MAX_DEVICES, get_poll_time());

	last_poll_exit_ts = get_timestamp();

	if (nfds == -1) {
		ALOGI("epoll_wait returned -1 (%s)\n", strerror(errno));
		goto await_event;
	}

	ALOGV("%d fds signalled\n", nfds);

	/* For each of the devices for which a report is available */
	for (i=0; i<nfds; i++)
		if (ev[i].events == EPOLLIN) {
			if (ev[i].data.u32 == INVALID_DEV_NUM) {
				acknowledge_release();
				goto await_event;
			} else
				/* Read report */
				integrate_device_report(ev[i].data.u32);
		}

	/* It's a good time to invalidate poll-mode sensor values */
	if (active_poll_sensors)
		for (s=0; s<sensor_count; s++)
			if (sensor_info[s].enable_count &&
				!sensor_info[s].num_channels)
					sensor_info[s].report_pending = 1;

	goto return_first_available_sensor_report;
}


int sensor_set_delay(int s, int64_t ns)
{
	/* Set the rate at which a specific sensor should report events */

	/* See Android sensors.h for indication on sensor trigger modes */

	char sysfs_path[PATH_MAX];
	int dev_num		=	sensor_info[s].dev_num;
	int i			=	sensor_info[s].catalog_index;
	const char *prefix	=	sensor_catalog[i].tag;
	int new_sampling_rate;
	int cur_sampling_rate;

	if (!ns) {
		ALOGE("Rejecting zero delay request on sensor %d\n", s);
		return -EINVAL;
	}

	new_sampling_rate = (int) (1000000000L/ns);

	if (!new_sampling_rate) {
		ALOGI("Sub-HZ sampling rate requested on on sensor %d\n", s);
		new_sampling_rate = 1;
	}

	sprintf(sysfs_path, SENSOR_SAMPLING_PATH, dev_num, prefix);

	if (sysfs_read_int(sysfs_path, &cur_sampling_rate) != -1)
		if (new_sampling_rate != cur_sampling_rate) {
			ALOGI(	"Sensor %d sampling rate set to %d\n",
				s, new_sampling_rate);

			if (trig_sensors_per_dev[dev_num])
				enable_buffer(dev_num, 0);

			sysfs_write_int(sysfs_path, new_sampling_rate);

			if (trig_sensors_per_dev[dev_num])
				enable_buffer(dev_num, 1);
	}

	sensor_info[s].sampling_rate = new_sampling_rate;

	/* Release the polling loop so an updated timeout value gets used */
	write(poll_socket_pair[1], "", 1);

	return 0;
}


int allocate_control_data (void)
{
	int i;
	struct epoll_event ev = {0};

	for (i=0; i<MAX_DEVICES; i++)
		device_fd[i] = -1;

	poll_fd = epoll_create(MAX_DEVICES);

	if (poll_fd == -1) {
		ALOGE("Can't create epoll instance for iio sensors!\n");
		return -1;
	}

	/* Create and add "unblocking" fd to the set of watched fds */

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, poll_socket_pair) == -1) {
		ALOGE("Can't create socket pair for iio sensors!\n");
		close(poll_fd);
		return -1;
	}

	ev.events = EPOLLIN;
	ev.data.u32 = INVALID_DEV_NUM;

	epoll_ctl(poll_fd, EPOLL_CTL_ADD, poll_socket_pair[0], &ev);

	return poll_fd;
}


void delete_control_data (void)
{
}
