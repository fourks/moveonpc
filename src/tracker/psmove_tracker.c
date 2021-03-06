/**
 * PS Move API - An interface for the PS Move Motion Controller
 * Copyright (c) 2012 Thomas Perl <m@thp.io>
 * Copyright (c) 2012 Benjamin Venditt <benjamin.venditti@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 **/

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

#include "opencv2/core/core_c.h"
#include "opencv2/imgproc/imgproc_c.h"

#include "psmove_tracker.h"
#include "../psmove_private.h"

#include "camera_control.h"
#include "tracker_helpers.h"
#include "tracked_controller.h"
#include "tracked_color.h"
#include "tracker_trace.h"

#ifdef __linux
#  include "platform/psmove_linuxsupport.h"
#endif

#define DIMMING_FACTOR 1  			// LED color dimming for use in high exposure settings
#define PRINT_DEBUG_STATS			// shall graphical statistics be printed to the image
//#define DEBUG_WINDOWS 			// shall additional windows be shown
#define GOOD_EXPOSURE 2051			// a very low exposure that was found to be good for tracking
#define ROIS 4                   	// the number of levels of regions of interest (roi)
#define BLINKS 4                 	// number of diff images to create during calibration
#define BLINK_DELAY 50             	// number of milliseconds to wait between a blink
#define CALIB_MIN_SIZE 50		 	// minimum size of the estimated glowing sphere during calibration process (in pixel)
#define CALIB_SIZE_STD 10	     	// maximum standard deviation (in %) of the glowing spheres found during calibration process
#define CALIB_MAX_DIST 30		 	// maximum displacement of the separate found blobs
#define COLOR_FILTER_RANGE_H 12		// +- H-Range of the hsv-colorfilter
#define COLOR_FILTER_RANGE_S 85		// +- s-Range of the hsv-colorfilter
#define COLOR_FILTER_RANGE_V 85		// +- v-Range of the hsv-colorfilter
#define CAMERA_FOCAL_LENGTH 28.3	// focal lenght constant of the ps-eye camera in (degrees)
#define CAMERA_PIXEL_HEIGHT 5		// pixel height constant of the ps-eye camera in (�m)
#define PS_MOVE_DIAMETER 47			// orb diameter constant of the ps-move controller in (mm)
/* Thresholds */
#define ROI_ADJUST_FPS_T 160		// the minimum fps to be reached, if a better roi-center adjusment is to be perfomred
#define CALIBRATION_DIFF_T 20		// during calibration, all grey values in the diff image below this value are set to black
// if tracker thresholds not met, sphere is deemed not to be found
#define TRACKER_QUALITY_T1 0.3		// minimum ratio of number of pixels in blob vs pixel of estimated circle.
#define TRACKER_QUALITY_T2 0.7		// maximum allowed change of the radius in percent, compared to the last estimated radius
#define TRACKER_QUALITY_T3 4		// minimum radius
#define TRACKER_ADAPTIVE_XY 1		// specifies to use a adaptive x/y smoothing
#define TRACKER_ADAPTIVE_Z 1		// specifies to use a adaptive z smoothing
#define COLOR_ADAPTION_QUALITY 35 	// maximal distance (calculated by 'psmove_tracker_hsvcolor_diff') between the first estimated color and the newly estimated
#define COLOR_UPDATE_RATE 1	 	 	// every x seconds adapt to the color, 0 means no adaption
// if color thresholds not met, color is not adapted
#define COLOR_UPDATE_QUALITY_T1 0.8	// minimum ratio of number of pixels in blob vs pixel of estimated circle.
#define COLOR_UPDATE_QUALITY_T2 0.2	// maximum allowed change of the radius in percent, compared to the last estimated radius
#define COLOR_UPDATE_QUALITY_T3 6	// minimum radius
#ifdef WIN32
#define PSEYE_BACKUP_FILE "PSEye_backup_win.ini"
#else
#define PSEYE_BACKUP_FILE "PSEye_backup_v4l.ini"
#endif

#define INTRINSICS_XML "intrinsics.xml"
#define DISTORTION_XML "distortion.xml"

struct _PSMoveTracker {
	CameraControl* cc;
	IplImage* frame; // the current frame of the camera
	int exposure; // the exposure to use
	IplImage* roiI[ROIS]; // array of images for each level of roi (colored)
	IplImage* roiM[ROIS]; // array of images for each level of roi (greyscale)
	IplConvKernel* kCalib; // kernel used for morphological operations during calibration
	CvScalar rHSV; // the range of the color filter
	TrackedController* controllers; // a pointer to a linked list of connected controllers
	PSMoveTrackingColor* available_colors; // a pointer to a linked list of available tracking colors
	CvMemStorage* storage; // use to store the result of cvFindContour and cvHughCircles
        long duration; // duration of tracking operation, in ms

	// internal variables
	float cam_focal_length; // in (mm)
	float cam_pixel_height; // in (�m)
	float ps_move_diameter; // in (mm)
	float user_factor_dist; // user defined factor used in distance calulation

	int tracker_adaptive_xy; // should adaptive x/y-smoothing be used
	int tracker_adaptive_z; // should adaptive z-smoothing be used

	int calibration_t; // the threshold used during calibration to create the diff image

	// if one is not met, the tracker is regarded as not found (although something has been found)
	float tracker_t1; // quality threshold1 for the tracker
	float tracker_t2; // quality threshold2 for the tracker
	float tracker_t3; // quality threshold3 for the tracker

	float adapt_t1; // quality threshold for the color adaption to discard its estimation again

	// if one is not met, the color will not use adaptive color estimation
	float color_t1; // quality threshold3 for the color adaption
	float color_t2; // quality threshold3 for the color adaption
	float color_t3; // quality threshold3 for the color adaption
	float color_update_rate; // how often shall the color be adapted (in seconds), 0 means never

	// internal variables (debug)
	float debug_fps; // the current FPS achieved by "psmove_tracker_update"

};

// -------- START: internal functions only

/**
 * XXX - this function is currently not used
 * 
 * Adapts the cameras exposure to the current lighting conditions
 * This function will adapt to the most suitable exposure, it will start
 * with "expMin" and increases step by step to "expMax" until it reaches "lumMin" or "expMax"
 *
 * tracker - A valid PSMoveTracker * instance
 * limMin  - Minimal luminance to reach
 * expMin  - Minimal exposure to test
 * expMax  - Maximal exposure to test
 *
 * Returns: the most suitable exposure within range
 **/
int psmove_tracker_adapt_to_light(PSMoveTracker *tracker, int lumMin, int expMin, int expMax);


/**
 * Wait for a given time for a frame from the tracker
 *
 * tracker - A valid PSMoveTracker * instance
 * frame - A pointer to an IplImage * to store the frame
 * delay - The delay to wait for the frame
 **/
void
psmove_tracker_wait_for_frame(PSMoveTracker *tracker, IplImage **frame, int delay);

/**
 * This function switches the sphere of the given PSMove on to the given color and takes
 * a picture via the given capture. Then it switches it of and takes a picture again. A difference image
 * is calculated from these two images. It stores the image of the lit sphere and
 * of the diff-image in the passed parameter "on" and "diff". Before taking
 * a picture it waits for the specified delay (in microseconds).
 *
 * tracker - the tracker that contains the camera control
 * move    - the PSMove controller to use
 * r,g,b   - the RGB color to use to lit the sphere
 * on	   - the pre-allocated image to store the captured image when the sphere is lit
 * diff    - the pre-allocated image to store the calculated diff-image
 * delay   - the time to wait before taking a picture (in microseconds)
 **/
void psmove_tracker_get_diff(PSMoveTracker* tracker, PSMove* move, int r, int g, int b, IplImage* on, IplImage* diff, int delay);

/**
 * This function seths the rectangle of the ROI and assures that the itis always within the bounds
 * of the camera image.
 *
 * tracker          - A valid PSMoveTracker * instance
 * tc         - The TrackableController containing the roi to check & fix
 * roi_x	  - the x-part of the coordinate of the roi
 * roi_y	  - the y-part of the coordinate of the roi
 * roi_width  - the width of the roi
 * roi_height - the height of the roi
 * cam_width  - the width of the camera image
 * cam_height - the height of the camera image
 **/
void psmove_tracker_set_roi(PSMoveTracker* tracker, TrackedController* tc, int roi_x, int roi_y, int roi_width, int roi_height);

/**
 * This function prepares the linked list of suitable colors, that can be used for tracking.
 */
void psmove_tracker_prepare_colors(PSMoveTracker* tracker);

/**
 * This function is just the internal implementation of "psmove_tracker_update"
 */
int psmove_tracker_update_controller(PSMoveTracker* tracker, TrackedController* tc);

/**
 * This draws tracking statistics into the current camera image. This is only used internally.
 *
 * tracker - the Tracker to use
 */
void psmove_tracker_draw_tracking_stats(PSMoveTracker* tracker);

/*
 *  This finds the biggest contour within the given image.
 *
 *  img  		- (in) 	the binary image to search for contours
 *  stor 		- (out) a storage that can be used to save the result of this function
 *  resContour 	- (out) points to the biggest contour found within the image
 *  resSize 	- (out)	the size of that contour in px�
 */
void psmove_tracker_biggest_contour(IplImage* img, CvMemStorage* stor, CvSeq** resContour, float* resSize);

/*
 * This calculates the distance of the orb of the controller.
 *
 * tracker 			 - (in) the PSMoveTracker to use (used to read variables)
 * blob_diameter - (in) the diameter size of the orb in pixels
 *
 * Returns: The distance between the orb and the camera in (mm).
 */
float psmove_tracker_calculate_distance(PSMoveTracker* tracker, float blob_diameter);

/*
 * This returns a subjective distance between the first estimated (during calibration process) color and the currently estimated color.
 * Subjective, because it takes the different color components not equally into account.
 *    Result calculates like: abs(c1.h-c2.h) + abs(c1.s-c2.s)*0.5 + abs(c1.v-c2.v)*0.5
 *
 * tc - The controller whose first/current color estimation distance should be calculated.
 *
 * Returns: a subjective distance
 */
float psmove_tracker_hsvcolor_diff(TrackedController* tc);

/*
 * This will estimate the position and the radius of the orb.
 * It will calcualte the radius by findin the two most distant points
 * in the contour. And its by choosing the mid point of those two.
 *
 * cont 	- (in) 	The contour representing the orb.
 * x            - (out) The X coordinate of the center.
 * y            - (out) The Y coordinate of the center.
 * radius	- (out) The radius of the contour that is calculated here.
 */
void
psmove_tracker_estimate_circle_from_contour(CvSeq* cont, float *x, float *y, float* radius);

/*
 * This function return a optimal ROI center point for a given Tracked controller.
 * On very fast movements, it may happen that the orb is visible in the ROI, but resides
 * at its border. This function will simply look for the biggest blob in the ROI and return a
 * point so that that blob would be in the center of the ROI.
 *
 * tc - (in) The controller whose ROI centerpoint should be adjusted.
 * tracker  - (in) The PSMoveTracker to use.
 * center - (out) The better center point for the current ROI
 *
 * Returns: nonzero if a new point was found, zero otherwise
 */
int
psmove_tracker_center_roi_on_controller(TrackedController* tc, PSMoveTracker* tracker, CvPoint *center);


/*
 * This function reads old calibration color values and tries to track the controller with that color.
 * if it works, the function returns 1, 0 otherwise.
 * Can help to speed up calibration process on application startup.
 *
 * tracker     - (in) A valid PSMoveTracker
 * move  - (in) A valid PSMove controller
 * r,g,b - (in) The color the PSMove controller's sphere will be lit.
 */

int psmove_tracker_old_color_is_tracked(PSMoveTracker* tracker, PSMove* move, int r, int g, int b);

// -------- END: internal functions only

PSMoveTracker *psmove_tracker_new() {
    int camera = 0;

#if defined(__linux) && defined(PSMOVE_USE_PSEYE)
    /**
     * On Linux, we might have multiple cameras (e.g. most laptops have
     * built-in cameras), so we try looking for the one that is handled
     * by the PSEye driver.
     **/
    camera = linux_find_pseye();
    if (camera == -1) {
        /* Could not find the PSEye - fallback to first camera */
        camera = 0;
    }
#endif

    char *camera_env = getenv(PSMOVE_TRACKER_CAMERA_ENV);
    if (camera_env) {
        char *end;
        long camera_env_id = strtol(camera_env, &end, 10);
        if (*end == '\0' && *camera_env != '\0') {
            camera = (int)camera_env_id;
#ifdef PSMOVE_DEBUG
            fprintf(stderr, "[PSMOVE] Using camera %d (%s is set)\n",
                    camera, PSMOVE_TRACKER_CAMERA_ENV);
#endif
        }
    }

    return psmove_tracker_new_with_camera(camera);
}

PSMoveTracker *
psmove_tracker_new_with_camera(int camera) {
	PSMoveTracker* tracker = (PSMoveTracker*) calloc(1, sizeof(PSMoveTracker));
	tracker->rHSV = cvScalar(COLOR_FILTER_RANGE_H, COLOR_FILTER_RANGE_S, COLOR_FILTER_RANGE_V, 0);
	tracker->storage = cvCreateMemStorage(0);

	tracker->cam_focal_length = CAMERA_FOCAL_LENGTH;
	tracker->cam_pixel_height = CAMERA_PIXEL_HEIGHT;
	tracker->ps_move_diameter = PS_MOVE_DIAMETER;
	tracker->user_factor_dist = 1.05;

	tracker->calibration_t = CALIBRATION_DIFF_T;
	tracker->tracker_t1 = TRACKER_QUALITY_T1;
	tracker->tracker_t2 = TRACKER_QUALITY_T2;
	tracker->tracker_t3 = TRACKER_QUALITY_T3;
	tracker->tracker_adaptive_xy = TRACKER_ADAPTIVE_XY;
	tracker->tracker_adaptive_z = TRACKER_ADAPTIVE_Z;
	tracker->adapt_t1 = COLOR_ADAPTION_QUALITY;
	tracker->color_t1 = COLOR_UPDATE_QUALITY_T1;
	tracker->color_t2 = COLOR_UPDATE_QUALITY_T2;
	tracker->color_t3 = COLOR_UPDATE_QUALITY_T3;
	tracker->color_update_rate = COLOR_UPDATE_RATE;
	
	// prepare available colors for tracking
	psmove_tracker_prepare_colors(tracker);

	// start the video capture device for tracking
	tracker->cc = camera_control_new(camera);

        char *intrinsics_xml = psmove_util_get_file_path(INTRINSICS_XML);
        char *distortion_xml = psmove_util_get_file_path(DISTORTION_XML);
	camera_control_read_calibration(tracker->cc, intrinsics_xml, distortion_xml);
        free(intrinsics_xml);
        free(distortion_xml);

	// backup the systems settings, if not already backuped
	char *filename = psmove_util_get_file_path(PSEYE_BACKUP_FILE);
	if (!th_file_exists(filename)) {
            camera_control_backup_system_settings(tracker->cc, filename);
    }
	free(filename);
	
	// use static exposure
	tracker->exposure = GOOD_EXPOSURE;
	// use dynamic exposure (This function would enable a lighting condition specific exposure.)
	//tracker->exposure = psmove_tracker_adapt_to_light(tracker, 25, 2051, 4051);
	camera_control_set_parameters(tracker->cc, 0, 0, 0, tracker->exposure, 0, 0xffff, 0xffff, 0xffff, -1, -1);

	// just query a frame so that we know the camera works
	IplImage* frame = NULL;
	while (!frame) {
		frame = camera_control_query_frame(tracker->cc);
	}

	// prepare ROI data structures
	
	/* The biggest roi is 1/4 of the whole image (a rectangle) */
	int w = frame->width/2;
	int h = frame->height/2;
	
	int i;
	for (i = 0; i < ROIS; i++) {
		tracker->roiI[i] = cvCreateImage(cvSize(w,h), frame->depth, 3);
		tracker->roiM[i] = cvCreateImage(cvSize(w,h), frame->depth, 1);
		
		/* Smaller rois are always square, and 70% of the previous level */
		h = w = MIN(w,h) * 0.7f;
	}

	// prepare structure used for erode and dilate in calibration process
	int ks = 5; // Kernel Size
	int kc = (ks + 1) / 2; // Kernel Center
	tracker->kCalib = cvCreateStructuringElementEx(ks, ks, kc, kc, CV_SHAPE_RECT, NULL);
	return tracker;
}

enum PSMoveTracker_Status psmove_tracker_enable(PSMoveTracker *tracker, PSMove *move) {
	// check if there is a free color, return on error immediately
	PSMoveTrackingColor* color = tracker->available_colors;
	while (color && color->is_used) {
		color = color->next;
	}

	if (!color)
		return Tracker_CALIBRATION_ERROR;

	// looks like there is a free color -> try to calibrate/enable the controller with that color
	unsigned char r = color->r;
	unsigned char g = color->g;
	unsigned char b = color->b;

	return psmove_tracker_enable_with_color(tracker, move, r, g, b);
}

int psmove_tracker_old_color_is_tracked(PSMoveTracker* tracker, PSMove* move, int r, int g, int b) {	
	int result = 0;
	// times to try to track the controller
	int nTimes = 3;
	// time to wait between each try 
	int delay = 100;

	TrackedController* tc = tracked_controller_create();
	tc->dColor = cvScalar(b, g, r, 0);

	if (tracked_controller_load_color(tc)) {
		result = 1;
		int i;
		for (i = 0; i < nTimes; i++) {
			// sleep a little befor checking the next image
			int d;
			for (d = 0; d < delay / 10; d++) {
				usleep(1000 * 10);
				psmove_set_leds(move,
                                        r * DIMMING_FACTOR,
                                        g * DIMMING_FACTOR,
                                        b * DIMMING_FACTOR);
				psmove_update_leds(move);
				psmove_tracker_update_image(tracker);
			}

			// try to track the contorller
			psmove_tracker_update_controller(tracker, tc);

			// if the quality is higher than 83% and the blobs radius bigger than 8px
			// TODO: move 0.83 and 8 as constants out
			result = result && tc->q1 > 0.83 && tc->q3 > 8;
		}
	}
	tracked_controller_release(&tc, 1);
	return result;
}

enum PSMoveTracker_Status
psmove_tracker_enable_with_color(PSMoveTracker *tracker, PSMove *move,
        unsigned char r, unsigned char g, unsigned char b)
{
	// check if the controller is already enabled!
	if (tracked_controller_find(tracker->controllers, move))
		return Tracker_CALIBRATED;

	// check if the color is already in use, if not, mark it as used, return with a error if it is already used
	PSMoveTrackingColor* tracked_color = tracked_color_find(tracker->available_colors, r, g, b);
	if (!tracked_color || tracked_color->is_used)
		return Tracker_CALIBRATION_ERROR;

	// try to track the controller with the old color, if it works we are done
	if (psmove_tracker_old_color_is_tracked(tracker, move, r, g, b)) {
		TrackedController* itm = tracked_controller_insert(&tracker->controllers, move);
		itm->dColor = cvScalar(b, g, r, 0);
		tracked_controller_load_color(itm);
		tracked_color->is_used = 1;
		return Tracker_CALIBRATED;
	}

	// clear the calibration html trace
	psmove_html_trace_clear();

	IplImage* frame = camera_control_query_frame(tracker->cc);
	// check if the frame retrieved, is valid
	assert(frame!=NULL);
	IplImage* images[BLINKS]; // array of images saved during calibration for estimation of sphere color
	IplImage* diffs[BLINKS]; // array of masks saved during calibration for estimation of sphere color
	double sizes[BLINKS]; // array of blob sizes saved during calibration for estimation of sphere color
	int i;
	for (i = 0; i < BLINKS; i++) {
		images[i] = cvCreateImage(cvGetSize(frame), frame->depth, 3);
		diffs[i] = cvCreateImage(cvGetSize(frame), frame->depth, 1);
	}
	// DEBUG log the assigned color
	CvScalar assignedColor = cvScalar(b, g, r, 0);
	psmove_html_trace_put_color_var("assignedColor", assignedColor);

	// for each blink
	for (i = 0; i < BLINKS; i++) {
		// create a diff image
		psmove_tracker_get_diff(tracker, move, r, g, b, images[i], diffs[i], BLINK_DELAY);

		// DEBUG log the diff image and the image with the lit sphere
		psmove_html_trace_image_at(images[i], i, "originals");
		psmove_html_trace_image_at(diffs[i], i, "rawdiffs");

		// threshold it to reduce image noise
		cvThreshold(diffs[i], diffs[i], tracker->calibration_t, 0xFF /* white */, CV_THRESH_BINARY);

		// DEBUG log the thresholded diff image
		psmove_html_trace_image_at(diffs[i], i, "threshdiffs");

		// use morphological operations to further remove noise
		cvErode(diffs[i], diffs[i], tracker->kCalib, 1);
		cvDilate(diffs[i], diffs[i], tracker->kCalib, 1);

		// DEBUG log the even more cleaned up diff-image
		psmove_html_trace_image_at(diffs[i], i, "erodediffs");
	}
    // create the final mask
	IplImage* mask = diffs[0];
	
	// put the diff images together to get hopefully only one intersection region
	// the region at which the controllers sphere resides.
	for (i = 1; i < BLINKS; i++) {
		cvAnd(mask, diffs[i], mask, NULL);
	}

	// find the biggest contour
	float sizeBest = 0;
	CvSeq* contourBest = NULL;
	psmove_tracker_biggest_contour(diffs[0], tracker->storage, &contourBest, &sizeBest);

	// blank out the image and repaint the blob where the sphere is deemed to be
	cvSet(mask, th_black, NULL);
	if (contourBest)
		cvDrawContours(mask, contourBest, th_white, th_white, -1, CV_FILLED, 8, cvPoint(0, 0));

	cvClearMemStorage(tracker->storage);

	// DEBUG log the final diff-image used for color estimation
	psmove_html_trace_image_at(mask, 0, "finaldiff");

	// CHECK if the blob contains a minimum number of pixels
	if (cvCountNonZero(mask) < CALIB_MIN_SIZE) {
		psmove_html_trace_put_log_entry("WARNING", "The final mask my not be representative for color estimation.");
	}

	// calculate the avg color
	CvScalar color = cvAvg(images[0], mask);
	CvScalar hsv_assigned = th_brg2hsv(assignedColor);  // HSV color sent to controller
	CvScalar hsv_color = th_brg2hsv(color);				// HSV color seen by camera
	
	psmove_html_trace_put_color_var("estimatedColor", color);
	psmove_html_trace_put_int_var("estimated_hue", hsv_color.val[0]);
	psmove_html_trace_put_int_var("assigned_hue", hsv_assigned.val[0]);
	psmove_html_trace_put_int_var("allowed_hue_difference", tracker->rHSV.val[0]);

	// CHECK if the hue of the estimated and the assigned colors differ more than allowed in the color-filter range.s
	if (abs(hsv_assigned.val[0] - hsv_color.val[0]) > tracker->rHSV.val[0]) {
		psmove_html_trace_put_log_entry("WARNING", "The estimated color seems not to be similar to the color it should be.");
	}

	int valid_countours = 0;
	// calculate upper & lower bounds for the color filter
	CvScalar min, max;
	th_minus(hsv_color.val, tracker->rHSV.val, min.val, 3);
	th_plus(hsv_color.val, tracker->rHSV.val, max.val, 3);
	// for each image (where the sphere was lit)

	CvPoint firstPosition;
	for (i = 0; i < BLINKS; i++) {
		// convert to HSV
		cvCvtColor(images[i], images[i], CV_BGR2HSV);
		// apply color filter
		cvInRangeS(images[i], min, max, mask);

		// use morphological operations to further remove noise
		cvErode(mask, mask, tracker->kCalib, 1);
		cvDilate(mask, mask, tracker->kCalib, 1);

		// DEBUG log the color filter and
		psmove_html_trace_image_at(mask, i, "filtered");

		// find the biggest contour in the image and save its location and size
		psmove_tracker_biggest_contour(mask, tracker->storage, &contourBest, &sizeBest);
		sizes[i] = 0;
		float dist = FLT_MAX;
		CvRect bBox;
		if (contourBest) {
			bBox = cvBoundingRect(contourBest, 0);
			if (i == 0) {
				firstPosition = cvPoint(bBox.x, bBox.y);
			}
			dist = sqrt(pow(firstPosition.x - bBox.x, 2) + pow(firstPosition.y - bBox.y, 2));
			sizes[i] = sizeBest;
		}

		// CHECK for errors (no contour, more than one contour, or contour too small)
		if (!contourBest) {
			psmove_html_trace_array_item_at(i, "contours", "no contour");
		} else if (sizes[i] <= CALIB_MIN_SIZE) {
			psmove_html_trace_array_item_at(i, "contours", "too small");
		} else if (dist >= CALIB_MAX_DIST) {
			psmove_html_trace_array_item_at(i, "contours", "too far apart");
		} else {
			psmove_html_trace_array_item_at(i, "contours", "OK");
			// all checks passed, increase the number of valid contours
			valid_countours++;
		}
		cvClearMemStorage(tracker->storage);

	}

	// clean up all temporary images
	for (i = 0; i < BLINKS; i++) {
		cvReleaseImage(&images[i]);
		cvReleaseImage(&diffs[i]);
		mask = NULL;
	}

	int has_calibration_errors = 0;
	// CHECK if sphere was found in each BLINK image
	if (valid_countours < BLINKS) {
		psmove_html_trace_put_log_entry("ERROR", "The sphere could not be found in all images.");
		has_calibration_errors++;
	}

	// CHECK if the size of the found contours are similar
	double stdSizes = sqrt(th_var(sizes, BLINKS));
	if (stdSizes >= (th_avg(sizes, BLINKS) / 100.0 * CALIB_SIZE_STD)) {
		psmove_html_trace_put_log_entry("ERROR", "The spheres found differ too much in size.");
		has_calibration_errors++;
	}

	if (has_calibration_errors)
		return Tracker_CALIBRATION_ERROR;

	// insert to list of tracked controllers
	TrackedController* itm = tracked_controller_insert(&tracker->controllers, move);
	// set current color
	itm->dColor = cvScalar(b, g, r, 0);
	// set first estimated color
	itm->eFColor = color;
	itm->eFColorHSV = hsv_color;
	// set current estimated color
	itm->eColor = color;
	itm->eColorHSV = hsv_color;

	// set, that this color is in use
	tracked_color->is_used = 1;

	tracked_controller_save_colors(tracker->controllers);
	return Tracker_CALIBRATED;
}

int psmove_tracker_get_color(PSMoveTracker *tracker, PSMove *move, unsigned char *r, unsigned char *g, unsigned char *b) {
    psmove_return_val_if_fail(tracker != NULL, 0);
	psmove_return_val_if_fail(move != NULL, 0);

	TrackedController* tc = tracked_controller_find(tracker->controllers, move);
	psmove_return_val_if_fail(tc != NULL, 0);

	*r = tc->dColor.val[2] * DIMMING_FACTOR;
	*g = tc->dColor.val[1] * DIMMING_FACTOR;
	*b = tc->dColor.val[0] * DIMMING_FACTOR;
	return 1;
}

void psmove_tracker_disable(PSMoveTracker *tracker, PSMove *move) {
    psmove_return_if_fail(tracker != NULL);
    psmove_return_if_fail(move != NULL);

	TrackedController* tc = tracked_controller_find(tracker->controllers, move);
	PSMoveTrackingColor* color = tracked_color_find(tracker->available_colors, tc->dColor.val[2], tc->dColor.val[1], tc->dColor.val[0]);
	if (tc) {
		tracked_controller_remove(&tracker->controllers, move);
		tracked_controller_release(&tc, 0);
	}
	
	if (color)
		color->is_used = 0;
}

enum PSMoveTracker_Status psmove_tracker_get_status(PSMoveTracker *tracker, PSMove *move) {
	TrackedController* tc = tracked_controller_find(tracker->controllers, move);
	if (tc) {
		if (tc->is_tracked)
			return Tracker_TRACKING;
		else
			return Tracker_CALIBRATED;
	} else {
		return Tracker_NOT_CALIBRATED;
	}
}

void*
psmove_tracker_get_image(PSMoveTracker *tracker) {
	return tracker->frame;
}

void psmove_tracker_update_image(PSMoveTracker *tracker) {
	tracker->frame = camera_control_query_frame(tracker->cc);
}

int
psmove_tracker_update_controller(PSMoveTracker *tracker, TrackedController* tc)
{
        float x, y;
	int i = 0;
	int sphere_found = 0;

	// calculate upper & lower bounds for the color filter
	CvScalar min, max;
	th_minus(tc->eColorHSV.val, tracker->rHSV.val, min.val, 3);
	th_plus(tc->eColorHSV.val, tracker->rHSV.val, max.val, 3);

	// this is the tracking algorithm
	while (1) {
		// get pointers to data structures for the given ROI-Level
		IplImage *roi_i = tracker->roiI[tc->roi_level];
		IplImage *roi_m = tracker->roiM[tc->roi_level];

		// adjust the ROI, so that the blob is fully visible, but only if we have a reasonable FPS
		if (tracker->debug_fps > ROI_ADJUST_FPS_T) {
			// TODO: check for validity differently
			CvPoint nRoiCenter;
                        if (psmove_tracker_center_roi_on_controller(tc, tracker, &nRoiCenter)) {
				psmove_tracker_set_roi(tracker, tc, nRoiCenter.x, nRoiCenter.y, roi_i->width, roi_i->height);
			}
		}

		// apply the ROI
		cvSetImageROI(tracker->frame, cvRect(tc->roi_x, tc->roi_y, roi_i->width, roi_i->height));
		cvCvtColor(tracker->frame, roi_i, CV_BGR2HSV);

		// apply color filter
		cvInRangeS(roi_i, min, max, roi_m);

		#ifdef DEBUG_WINDOWS
			if (!tc->next){
				cvShowImage("binary:0", roi_m);
				cvShowImage("hsv:0", roi_i);
			}
			else{
				cvShowImage("binary:1", roi_m);
				cvShowImage("hsv:1", roi_i);
			}
		#endif

		// find the biggest contour in the image
		float sizeBest = 0;
		CvSeq* contourBest = NULL;
		psmove_tracker_biggest_contour(roi_m, tracker->storage, &contourBest, &sizeBest);

		if (contourBest) {
			CvMoments mu; // ImageMoments are use to calculate the center of mass of the blob
			CvRect br = cvBoundingRect(contourBest, 0);

			// restore the biggest contour
			cvSet(roi_m, th_black, NULL);
			cvDrawContours(roi_m, contourBest, th_white, th_white, -1, CV_FILLED, 8, cvPoint(0, 0));
			// calucalte image-moments
			cvMoments(roi_m, &mu, 0);
			// calucalte the mass center
			CvPoint p = cvPoint(mu.m10 / mu.m00, mu.m01 / mu.m00);
			CvPoint oldMCenter = cvPoint(tc->mx, tc->my);
			tc->mx = p.x + tc->roi_x;
			tc->my = p.y + tc->roi_y;
			CvPoint newMCenter = cvPoint(tc->mx, tc->my);

			// remember the old radius and calcutlate the new x/y position and radius of the found contour
			float oldRadius = tc->r;
			// estimate x/y position and radius of the sphere
			psmove_tracker_estimate_circle_from_contour(contourBest, &x, &y, &tc->r);

			// apply radius-smoothing if enabled
			if (tracker->tracker_adaptive_z) {
				// calculate the difference between calculated radius and the smoothed radius of the past
				float rDiff = abs(tc->rs - tc->r);
				// calcualte a adaptive smoothing factor
				// a big distance leads to no smoothing, a small one to strong smoothing
				float rf = MIN(rDiff/4+0.15,1);

				// apply adaptive smoothing of the radius
				tc->rs = tc->rs * (1 - rf) + tc->r * rf;
				tc->r = tc->rs;
			}

			// apply x/y coordinate smoothing if enabled
			if (tracker->tracker_adaptive_z) {
				// a big distance between the old and new center of mass results in no smoothing
				// a little one to strong smoothing
				float diff = th_dist(oldMCenter, newMCenter);
				float f = MIN(diff / 7 + 0.15, 1);
				// apply adaptive smoothing
				tc->x = tc->x * (1 - f) + (x + tc->roi_x) * f;
				tc->y = tc->y * (1 - f) + (y + tc->roi_y) * f;
			} else {
				// do NOT apply adaptive smoothing
				tc->x = x + tc->roi_x;
				tc->y = y + tc->roi_y;
			}

			// calculate the quality of the tracking
			int pixelInBlob = cvCountNonZero(roi_m);
			float pixelInResult = tc->r * tc->r * th_PI;
                        tc->q1 = 0;
                        tc->q2 = FLT_MAX;
                        tc->q3 = tc->r;

			// decrease TQ1 by half if below 20px (gives better results if controller is far away)
			if (pixelInBlob < 20) {
				tc->q1 /= 2;
                        }

			// The quality checks are all performed on the radius of the blob
			// its old radius and size.
			tc->q1 = pixelInBlob / pixelInResult;

			// always check pixel-ratio and minimal size
			sphere_found = tc->q1 > tracker->tracker_t1 && tc->q3 > tracker->tracker_t3;

			// use the mass center if the quality is very good
			// TODO: make 0.85 as a CONST
			if (tc->q1 > 0.85) {
				tc->x = tc->mx;
				tc->y = tc->my;
			}
			// only perform check if we already found the sphere once
			if (oldRadius > 0 && tc->search_quadrant==0) {
				tc->q2 = abs(oldRadius - tc->r) / (oldRadius + FLT_EPSILON);

				// additionally check for to big changes
				sphere_found = sphere_found && tc->q2 < tracker->tracker_t2;
			}

			// only if the quality is okay update the future ROI
			if (sphere_found) {
				// use adaptive color detection
				// only if 	1) the sphere has been found
				// AND		2) the UPDATE_RATE has passed
				// AND		3) the tracking-quality is high;
				int do_color_adaption = 0;
				long now = psmove_util_get_ticks();
				if (tracker->color_update_rate > 0 && (now - tc->last_color_update) > tracker->color_update_rate*1000)
					do_color_adaption = 1;

				if (do_color_adaption && tc->q1 > tracker->color_t1 && tc->q2 < tracker->color_t2 && tc->q3 > tracker->color_t3) {
					// calculate the new estimated color (adaptive color estimation)
					CvScalar newColor = cvAvg(tracker->frame, roi_m);
					th_plus(tc->eColor.val, newColor.val, tc->eColor.val, 3);
					th_mul(tc->eColor.val, 0.5, tc->eColor.val, 3);
					tc->eColorHSV = th_brg2hsv(tc->eColor);
					tc->last_color_update = now;
					// CHECK if the current estimate is too far away from its original estimation
					if (psmove_tracker_hsvcolor_diff(tc) > tracker->adapt_t1) {
						tc->eColor = tc->eFColor;
						tc->eColorHSV = tc->eFColorHSV;
						sphere_found = 0;
					}
				}

				// update the future roi box
				br.width = th_max(br.width, br.height) * 3;
				br.height = br.width;
				// find a suitable ROI level
				for (i = 0; i < ROIS; i++) {
					if (br.width > tracker->roiI[i]->width && br.height > tracker->roiI[i]->height)
						break;
					tc->roi_level = i;
					// update easy accessors
					roi_i = tracker->roiI[tc->roi_level];
					roi_m = tracker->roiM[tc->roi_level];
				}

				// assure that the roi is within the target image
				psmove_tracker_set_roi(tracker, tc, tc->x - roi_i->width / 2, tc->y - roi_i->height / 2,  roi_i->width, roi_i->height);
			}
		}
		cvClearMemStorage(tracker->storage);
		cvResetImageROI(tracker->frame);

		if (sphere_found) {
			tc->search_quadrant = 0;
			// the sphere was found
			break;
		}else if(tc->roi_level>0){
			// the sphere was not found, increase the ROI and search again!
			tc->roi_x += roi_i->width / 2;
			tc->roi_y += roi_i->height / 2;

			tc->roi_level = tc->roi_level - 1;
			// update easy accessors
			roi_i = tracker->roiI[tc->roi_level];
			roi_m = tracker->roiM[tc->roi_level];

			// assure that the roi is within the target image
			psmove_tracker_set_roi(tracker, tc, tc->roi_x -roi_i->width / 2, tc->roi_y - roi_i->height / 2, roi_i->width, roi_i->height);
		}else {
			int rx;
			int ry;
			// the sphere could not be found til a reasonable roi-level
			
			switch(tc->search_quadrant)
			{
			case 0:
				rx=0;
				ry=0;
				break;
			case 1:
				rx=tracker->frame->width/2;
				ry=0;
				break;
			case 2:
				rx=tracker->frame->width/2;
				ry=tracker->frame->height/2;
				break;
			case 3:
				rx=0;
				ry=tracker->frame->height/2;
				break;
			default:
				assert(0);
			}

			tc->search_quadrant = (tc->search_quadrant + 1) % 4;
			tc->roi_level=0;
			psmove_tracker_set_roi(tracker, tc, rx, ry, tracker->roiI[tc->roi_level]->width, tracker->roiI[tc->roi_level]->height);
			break;
		}
	}

	// remember if the sphere was found
	tc->is_tracked = sphere_found;
	return sphere_found;
}

int psmove_tracker_update(PSMoveTracker *tracker, PSMove *move) {
	TrackedController* tc = NULL;
	int spheres_found = 0;
	int UPDATE_ALL_CONTROLLERS = (move == NULL);

    // FPS calculation
    long started = psmove_util_get_ticks();
	if (UPDATE_ALL_CONTROLLERS) {
		// iterate trough all controllers and find their lit spheres
		tc = tracker->controllers;
		for (; tc && tracker->frame; tc = tc->next) {
			spheres_found += psmove_tracker_update_controller(tracker, tc);
		}
	} else {
		// find just that specific controller
		tc = tracked_controller_find(tracker->controllers, move);
		if (tracker->frame && tc) {
			spheres_found = psmove_tracker_update_controller(tracker, tc);
		}
	}
    tracker->duration = (psmove_util_get_ticks() - started);

	// draw all/one controller information to camera image
#ifdef PRINT_DEBUG_STATS
	psmove_tracker_draw_tracking_stats(tracker);
#endif
	// return the number of spheres found
	return spheres_found;

}

int psmove_tracker_get_position(PSMoveTracker *tracker, PSMove *move, float *x, float *y, float *radius) {
	TrackedController* tc = tracked_controller_find(tracker->controllers, move);
	psmove_return_val_if_fail(tc != NULL, 0);

	if (x)
		*x = tc->x;

	if (y)
		*y = tc->y;

	if (radius)
		*radius = tc->r;
	// TODO: return age of tracking values (if possible)
	
	return 1;
}

void psmove_tracker_free(PSMoveTracker *tracker) {
	tracked_controller_save_colors(tracker->controllers);

	char *filename = psmove_util_get_file_path(PSEYE_BACKUP_FILE);
	if (th_file_exists(filename)) {
            camera_control_restore_system_settings(tracker->cc, filename);
    }
	free(filename);
	
	cvReleaseMemStorage(&tracker->storage);
	int i = 0;
	for (; i < ROIS; i++) {
		cvReleaseImage(&tracker->roiM[i]);
		cvReleaseImage(&tracker->roiI[i]);
	}
	cvReleaseStructuringElement(&tracker->kCalib);
	tracked_controller_release(&tracker->controllers, 1);
	tracked_color_release(&tracker->available_colors, 1);

    camera_control_delete(tracker->cc);
    free(tracker);
}

// -------- Implementation: internal functions only
int psmove_tracker_adapt_to_light(PSMoveTracker *tracker, int lumMin, int expMin, int expMax) {
	int exp = expMin;
	// set the camera parameters to minimal exposure
	camera_control_set_parameters(tracker->cc, 0, 0, 0, exp, 0, 0xffff, 0xffff, 0xffff, -1, -1);
	IplImage* frame;
	// calculate a stepsize to increase the exposure, so that not more than 5 steps are neccessary
	int step = (expMax - expMin) / 10;
	if (step == 0)
		step = 1;
	int lastExp = exp;
	while (1) {
		// wait a little for the new parameters to be applied
		usleep(1000000 / 10);
		frame = camera_control_query_frame(tracker->cc);
		//}
		if (!frame)
			continue;

		// calculate the average color
		CvScalar avgColor = cvAvg(frame, 0x0);
		// calculate the average luminance (energy)
		float avgLum = th_avg(avgColor.val, 3);

		printf("exp:%d: lum:%f\n", exp, avgLum);
		// if the minimal luminance "limMin" has not been reached, increase the current exposure "exp"
		if (avgLum < lumMin)
			exp = exp + step;

		// check exposure boundaries
		if (exp < expMin)
			exp = expMin;
		if (exp > expMax)
			exp = expMax;

		// if the current exposure has been modified, apply it!
		if (lastExp != exp) {
			// reconfigure the camera
			camera_control_set_parameters(tracker->cc, 0, 0, 0, exp, 0, 0xffff, 0xffff, 0xffff, -1, -1);
			lastExp = exp;
		} else
			break;
	}
	printf("exposure set to %d(0x%x)\n", exp, exp);
	return exp;
}

void
psmove_tracker_wait_for_frame(PSMoveTracker *tracker, IplImage **frame, int delay)
{
    int elapsed_time = 0;
    int step = 10;

    while (elapsed_time < delay) {
        usleep(1000 * step);
        *frame = camera_control_query_frame(tracker->cc);
        elapsed_time += step;
    }
}

void psmove_tracker_get_diff(PSMoveTracker* tracker, PSMove* move, int r, int g, int b, IplImage* on, IplImage* diff, int delay) {
	// the time to wait for the controller to set the color up
	IplImage* frame;
	// switch the LEDs ON and wait for the sphere to be fully lit
	r *= DIMMING_FACTOR;
	g *= DIMMING_FACTOR;
	b *= DIMMING_FACTOR;
	psmove_set_leds(move, r, g, b);
	psmove_update_leds(move);

	// take the first frame (sphere lit)
        psmove_tracker_wait_for_frame(tracker, &frame, delay);
	cvCopy(frame, on, NULL);

	// switch the LEDs OFF and wait for the sphere to be off
	psmove_set_leds(move, 0, 0, 0);
	psmove_update_leds(move);

	// take the second frame (sphere iff)
        psmove_tracker_wait_for_frame(tracker, &frame, delay);

	// convert both to grayscale images
	IplImage* grey1 = cvCloneImage(diff);
	IplImage* grey2 = cvCloneImage(diff);
	cvCvtColor(frame, grey1, CV_BGR2GRAY);
	cvCvtColor(on, grey2, CV_BGR2GRAY);

	// calculate the diff of to images and save it in "diff"
	cvAbsDiff(grey1, grey2, diff);

	// clean up
	cvReleaseImage(&grey1);
	cvReleaseImage(&grey2);
}

void psmove_tracker_set_roi(PSMoveTracker* tracker, TrackedController* tc, int roi_x, int roi_y, int roi_width, int roi_height) {
	tc->roi_x = roi_x;
	tc->roi_y = roi_y;
	
	if (tc->roi_x < 0)
		tc->roi_x = 0;
	if (tc->roi_y < 0)
		tc->roi_y = 0;

	if (tc->roi_x + roi_width > tracker->frame->width)
		tc->roi_x = tracker->frame->width - roi_width;
	if (tc->roi_y + roi_height > tracker->frame->height)
		tc->roi_y = tracker->frame->height - roi_height;
}

void psmove_tracker_prepare_colors(PSMoveTracker* tracker) {
	// create MAGENTA (good tracking)
	tracked_color_insert(&tracker->available_colors, 0xff, 0, 0xff);
	// create CYAN (fair tracking)
	tracked_color_insert(&tracker->available_colors, 0, 0xff, 0xff);
	// create BLUE (fair tracking)
	tracked_color_insert(&tracker->available_colors, 0, 0, 0xff);

}

void psmove_tracker_draw_tracking_stats(PSMoveTracker* tracker) {
	CvPoint p;
	IplImage* frame = psmove_tracker_get_image(tracker);

	float textSmall = 0.8;
	float textNormal = 1;
	char text[256];
	CvScalar c;
	CvScalar avgC;
	float avgLum = 0;
	int roi_w = 0;
	int roi_h = 0;

	// general statistics
	avgC = cvAvg(frame, 0x0);
	avgLum = th_avg(avgC.val, 3);
	cvRectangle(frame, cvPoint(0, 0), cvPoint(frame->width, 25), th_black, CV_FILLED, 8, 0);
	sprintf(text, "fps:%.0f", tracker->debug_fps);
	th_put_text(frame, text, cvPoint(10, 20), th_white, textNormal);
        if (tracker->duration) {
            tracker->debug_fps = (0.85 * tracker->debug_fps + 0.15 *
                (1000. / (double)tracker->duration));
        }
	sprintf(text, "avg(lum):%.0f", avgLum);
	th_put_text(frame, text, cvPoint(255, 20), th_white, textNormal);

	TrackedController* tc;
	// draw all/one controller information to camera image
	tc = tracker->controllers;
	for (; tc != 0x0 && tracker->frame; tc = tc->next) {
		if (tc->is_tracked) {
			// controller specific statistics
			p.x = tc->x;
			p.y = tc->y;
			roi_w = tracker->roiI[tc->roi_level]->width;
			roi_h = tracker->roiI[tc->roi_level]->height;
			c = tc->eColor;

			cvRectangle(frame, cvPoint(tc->roi_x, tc->roi_y), cvPoint(tc->roi_x + roi_w, tc->roi_y + roi_h), th_white, 3, 8, 0);
			cvRectangle(frame, cvPoint(tc->roi_x, tc->roi_y), cvPoint(tc->roi_x + roi_w, tc->roi_y + roi_h), th_red, 1, 8, 0);
			cvRectangle(frame, cvPoint(tc->roi_x, tc->roi_y - 45), cvPoint(tc->roi_x + roi_w, tc->roi_y - 5), th_black, CV_FILLED, 8, 0);

			int vOff = 0;
			if (roi_h == frame->height)
				vOff = roi_h;
			sprintf(text, "RGB:%x,%x,%x", (int) c.val[2], (int) c.val[1], (int) c.val[0]);
			th_put_text(frame, text, cvPoint(tc->roi_x, tc->roi_y + vOff - 5), c, textSmall);

			sprintf(text, "ROI:%dx%d", roi_w, roi_h);
			th_put_text(frame, text, cvPoint(tc->roi_x, tc->roi_y + vOff - 15), c, textSmall);

			double distance = psmove_tracker_calculate_distance(tracker, tc->r * 2);

			sprintf(text, "radius: %.2f", tc->r);
			th_put_text(frame, text, cvPoint(tc->roi_x, tc->roi_y + vOff - 35), c, textSmall);
			sprintf(text, "dist: %.2fmm", distance);
			th_put_text(frame, text, cvPoint(tc->roi_x, tc->roi_y + vOff - 25), c, textSmall);

			cvCircle(frame, p, tc->r, th_white, 1, 8, 0);
		}
	}
}

float psmove_tracker_hsvcolor_diff(TrackedController* tc) {
	float diff = 0;
	diff += abs(tc->eFColorHSV.val[0] - tc->eColorHSV.val[0]) * 1; // diff of HUE is very important
	diff += abs(tc->eFColorHSV.val[1] - tc->eColorHSV.val[1]) * 0.5; // saturation and value not so much
	diff += abs(tc->eFColorHSV.val[2] - tc->eColorHSV.val[2]) * 0.5;
	return diff;
}

float psmove_tracker_calculate_distance(PSMoveTracker* tracker, float blob_diameter) {

	// PS Eye uses OV7725 Chip --> http://image-sensors-world.blogspot.co.at/2010/10/omnivision-vga-sensor-inside-sony-eye.html
	// http://www.ovt.com/download_document.php?type=sensor&sensorid=80
	// http://photo.stackexchange.com/questions/12434/how-do-i-calculate-the-distance-of-an-object-in-a-photo
	/*
	 distance to object (mm) =   focal length (mm) * real height of the object (mm) * image height (pixels)
	 ---------------------------------------------------------------------------
	 object height (pixels) * sensor height (mm)
	 */
	 
	// TODO: use measured distance only if the psmoveeye is used
	//int n = 32;
	// distance in mm
	//int x[]={100,150,200,300,400,500,600,700,800,900,1000,1100,1200,1300,1400,1500,1600,1700,1800,1900,2000,2100,2200,2300,2400,2500,2600,2700,2800,2900,3000};
	// radius in pixel
	//float y[]={122.27,79.8,60.02,40.34,30.16,24.26,20.18,17.41,15.37,13.65,12.32,11.28,10.35,9.58,8.93,8.35,7.83,7.37,7.03,6.62,6.38,6.13,5.82,5.58,5.33,5.39,5.07,5.02,4.94,4.5,4.45};
	
	return (tracker->cam_focal_length * tracker->ps_move_diameter * tracker->user_factor_dist) / (blob_diameter * tracker->cam_pixel_height / 100.0 + FLT_EPSILON);
}

void psmove_tracker_biggest_contour(IplImage* img, CvMemStorage* stor, CvSeq** resContour, float* resSize) {
	CvSeq* contour;
	*resSize = 0;
	*resContour = 0;
	cvFindContours(img, stor, &contour, sizeof(CvContour), CV_RETR_LIST, CV_CHAIN_APPROX_SIMPLE, cvPoint(0, 0));

	for (; contour; contour = contour->h_next) {
		float f = cvContourArea(contour, CV_WHOLE_SEQ, 0);
		if (f > *resSize) {
			*resSize = f;
			*resContour = contour;
		}
	}
}

void
psmove_tracker_estimate_circle_from_contour(CvSeq* cont, float *x, float *y, float* radius)
{
    psmove_return_if_fail(cont != NULL);
    psmove_return_if_fail(x != NULL && y != NULL && radius != NULL);

	int i, j;
	float d = 0;
	float cd = 0;
	CvPoint m1;
	CvPoint m2;
	CvPoint * p1;
	CvPoint * p2;

	int step = MAX(1,cont->total/20);

	// compare every two points of the contour (but not more than 20)
	// to find the most distant pair
	for (i = 0; i < cont->total; i += step) {
		p1 = (CvPoint*) cvGetSeqElem(cont, i);
		for (j = i + 1; j < cont->total; j += step) {
			p2 = (CvPoint*) cvGetSeqElem(cont, j);
			cd = th_dist_squared(*p1,*p2);
			if (cd > d) {
				d = cd;
				m1 = *p1;
				m2 = *p2;
			}
		}
	}
	// calculate center of that pair
	*x = 0.5 * (m1.x + m2.x);
	*y = 0.5 * (m1.y + m2.y);
	// calcualte the radius
	*radius = sqrt(d) / 2;
}

int
psmove_tracker_center_roi_on_controller(TrackedController* tc, PSMoveTracker* tracker, CvPoint *center)
{
    psmove_return_val_if_fail(tc != NULL, 0);
    psmove_return_val_if_fail(tracker != NULL, 0);
    psmove_return_val_if_fail(center != NULL, 0);

	CvScalar min, max;
	th_minus(tc->eColorHSV.val, tracker->rHSV.val, min.val, 3);
	th_plus(tc->eColorHSV.val, tracker->rHSV.val, max.val, 3);

	IplImage *roi_i = tracker->roiI[tc->roi_level];
	IplImage *roi_m = tracker->roiM[tc->roi_level];

	// cut out the roi!
	cvSetImageROI(tracker->frame, cvRect(tc->roi_x, tc->roi_y, roi_i->width, roi_i->height));
	cvCvtColor(tracker->frame, roi_i, CV_BGR2HSV);

	// apply color filter
	cvInRangeS(roi_i, min, max, roi_m);
	
	float sizeBest = 0;
	CvSeq* contourBest = NULL;
	psmove_tracker_biggest_contour(roi_m, tracker->storage, &contourBest, &sizeBest);
	if (contourBest) {
		cvSet(roi_m, th_black, NULL);
		cvDrawContours(roi_m, contourBest, th_white, th_white, -1, CV_FILLED, 8, cvPoint(0, 0));
		// calucalte image-moments to estimate the better ROI center
		CvMoments mu;
		cvMoments(roi_m, &mu, 0);

		*center = cvPoint(mu.m10 / mu.m00, mu.m01 / mu.m00);
		center->x += tc->roi_x - roi_m->width / 2;
		center->y += tc->roi_y - roi_m->height / 2;
	}
	cvClearMemStorage(tracker->storage);
	cvResetImageROI(tracker->frame);

        return (contourBest != NULL);
}

