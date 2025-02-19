/*
* Copyright by flar2 (c) 2016-2018
* Copyright by Pal Zoltan Illes (c) 2020-2021
* Licensed under GPL-v2 or above.
*/
#include <linux/module.h>
#include <linux/kernel.h>    
#include <linux/init.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/slab.h>

#ifdef CONFIG_UCI
#include <linux/uci/uci.h>
#include <linux/notification/notification.h>
#endif

#define DRIVER_AUTHOR "Pal Zoltan Illes"
#define DRIVER_DESCRIPTION "sweep2sleep driver"
#define DRIVER_VERSION "4.1"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");

//sweep2sleep
#define S2S_PWRKEY_DUR         20
#if 1
// 3040x1440
static int S2S_Y_MAX = 3040;
static int S2S_X_MAX = 1440;
static int S2S_X_LEFT_CORNER_END = 150;
static int S2S_X_RIGHT_CORNER_START = 1290; // 1440-150
#else
// 2280x1080
#define S2S_Y_MAX             	2280
#define S2S_X_LEFT_CORNER_END 90
#define S2S_X_RIGHT_CORNER_START 950
#endif

#define SWEEP_RIGHT		0x01
#define SWEEP_LEFT		0x02
#define VIB_STRENGTH		20

#define X_DIFF_THRESHOLD_0 70 // 200
#define X_DIFF_THRESHOLD_1 70 // 180

// 0 on, 1 off
static int s2s_onoff = 0;
// 1=sweep right, 2=sweep left, 3=both
static int s2s_switch = 0;
static int s2s_filter_mode = 0; // 0 input filter NO, 1 YES RIGHT HANDED MODE, 2 YES LEFT HANDED MODE, 3 BOTH HANDS
static int s2s_doubletap_mode = 0; // 0 - off, 1 - powerOff, 2 - signal thru UCI
static int s2s_longtap_switch = 1; // 0 - off, 1 - on, 2 - on/disable swipe gesture
static int s2s_longtap_min_holdtime = 100; // 60 - 300 jiffies
static int s2s_height = 130;
static int s2s_doubletap_height = 70; // where doubletap Y coordinates are registered
static int s2s_height_above = 20;
static int s2s_width = 70;
static int s2s_from_corner = 0;
static int s2s_width_cutoff = 60;
static int s2s_corner_width = 150;
static int s2s_continuous_vib = 0;
static int s2s_wait_for_finger_leave = 1;
static int s2s_reenable_after_screen_off = 1;

static int touch_x = 0, touch_y = 0, firstx = 0;
static bool touch_x_called = false, touch_y_called = false, touch_down_called = false;
static bool scr_on_touch = false, barrier[2] = {false, false};
static bool exec_count = true;
static struct input_dev * sweep2sleep_pwrdev;
static DEFINE_MUTEX(pwrkeyworklock);
static DEFINE_MUTEX(longtapworklock);
static struct workqueue_struct *s2s_input_wq;
static struct work_struct s2s_input_work;
//extern void set_vibrate(int value);
extern void set_vibrate_2(int value, int boost_power);
static int vib_strength = VIB_STRENGTH;
static bool first_event = false;
static bool setup_done = false;

// set to true if screen off was executed after the s2s gesture
static bool screen_off_after_gesture = true;

static bool filter_coords_status = false;

extern char* init_get_saved_command_line(void);

#ifdef CONFIG_UCI
static int get_s2s_switch(void) {
	return s2s_onoff?s2s_switch:0;
}
static int get_s2s_filter_mode(void) { // 0 off, 1 right handed, 2 left handed, 3 both
	return s2s_filter_mode;
}
static int get_s2s_doubletap_mode(void) {
	return s2s_doubletap_mode;
}
static int get_s2s_longtap_switch(void) {
	return s2s_longtap_switch;
}
static int get_s2s_height(void) {
	return s2s_height;
}
static int get_s2s_doubletap_height(void) {
	return s2s_doubletap_height;
}
static int get_s2s_height_above(void) {
	return s2s_height_above;
}
static int get_s2s_width(void) {
	return s2s_width;
}
static int get_s2s_width_cutoff(void) {
	return s2s_width_cutoff;
}
static int get_s2s_corner_width(void) {
	return s2s_corner_width;
}
static int get_s2s_from_corner(void) {
	return s2s_from_corner;
}
static int get_s2s_continuous_vib(void) {
	return s2s_continuous_vib;
}
static int get_s2s_wait_for_finger_leave(void) {
	return s2s_wait_for_finger_leave;
}
static int get_s2s_reenable_after_screen_off(void) {
	return s2s_reenable_after_screen_off;
}

static int get_s2s_y_limit(void) {
	return S2S_Y_MAX - get_s2s_height();
}
static int get_s2s_y_limit_doubletap(void) {
	return S2S_Y_MAX - get_s2s_doubletap_height();
}
static int get_s2s_y_above(void) {
	return S2S_Y_MAX - get_s2s_height_above();
}
#endif

// device specifics
static void s2s_setup_values(void) {
	if ( strstr(init_get_saved_command_line(),"hardware=coral") ) {
		// CORAL 3040x1440
		pr_info("%s hw coral\n",__func__);
		// leave original values
	} else {
		pr_info("%s hw flame\n",__func__);
		// FLAME 2280x1080
		S2S_Y_MAX = 2280;
		S2S_X_MAX = 1080;
		S2S_X_LEFT_CORNER_END = 100;
		S2S_X_RIGHT_CORNER_START = 1080-100;
	}
}

static int finger_counter = 0;
static bool pause_before_pwr_off = false;

static bool check_no_finger(int timeout) {
	int timeout_count = 0;
	if (!get_s2s_wait_for_finger_leave()) return true;
	while (1) {
		if (finger_counter==0) break;
		msleep(2);
		timeout_count++;
		if (timeout_count>timeout)
		{
			return false;
		}
	}
	return true;
}

/* PowerKey work func */
static void sweep2sleep_presspwr(struct work_struct * sweep2sleep_presspwr_work) {
	if (!mutex_trylock(&pwrkeyworklock))
                return;

	if (!check_no_finger(100)) {
		set_vibrate_2(10,60);
		screen_off_after_gesture = true;
		goto exit_mutex;
	}

	// should indicate gesture cannot be done again till full screen off or unlocked
	screen_off_after_gesture = false;
	if (pause_before_pwr_off) msleep(260);
	pause_before_pwr_off = false;

	msleep(S2S_PWRKEY_DUR-20);

	if (!check_no_finger(1)) {
		set_vibrate_2(10,60);
		screen_off_after_gesture = true;
		goto exit_mutex;
	}


	input_event(sweep2sleep_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(sweep2sleep_pwrdev, EV_SYN, 0, 0);
	msleep(S2S_PWRKEY_DUR);
	input_event(sweep2sleep_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(sweep2sleep_pwrdev, EV_SYN, 0, 0);
	msleep(S2S_PWRKEY_DUR);
exit_mutex:
        mutex_unlock(&pwrkeyworklock);
	return;
}
static DECLARE_WORK(sweep2sleep_presspwr_work, sweep2sleep_presspwr);

static int vib_power = 50;
static void sweep2sleep_vib(struct work_struct * sweep2sleep_vib_work) {
	set_vibrate_2(vib_strength-10,vib_power);
	return;
}
static DECLARE_WORK(sweep2sleep_vib_work, sweep2sleep_vib);

/* PowerKey trigger */
static void sweep2sleep_pwrtrigger(void) {
	vib_power = 100;
	schedule_work(&sweep2sleep_vib_work);
	schedule_work(&sweep2sleep_presspwr_work);
        return;
}

static int last_tap_coord_x = 0;
static int last_tap_coord_y = 0;
static unsigned long last_tap_jiffies = 0;
static bool last_tap_starts_in_dt_area = false;

static int last_tap_for_longtap_coord_x = 0;
static int last_tap_for_longtap_coord_y = 0;
static unsigned long last_tap_for_longtap_jiffies = 0;

static void reset_longtap_tracking(void) {
	last_tap_for_longtap_coord_x = -1000;
	last_tap_for_longtap_coord_y = -1000;
	last_tap_for_longtap_jiffies = 0;
}
static void store_longtap_touch(void) {
	last_tap_for_longtap_coord_x = touch_x;
	last_tap_for_longtap_coord_y = touch_y;
	last_tap_for_longtap_jiffies = jiffies;
}

static void reset_doubletap_tracking(void) {
	last_tap_coord_x = 0;
	last_tap_coord_y = 0;
	last_tap_jiffies = 0;
}
static void store_doubletap_touch(void) {
	last_tap_coord_x = touch_x;
	last_tap_coord_y = touch_y;
	last_tap_jiffies = jiffies;
}

static void sweep2sleep_longtap_count(struct work_struct * sweep2sleep_longtap_count_work) {
	unsigned int last_tap_time_diff = 0;
	mutex_lock(&longtapworklock);
	store_longtap_touch();
	while (true) {
		mdelay(10);
		if (last_tap_for_longtap_jiffies == 0) { // doubletap/longtap tracking was reset, when finger pulled off
			break;
		}
		last_tap_time_diff = jiffies - last_tap_for_longtap_jiffies;
		{
			int delta_x = last_tap_for_longtap_coord_x - touch_x;
			int delta_y = last_tap_for_longtap_coord_y - touch_y;
#ifdef CONFIG_DEBUG_S2S
			pr_info("%s longtap check at finger mvmnt, Time: %u X: %d Y: %d\n",__func__,last_tap_time_diff,delta_x,delta_y);
#endif
			if (delta_x > 60 || delta_x < -60 || delta_y > 60 || delta_y < -60) {
				goto exit_mutex;
			}
		}
		// first touch time is past enough (100)
		if (last_tap_time_diff > s2s_longtap_min_holdtime) {
			reset_doubletap_tracking();
			reset_longtap_tracking();
			if (get_s2s_doubletap_mode()==1) { // power button mode - long tap -> notif down
				vib_power = 100;
				schedule_work(&sweep2sleep_vib_work);
				write_uci_out("fp_touch");
			} else { // dt notif down mode -> long tap => power off
				// wait a bit before actually emulate pwr button press in the trigger, to avoid wake screen on lockscreen touch
				if (uci_get_sys_property_int_mm("locked", 0, 0, 1)) { // if locked...
					pause_before_pwr_off = true;
				}
				sweep2sleep_pwrtrigger();
			}
			goto exit_mutex;
		}
	}
exit_mutex:
        mutex_unlock(&longtapworklock);
	return;
}
static DECLARE_WORK(sweep2sleep_longtap_count_work, sweep2sleep_longtap_count);


/* reset on finger release */
static void sweep2sleep_reset(bool reset_filter_coords) {
	exec_count = true;
	barrier[0] = false;
	barrier[1] = false;
	firstx = 0;
	first_event = false;
	scr_on_touch = false;
	if (reset_filter_coords) {
		filter_coords_status = false;
	}
}


/* Sweep2sleep main function */
static void detect_sweep2sleep(int x, int y, bool st)
{
        int prevx = 0, nextx = 0;
#ifdef CONFIG_UCI
	static unsigned long last_scheduled_vib_time = 0;
	int s2s_y_limit = get_s2s_y_limit();
	int s2s_y_above = get_s2s_y_above();
#else
	int s2s_y_limit = S2S_Y_MAX - s2s_height;
	int s2s_y_above = S2S_Y_MAX - s2s_height_above;
#endif
	int x_threshold_0 = X_DIFF_THRESHOLD_0 + get_s2s_width();
	int x_threshold_1 = X_DIFF_THRESHOLD_1 + get_s2s_width();
        bool single_touch = st;


	if (firstx == 0) {
		firstx = x;
		first_event = true;
	}

	if (get_s2s_switch() > 3)
		s2s_switch = 3;

	if (get_s2s_switch()==0 || (get_s2s_filter_mode() && get_s2s_doubletap_mode() && get_s2s_longtap_switch()==2)) { // swipe is not disabled with longtap / dt only mode (switch == 2)
		return;
	}

	//left->right
	if (single_touch && ((firstx < (S2S_X_RIGHT_CORNER_START-40) && firstx < (S2S_X_MAX/2) && !get_s2s_from_corner()) || ((firstx > get_s2s_width_cutoff()) && firstx < get_s2s_corner_width())) && (get_s2s_switch() & SWEEP_RIGHT)) {
		scr_on_touch=true;
		prevx = firstx;
		nextx = prevx + x_threshold_1;
		if ((barrier[0] == true) ||
		   ((x > prevx) &&
		    (x < nextx) &&
		    ( (y > s2s_y_limit && y < s2s_y_above) || (filter_coords_status && get_s2s_filter_mode()) ) )) {
			if (get_s2s_filter_mode() && get_s2s_doubletap_mode() && get_s2s_longtap_switch()) { first_event = false; } // don't vibrate when longtap is on...
			if (((x > firstx + (15 + get_s2s_width()*0.3)) && first_event) || get_s2s_continuous_vib()) { // signal gesture start with vib, or continuously. Only start when at least X coordinate moved a little bit from first touch X (~20px)
				if (exec_count) {
					unsigned int last_vib_diff = jiffies - last_scheduled_vib_time;
					if (barrier[1] == true) { vib_power = 50; } else { vib_power = get_s2s_continuous_vib()?1:60; }
					if (last_vib_diff > 12) {
						schedule_work(&sweep2sleep_vib_work);
						last_scheduled_vib_time = jiffies;
					}
				}
				first_event = false;
			}
			prevx = nextx;
			nextx += x_threshold_0;
			barrier[0] = true;
			if (x > firstx + (15 + get_s2s_width()*0.3)) {
				reset_longtap_tracking();
			}
			if ((barrier[1] == true) ||
			   ((x > prevx) &&
			    (x < nextx) &&
			    ( (y > s2s_y_limit && y < s2s_y_above) || (filter_coords_status && get_s2s_filter_mode()) ) )) {
				prevx = nextx;
				barrier[1] = true;
				if ((x > prevx) &&
				    ( (y > s2s_y_limit && y < s2s_y_above) || (filter_coords_status && get_s2s_filter_mode()) ) ) {
					if (x > (nextx + x_threshold_1)) {
						if (exec_count) {
							if (uci_get_sys_property_int_mm("locked", 0, 0, 1)) { // if locked...
								pause_before_pwr_off = true;
							}
							sweep2sleep_pwrtrigger();
							exec_count = false;
						}
					}
				}
			}
		}
	//right->left
	} else if (((firstx >= (S2S_X_LEFT_CORNER_END-40) && firstx > (S2S_X_MAX/2) && !get_s2s_from_corner()) || (firstx >= S2S_X_MAX - get_s2s_corner_width() && (firstx < S2S_X_MAX - get_s2s_width_cutoff()))) && (get_s2s_switch() & SWEEP_LEFT)) {
		scr_on_touch=true;
		prevx = firstx;
		nextx = prevx - x_threshold_1;
		if ((barrier[0] == true) ||
		   ((x < prevx) &&
		    (x > nextx) &&
		    ( (y > s2s_y_limit && y < s2s_y_above) || (filter_coords_status && get_s2s_filter_mode()) ) )) {
			if (get_s2s_filter_mode() && get_s2s_doubletap_mode() && get_s2s_longtap_switch()) { first_event = false; } // don't vibrate when longtap is on...
			if (((x < firstx - (15 + get_s2s_width()*0.3)) && first_event) || get_s2s_continuous_vib()) { // signal gesture start with vib, or continuously. Only start when at least X coordinate moved a little bit from first touch X (~20px)
				if (exec_count) {
					unsigned int last_vib_diff = jiffies - last_scheduled_vib_time;
					if (barrier[1] == true) { vib_power = 50; } else { vib_power = get_s2s_continuous_vib()?1:60; }
					if (last_vib_diff > 12) {
						schedule_work(&sweep2sleep_vib_work);
						last_scheduled_vib_time = jiffies;
					}
				}
				first_event = false;
			}
			prevx = nextx;
			nextx -= x_threshold_0;
			barrier[0] = true;
			if (x < firstx - (15 + get_s2s_width()*0.3)) {
				reset_longtap_tracking();
			}
			if ((barrier[1] == true) ||
			   ((x < prevx) &&
			    (x > nextx) &&
			    ( (y > s2s_y_limit && y < s2s_y_above) || (filter_coords_status && get_s2s_filter_mode()) ) )) {
				prevx = nextx;
				barrier[1] = true;
				if ((x < prevx) &&
				    ( (y > s2s_y_limit && y < s2s_y_above) || (filter_coords_status && get_s2s_filter_mode()) ) ) {
					if (x < (nextx - x_threshold_1)) {
						if (exec_count) {
							if (uci_get_sys_property_int_mm("locked", 0, 0, 1)) { // if locked...
								pause_before_pwr_off = true;
							}
							sweep2sleep_pwrtrigger();
							exec_count = false;
						}
					}
				}
			}
		}
	}
}


static void s2s_input_callback(struct work_struct *unused) {

	detect_sweep2sleep(touch_x, touch_y, true);

	return;
}



#ifdef CONFIG_DEBUG_S2S
static int log_throttling_count = 0;
#endif

static int frozen_x = 0; // fake x/y for the user land
static int frozen_y = 0;
static int real_x = 0; // reported thru s2s_freeze_cords by touchscreen driver
static int real_y = 0;


#define FULL_FILTER

#ifdef FULL_FILTER
static int in_gesture_finger_counter = 0;
#endif
static int frozen_rand = 0;
static bool freeze_touch_area_detected = false;
static unsigned long last_outside_area_touch_time = 0;

bool s2s_freeze_coords(int *x, int *y, int r_x, int r_y) {
	real_x = r_x;
	real_y = r_y;
	if (get_s2s_switch() && get_s2s_filter_mode() && filter_coords_status) {
		*x = frozen_x + (frozen_rand)%2; // make some random variance so input report will actually get it through
		*y = S2S_Y_MAX + 3 + (frozen_rand++)%2; // don't let real Y get thru, it crashes the framework occasionally

#ifdef CONFIG_DEBUG_S2S
		pr_info("%s frozen coords used filtered mode: %d %d\n",__func__,*x,*y);
#endif
		return true;
	}
	{
		unsigned int time_diff = last_outside_area_touch_time - jiffies; // to avoid accidental touch down detections after outside coordinates, avoid triggering stock gestures...
		int s2s_y_limit = get_s2s_y_limit();
		int s2s_y_above = get_s2s_y_above();
#ifdef CONFIG_DEBUG_S2S
		pr_info("%s touch x/y gathered.\n",__func__);
#endif
		if (get_s2s_switch() && get_s2s_filter_mode() && !filter_coords_status && !finger_counter && time_diff>15) {
			if (
			// if ... first touch was not registered (filter_coords_status = false) && register only in corner area, and X is outside cordner area,
			(!get_s2s_from_corner() || (get_s2s_from_corner() && (r_x > S2S_X_MAX - get_s2s_corner_width() || r_x < get_s2s_corner_width()))) &&
			// or if... y is not in the touch area or x is not in the whole area,
			(r_y < s2s_y_above && r_y > s2s_y_limit) &&
			(r_x > get_s2s_width_cutoff()) && (r_x < S2S_X_MAX - get_s2s_width_cutoff()) &&
			// or if in filtering mode (left right handed separately checked) and X is not in the 40% of the possible width, and this is still the first touch (!filter_coords_status)...
			(
			(get_s2s_filter_mode() == 1 && (r_x > (S2S_X_MAX * 0.60))) || // right handed, on the left side tapped shouldn't filter...
			(get_s2s_filter_mode() == 2 && (r_x < (S2S_X_MAX * 0.40))) || // left handed, on the right side tapped...
			(get_s2s_filter_mode() == 3 && (r_x > (S2S_X_MAX * 0.60) || r_x < (S2S_X_MAX * 0.40))) // both handed, in the middle region
			)
			)
			{
				*x = r_x + (frozen_rand)%2; // make some random variance so input report will actually get it through
				*y = S2S_Y_MAX + 3 + (frozen_rand++)%2; // don't let real Y get thru, it crashes the framework occasionally
#ifdef CONFIG_DEBUG_S2S
				pr_info("%s first touch --- frozen coords used filtered mode: %d %d\n",__func__,*x,*y);
#endif
				freeze_touch_area_detected = true;
				return true;
			}
		}
	}
	freeze_touch_area_detected = false;
	last_outside_area_touch_time = jiffies;
	return false;
}
EXPORT_SYMBOL(s2s_freeze_coords);



#ifdef FULL_FILTER
static bool filtering_on(void) {
	return get_s2s_switch() && get_s2s_filter_mode() && (((filter_coords_status || freeze_touch_area_detected) && finger_counter<=1) || in_gesture_finger_counter>0);
}
#endif

static bool __s2s_input_filter(struct input_handle *handle, unsigned int type,
				unsigned int code, int value) {
	bool first_touch_detection = false;

	if (!setup_done) {
		setup_done = true;
		s2s_setup_values();
	}

	if (get_s2s_switch()==0) {
		sweep2sleep_reset(true);
		return false;
	}

#ifdef CONFIG_DEBUG_S2S
	if ((log_throttling_count++)%50>40) {
		pr_info("%s type: %d code: %d value: %d -- max y = %d\n",__func__,type,code,value,S2S_Y_MAX);
	}
	if (log_throttling_count%50==49) log_throttling_count = 0;
#endif

	if (type == EV_KEY && code == BTN_TOUCH && value == 1) {
#ifdef FULL_FILTER
		if (filtering_on()) {
			in_gesture_finger_counter++;
		}
#endif
		finger_counter++;

		if (!get_s2s_filter_mode() || (get_s2s_filter_mode() && freeze_touch_area_detected)) { // not in filtered mode, or freeze touch area detected...start touch down...
			if (finger_counter == 1) {
				touch_down_called = true;
			}
		} else {
			touch_down_called = false;
		}
		touch_x_called = false;
		touch_y_called = false;
		last_tap_starts_in_dt_area = false; // reset boolean
		sweep2sleep_reset(true);
#ifdef CONFIG_DEBUG_S2S
		pr_info("%s first touch...\n",__func__);
#endif
#ifdef FULL_FILTER
		return filtering_on();
#else
		return false;
#endif
	}

	if (type == EV_KEY && code == BTN_TOUCH && value == 0) {
#ifdef FULL_FILTER
		bool is_filtering_on = filtering_on();
		if (filtering_on()) {
			in_gesture_finger_counter--;
			if (in_gesture_finger_counter<0) in_gesture_finger_counter = 0;
		}
#else
		bool is_filtering_on = false;
#endif
		finger_counter--;
		if (finger_counter<0) finger_counter = 0;

		touch_down_called = false;
		touch_x_called = false;
		touch_y_called = false;
		reset_longtap_tracking();
		if (last_tap_starts_in_dt_area) {
			int delta_x = last_tap_coord_x - touch_x;
			int delta_y = last_tap_coord_y - touch_y;
#ifdef CONFIG_DEBUG_S2S
			pr_info("%s doubletap check at btn leave, Time: %u X: %d Y: %d\n",__func__,last_tap_time_diff,delta_x,delta_y);
#endif
			if (delta_x < 20 && delta_x > -20 && delta_y < 20 && delta_y > -20) {
				unsigned int last_tap_time_diff = jiffies - last_tap_jiffies;
				// first touch time is very close and didn't move more than 20px before leaving screen... finishing that touch within the area? vibrate...
				if (last_tap_time_diff < 50) { 
					if (!get_s2s_longtap_switch()) { // if not in longtap mode, then vibrate here (otherwise first touch already vibrates in other part of driver)
						vib_power = 60;
						schedule_work(&sweep2sleep_vib_work);
					}
				}
			} else {
				// finger leaving screen too far from the original touch point... cancel DT tracking data of first touch..
				reset_doubletap_tracking();
			}
		}
		last_tap_starts_in_dt_area = false; // reset boolean
		sweep2sleep_reset(true);
#ifdef CONFIG_DEBUG_S2S
		pr_info("%s untouch...\n",__func__);
#endif
		return is_filtering_on;
	}

	// if only reenable after a full sreen off is set, and the screen off not yet happened, s2s gesture shouldn't be available, return false here...
	if (get_s2s_reenable_after_screen_off() && !screen_off_after_gesture) return false;

	if (code == ABS_MT_SLOT) {
		touch_x_called = false;
		touch_y_called = false;
#ifdef CONFIG_DEBUG_S2S
		pr_info("%s reset based on slot...\n",__func__);
#endif
		sweep2sleep_reset(false);
#ifdef FULL_FILTER
		return filtering_on();
#else
		return false;
#endif
	}

	if (code == ABS_MT_TRACKING_ID && value == -1) {
		touch_down_called = false;
		touch_x_called = false;
		touch_y_called = false;
		sweep2sleep_reset(false);
#ifdef CONFIG_DEBUG_S2S
		pr_info("%s untouch based on tracking id...\n",__func__);
#endif
#ifdef FULL_FILTER
		return filtering_on();
#else
		return false;
#endif
	}

	if (code == ABS_MT_POSITION_X && touch_down_called) {
		if (get_s2s_switch() && get_s2s_filter_mode() && (filter_coords_status||freeze_touch_area_detected)) {
			touch_x = real_x;
		} else {
			touch_x = value;
		}
		touch_x_called = true;
	}

	if (code == ABS_MT_POSITION_Y && touch_down_called) {
		if (get_s2s_switch() && get_s2s_filter_mode() && (filter_coords_status||freeze_touch_area_detected)) {
			touch_y = real_y;
		} else {
			touch_y = value;
		}
		touch_y_called = true;
	}

	if (touch_x_called && touch_y_called && touch_down_called) {
		int s2s_y_limit = get_s2s_y_limit();
		int s2s_y_above = get_s2s_y_above();
		touch_x_called = false;
		touch_y_called = false;
#ifdef CONFIG_DEBUG_S2S
		pr_info("%s touch x/y gathered.\n",__func__);
#endif
		if (
			// if ... first touch was not registered (filter_coords_status = false) && register only in corner area, and X is outside cordner area,
			(!filter_coords_status && get_s2s_from_corner() && (touch_x < S2S_X_MAX - get_s2s_corner_width() && touch_x > get_s2s_corner_width())) ||
			// or if... y is not in the touch area or x is not in the whole area,
			(get_s2s_filter_mode() && (!filter_coords_status && (touch_y > s2s_y_above || touch_y < s2s_y_limit))) || // TODO still add some Y limit even if in filtered mode, to block to vertical gestures
				// TODO, think about adding vertical gesture BACK!
			(!get_s2s_filter_mode() && (touch_y > s2s_y_above || touch_y < s2s_y_limit)) || 
			(touch_x < get_s2s_width_cutoff()) || (touch_x > S2S_X_MAX - get_s2s_width_cutoff()) ||
			// or if in filtering mode (left right handed separately checked) and X is not in the 40% of the possible width, and this is still the first touch (!filter_coords_status)...
			(get_s2s_filter_mode() == 1 && !filter_coords_status && (touch_x < (S2S_X_MAX * 0.60))) || // right handed, on the left side tapped shouldn't filter...
			(get_s2s_filter_mode() == 2 && !filter_coords_status && (touch_x > (S2S_X_MAX * 0.40))) || // left handed, on the right side tapped...
			(get_s2s_filter_mode() == 3 && !filter_coords_status && (touch_x < (S2S_X_MAX * 0.60) && touch_x > (S2S_X_MAX * 0.40))) // both handed, in the middle region
			)
		{	// cancel now...
			touch_down_called = false;
			freeze_touch_area_detected = false;
			sweep2sleep_reset(true);
		} else {
			// in touch area...
			if (get_s2s_filter_mode()>0 && !filter_coords_status) { // filtered input mode, and first touch point registered without lifting finger...
				first_touch_detection = true; // this is the firt touch so far without lifting finger...
				if (touch_y > get_s2s_y_limit_doubletap()) // only check doubletaps if Y is at the right part
				if (get_s2s_doubletap_mode()>0) {
					unsigned int last_tap_time_diff = jiffies - last_tap_jiffies;
					int delta_x = last_tap_coord_x - touch_x;
					int delta_y = last_tap_coord_y - touch_y;
#ifdef CONFIG_DEBUG_S2S
					pr_info("%d doubletap check, Time: %u X: %d Y: %d\n",last_tap_time_diff,delta_x,delta_y);
#endif
					if (last_tap_time_diff < 150) { // previous first touch time and coordinate comparision to detect double tap...
						if (delta_x < 60 && delta_x > -60 && delta_y < 60 && delta_y > -60) {
							touch_down_called = false;
							sweep2sleep_reset(false); // do not let coordinate freezing yet off, finger is on screen and gesture is still on => (false)
							filter_coords_status = true; // set filtering on...
							if (get_s2s_doubletap_mode()==1) { // power button mode
								// wait a bit before actually emulate pwr button press in the trigger, to avoid wake screen on lockscreen touch
								if (uci_get_sys_property_int_mm("locked", 0, 0, 1)) { // if locked...
									pause_before_pwr_off = true;
								}
								sweep2sleep_pwrtrigger();
							} else { // mode 2
								vib_power = 60;
								schedule_work(&sweep2sleep_vib_work);
								write_uci_out("fp_touch");
							}
							reset_doubletap_tracking();
#ifdef FULL_FILTER
							return filtering_on();
#else
							return false; // break out here, don't filter
#endif
						}
					} else {
						last_tap_starts_in_dt_area = true;
					}
					store_doubletap_touch();
					if (get_s2s_longtap_switch()) {
						vib_power = 70;
						schedule_work(&sweep2sleep_vib_work);
						schedule_work(&sweep2sleep_longtap_count_work);
					}
				}
			}
			// in touch area, set filter status True...
			if (!filter_coords_status) {
				frozen_x = touch_x;
				frozen_y = touch_y;
				frozen_rand = 0;
			}
			freeze_touch_area_detected = false;
			filter_coords_status = true;

			if (get_s2s_switch()==0 || (get_s2s_filter_mode() && get_s2s_doubletap_mode() && get_s2s_longtap_switch()==2)) { 
				// swipe is disabled with longtap / dt only mode (switch == 2)
			} else
			{
				queue_work_on(0, s2s_input_wq, &s2s_input_work);
			}
		}
	}

#ifdef FULL_FILTER
	// filter if filter mode active and in sweep touch area, and...
	// ...this is not right the first touch detection and so the Y coordinate 
	//    which should NOT be filtered, or it will cause touch positioning issues...
	return filtering_on();
#else
	return false; // touch driver should use s2s_freeze_cords, here we let all event through
#endif
}
static bool s2s_input_filter(struct input_handle *handle, unsigned int type,
				unsigned int code, int value) {
	bool ret = __s2s_input_filter(handle,type,code,value);
#ifdef CONFIG_DEBUG_S2S
	pr_info("%s [FILTER] fresult=%s , type: %d code: %d value: %d\n",__func__,ret?"TRUE":"FALSE",type,code,value);
#endif
	return ret;

}

static void s2s_input_event(struct input_handle *handle, unsigned int type,
                                unsigned int code, int value) {
}

static void uci_sys_listener(void) {
	if (!!uci_get_sys_property_int_mm("locked", 0, 0, 1)==false) {
		// unlocking also should indicate gesture can be done again...
		screen_off_after_gesture = true;
	}

}
static void uci_user_listener(void) {
	s2s_onoff = uci_get_user_property_int_mm("sweep2sleep_switch", s2s_onoff, 0, 1);
	s2s_switch = uci_get_user_property_int_mm("sweep2sleep_mode", s2s_switch, 0, 3);
	s2s_filter_mode = uci_get_user_property_int_mm("sweep2sleep_filter_mode", s2s_filter_mode, 0, 3);
	s2s_doubletap_mode = uci_get_user_property_int_mm("sweep2sleep_doubletap_mode", s2s_doubletap_mode, 0, 2);
	s2s_longtap_switch = uci_get_user_property_int_mm("sweep2sleep_longtap_switch", s2s_longtap_switch, 0, 2);
	s2s_longtap_min_holdtime = uci_get_user_property_int_mm("sweep2sleep_longtap_min_holdtime", s2s_longtap_min_holdtime, 60, 300); // 0.2 - 1 sec
	s2s_height = uci_get_user_property_int_mm("sweep2sleep_height", s2s_height, 50, 350);
	s2s_doubletap_height = uci_get_user_property_int_mm("sweep2sleep_doubletap_height", s2s_doubletap_height, 50, 350);
	s2s_height_above = uci_get_user_property_int_mm("sweep2sleep_height_above", s2s_height_above, 0, 150);
	s2s_width = uci_get_user_property_int_mm("sweep2sleep_width", s2s_width, 0, 150);
	s2s_from_corner = uci_get_user_property_int_mm("sweep2sleep_from_corner", s2s_from_corner, 0, 1);
	s2s_width_cutoff = uci_get_user_property_int_mm("sweep2sleep_width_cutoff", 60, 0, 120);
	s2s_corner_width = uci_get_user_property_int_mm("sweep2sleep_corner_width", 150, 100, 350);
	s2s_continuous_vib = uci_get_user_property_int_mm("sweep2sleep_continuous_vib", 0, 0, 1);
	s2s_wait_for_finger_leave = uci_get_user_property_int_mm("sweep2sleep_wait_for_finger_leave", s2s_wait_for_finger_leave, 0, 1);
	s2s_reenable_after_screen_off = uci_get_user_property_int_mm("sweep2sleep_reenable_after_screen_off", s2s_reenable_after_screen_off, 0, 1);
}

static void ntf_listener(char* event, int num_param, char* str_param) {
        if (strcmp(event,NTF_EVENT_CHARGE_LEVEL) && strcmp(event, NTF_EVENT_INPUT)) {
                pr_info("%s ifilter ntf listener event %s %d %s\n",__func__,event,num_param,str_param);
        }

        if (!strcmp(event,NTF_EVENT_SLEEP)) {
		// screen off also should indicate gesture can be done again...
		screen_off_after_gesture = true;
		finger_counter = 0;
        }
}

static int input_dev_filter(struct input_dev *dev) {
	pr_info("%s sweep2sleep device filter check. Device: %s\n",__func__,dev->name);
	if (strstr(dev->name, "qpnp_pon")) {
		sweep2sleep_pwrdev = dev;
	}
	if (strstr(dev->name, "synaptics,s3320")) {
		return 0;
	} else
	if (strstr(dev->name, "fts")) {
		return 0;
	} else
	if (strstr(dev->name, "sec_touchscreen")) {
		return 0;
	} else {
		return 1;
	}
}

static int s2s_input_connect(struct input_handler *handler,
				struct input_dev *dev, const struct input_device_id *id) {
	struct input_handle *handle;
	int error;

	if (input_dev_filter(dev))
		return -ENODEV;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "s2s";

	error = input_register_handle(handle);

	error = input_open_device(handle);

	return 0;

}

static void s2s_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id s2s_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler s2s_input_handler = {
	.filter         = s2s_input_filter,
	.event		= s2s_input_event,
	.connect	= s2s_input_connect,
	.disconnect	= s2s_input_disconnect,
	.name		= "s2s_inputreq",
	.id_table	= s2s_ids,
};
#if 0
static ssize_t sweep2sleep_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", s2s_switch);
}

static ssize_t sweep2sleep_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned long input;

	ret = kstrtoul(buf, 0, &input);
	if (ret < 0)
		return ret;

	if (input < 0 || input > 3)
		input = 0;

	s2s_switch = input;
	
	return count;
}

static DEVICE_ATTR(sweep2sleep, (S_IWUSR|S_IRUGO),
	sweep2sleep_show, sweep2sleep_dump);
#endif

static struct kobject *sweep2sleep_kobj;

static int __init sweep2sleep_init(void)
{
	int rc = 0;

/*	sweep2sleep_pwrdev = input_allocate_device();
	if (!sweep2sleep_pwrdev) {
		pr_err("Failed to allocate sweep2sleep_pwrdev\n");
		goto err_alloc_dev;
	}

	input_set_capability(sweep2sleep_pwrdev, EV_KEY, KEY_POWER);

	sweep2sleep_pwrdev->name = "s2s_pwrkey";
	sweep2sleep_pwrdev->phys = "s2s_pwrkey/input0";

	rc = input_register_device(sweep2sleep_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err=%d\n", __func__, rc);
		goto err_input_dev;
	}
*/
	s2s_input_wq = create_workqueue("s2s_iwq");
	if (!s2s_input_wq) {
		pr_err("%s: Failed to create workqueue\n", __func__);
		return -EFAULT;
	}
	INIT_WORK(&s2s_input_work, s2s_input_callback);

	rc = input_register_handler(&s2s_input_handler);
	if (rc)
		pr_err("%s: Failed to register s2s_input_handler\n", __func__);

	sweep2sleep_kobj = kobject_create_and_add("sweep2sleep", NULL) ;
	if (sweep2sleep_kobj == NULL) {
		pr_warn("%s: sweep2sleep_kobj failed\n", __func__);
	}
#if 0
	rc = sysfs_create_file(sweep2sleep_kobj, &dev_attr_sweep2sleep.attr);
	if (rc)
		pr_err("%s: sysfs_create_file failed for sweep2sleep\n", __func__);
#endif

        uci_add_user_listener(uci_user_listener);
        uci_add_sys_listener(uci_sys_listener);
        ntf_add_listener(ntf_listener);

//err_input_dev:
//	input_free_device(sweep2sleep_pwrdev);

//err_alloc_dev:
	pr_info("%s done\n", __func__);
	return 0;
}

static void __exit sweep2sleep_exit(void)
{
	kobject_del(sweep2sleep_kobj);
	input_unregister_handler(&s2s_input_handler);
	destroy_workqueue(s2s_input_wq);
//	input_unregister_device(sweep2sleep_pwrdev);
//	input_free_device(sweep2sleep_pwrdev);

	return;
}

module_init(sweep2sleep_init);
module_exit(sweep2sleep_exit);
