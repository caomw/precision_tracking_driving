/*
 * test_tracking.cpp
 *
 *  Created on: Jul 30, 2014
 *      Author: davheld
 */

#include <string>
#include <cstdio>
#include <sstream>

#include <boost/math/constants/constants.hpp>
#include <boost/make_shared.hpp>

#include <precision_tracking/track_manager_color.h>
#include <precision_tracking/tracker.h>
#include <precision_tracking/high_res_timer.h>

using std::string;

namespace {

const double pi = boost::math::constants::pi<double>();

} // namespace

// Structure for storing estimated velocities for each track.
struct TrackResults {
  int track_num;
  std::vector<Eigen::Vector3f> estimated_velocities;
  std::vector<bool> ignore_frame;
};

// Get the ground-truth velocities.
void getGTVelocities(const string& gt_folder, const int track_num,
                     std::vector<double>* gt_velocities) {
  string filename = gt_folder + "/track%dgt.txt";

  std::ostringstream filename_stream;
  filename_stream << gt_folder << "/track" << track_num << "gt.txt";
  filename = filename_stream.str();

  FILE* fid = fopen(filename.c_str(), "r");

  if (fid == NULL) {
    printf("Cannot open file: %s\n", filename.c_str());
    exit(1);
  }

  double velocity;
  while(fscanf(fid, "%lf\n", &velocity) > 0) {
    gt_velocities->push_back(velocity);
  }

  fclose(fid);
}

void computeErrorStatistics(const std::vector<double>& errors) {
  double sum_sq = 0;

  size_t num_frames = errors.size();

  // Compute the root-mean-square error.
  for (size_t i = 0; i < num_frames; ++i) {
    sum_sq += pow(errors[i], 2);
  }

  const double rms_error = sqrt(sum_sq / errors.size());

  printf("RMS error: %lf m/s\n", rms_error);
}

void evaluateTracking(const std::vector<TrackResults>& velocity_estimates,
                      const string& gt_folder,
                      boost::shared_ptr<std::vector<bool> > filter) {

  std::vector<double> errors;

  int total_framenum = -1;

  // Iterate over all tracks.
  for (size_t i = 0; i < velocity_estimates.size(); ++i) {
    TrackResults track_results = velocity_estimates[i];

    int track_num = track_results.track_num;

    std::vector<double> gt_velocities;
    getGTVelocities(gt_folder, track_num, &gt_velocities);

    int skipped = 0;

    for (size_t j = 0; j < track_results.estimated_velocities.size(); ++j) {
      total_framenum++;

      if (track_results.ignore_frame[j]) {
        skipped++;
        continue;
      }

      if (filter && !((*filter)[total_framenum])) {
        continue;
      }

      const Eigen::Vector3f& estimated_velocity =
          track_results.estimated_velocities[j];

      const double estimated_velocity_magnitude = estimated_velocity.norm();
      const double gt_velocity_magnitude = gt_velocities[j-skipped];
      const double error = estimated_velocity_magnitude - gt_velocity_magnitude;

      errors.push_back(error);
    }
  }

  computeErrorStatistics(errors);
}

// Filter to only evaluate on objects within a given distance (in meters).
void getWithinDistance(
    const precision_tracking::track_manager_color::TrackManagerColor& track_manager,
    const double max_distance, std::vector<bool>& filter) {
  const std::vector< boost::shared_ptr<precision_tracking::track_manager_color::Track> >& tracks =
      track_manager.tracks_;
  for (size_t i = 0; i < tracks.size(); ++i) {
    // Extract frames.
    const boost::shared_ptr<precision_tracking::track_manager_color::Track>& track = tracks[i];
    std::vector< boost::shared_ptr<precision_tracking::track_manager_color::Frame> > frames =
        track->frames_;
    for (size_t j = 1; j < frames.size(); ++j) {
      boost::shared_ptr<precision_tracking::track_manager_color::Frame> frame = frames[j];

      Eigen::Vector3f centroid = frame->getCentroid();
      const double distance = sqrt(pow(centroid(0), 2) + pow(centroid(1), 2));

      if (distance <= max_distance) {
        filter.push_back(true);
      } else {
        filter.push_back(false);
      }
    }
  }
}

// Ingore frames in the back where half of the car was recorded at
// the beginning of a spin and the other half was recorded at the end of a spin.
// Also ignore frames where the time difference is extremely small, essentially
// because the car moved between the end of one spin to the beginning of the
// next.  For such frames, estimating the velocity is prone to errors
// that should ideally be fixed before the track is passed on to the velocity
// estimator.
void find_bad_frames(const precision_tracking::track_manager_color::TrackManagerColor& track_manager,
                     std::vector<TrackResults>* velocity_estimates) {
  const std::vector< boost::shared_ptr<precision_tracking::track_manager_color::Track> >& tracks =
      track_manager.tracks_;

  // Iterate over all tracks.
  for (size_t i = 0; i < tracks.size(); ++i) {
    // Extract frames.
    const boost::shared_ptr<precision_tracking::track_manager_color::Track>& track = tracks[i];
    std::vector< boost::shared_ptr<precision_tracking::track_manager_color::Frame> > frames =
        track->frames_;

    // Structure for storing estimated velocities for this track.
    TrackResults& track_estimates = (*velocity_estimates)[i];

    bool skip_next = false;
    double prev_angle = 0;
    double prev_time = 0;

    // Iterate over all frames for this track.
    for (size_t j = 0; j < frames.size(); ++j) {
      boost::shared_ptr<precision_tracking::track_manager_color::Frame> frame = frames[j];

      const Eigen::Vector3f& centroid = frame->getCentroid();
      const double angle = atan2(centroid(1), centroid(0));
      const double angle_diff = fabs(angle - prev_angle);

      const double curr_time = frame->timestamp_;
      const double time_diff = curr_time - prev_time;

      prev_angle = angle;

      if (j > 0) {
        if (angle_diff <= 1 || j == 0) {
          if (!skip_next) {
            if (time_diff >= 0.05) {
              //track_estimates.ignore_frame.push_back(false);
            } else {
              track_estimates.ignore_frame[j-1] = true;
            }
          } else {
            track_estimates.ignore_frame[j-1] = true;
          }
          skip_next = false;
        } else {
          track_estimates.ignore_frame[j-1] = true;
          skip_next = true;
          if (j > 1) {
            track_estimates.ignore_frame[j-2] = true;
          }
        }
      }
    }
  }
}

// Computes the sensor resolution for an object at a given distance,
// for the 64-beam Velodyne.
void getSensorResolution(const Eigen::Vector3f& centroid_local_coordinates,
                         double* sensor_horizontal_res,
                         double* sensor_vertical_res) {
  // Get the distance to the tracked object.
  const double horizontal_distance =
      sqrt(pow(centroid_local_coordinates(0), 2) +
           pow(centroid_local_coordinates(1), 2));

  // The horizontal resolution for the 64-beam Velodyne spinning at 10 Hz
  // is 0.18 degrees.
  const double velodyne_horizontal_angular_res = 0.18;

  // There are 64 beams spanning 26.8 vertical degrees, so the average spacing
  // between each beam is computed as follows.
  const double velodyne_vertical_angular_res = 26.8 / 63;

  // We convert the angular resolution to meters for a given range.
  const double velodyne_horizontal_res =
      2 * horizontal_distance *
      tan(velodyne_horizontal_angular_res / 2.0 * pi / 180.0);
  const double velodyne_vertical_res =
      2 * horizontal_distance *
      tan(velodyne_vertical_angular_res / 2.0 * pi / 180.0);

  *sensor_horizontal_res = velodyne_horizontal_res;
  *sensor_vertical_res = velodyne_vertical_res;
}

void track(precision_tracking::Tracker* tracker,
           const precision_tracking::track_manager_color::TrackManagerColor& track_manager,
           std::vector<TrackResults>* velocity_estimates) {
  int total_num_frames = 0;

  const std::vector< boost::shared_ptr<precision_tracking::track_manager_color::Track> >& tracks =
      track_manager.tracks_;

  std::ostringstream hrt_title_stream;
  hrt_title_stream << "Total time for tracking " << tracks.size() << " objects";
  precision_tracking::HighResTimer hrt(hrt_title_stream.str());
  hrt.start();

  // Iterate over all tracks.
  for (size_t i = 0; i < tracks.size(); ++i) {
    // Reset the tracker for this new track.
    tracker->clear();

    // Extract frames.
    const boost::shared_ptr<precision_tracking::track_manager_color::Track>& track = tracks[i];
    std::vector< boost::shared_ptr<precision_tracking::track_manager_color::Frame> > frames =
        track->frames_;

    // Structure for storing estimated velocities for this track.
    TrackResults track_estimates;
    track_estimates.track_num = track->track_num_;

    // Iterate over all frames for this track.
    for (size_t j = 0; j < frames.size(); ++j) {
      boost::shared_ptr<precision_tracking::track_manager_color::Frame> frame = frames[j];

      // Get the sensor resolution.
      double sensor_horizontal_resolution;
      double sensor_vertical_resolution;
      getSensorResolution(frame->getCentroid(), &sensor_horizontal_resolution,
                          &sensor_vertical_resolution);

      // Track object.
      Eigen::Vector3f estimated_velocity;
      double alignment_probability;
      tracker->addPoints(frame->cloud_, frame->timestamp_,
                         sensor_horizontal_resolution,
                         sensor_vertical_resolution,
                         &estimated_velocity,
                         &alignment_probability);

      // The first time we see this object, we don't have a velocity yet.
      // After the first time, save the estimated velocity.
      if (j > 0) {
        total_num_frames++;
        track_estimates.estimated_velocities.push_back(estimated_velocity);

        // By default, don't ignore any frames.
        track_estimates.ignore_frame.push_back(false);
      }
    }

    velocity_estimates->push_back(track_estimates);
  }

  hrt.stop();
  hrt.print();
  const double ms = hrt.getMilliseconds();
  printf("Mean runtime per frame: %lf ms\n", ms / total_num_frames);
}

void trackAndEvaluate(
    precision_tracking::Tracker* tracker,
    const precision_tracking::track_manager_color::TrackManagerColor& track_manager,
    const string gt_folder) {
  // Track all objects and store the estimated velocities.
  std::vector<TrackResults> velocity_estimates;
  track(tracker, track_manager, &velocity_estimates);

  // Find bad frames that we want to ignore.
  find_bad_frames(track_manager, &velocity_estimates);

  // Evaluate the tracking accuracy.
  boost::shared_ptr<std::vector<bool> > empty_filter;
  evaluateTracking(velocity_estimates, gt_folder, empty_filter);

  // Evaluate the tracking accuracy for nearby objects.
  const double max_distance = 5;
  printf("Evaluating only for objects within %lf m:\n", max_distance);
  boost::shared_ptr<std::vector<bool> > filter(new std::vector<bool>);
  getWithinDistance(track_manager, max_distance, *filter);
  evaluateTracking(velocity_estimates, gt_folder, filter);
}

void testKalman(const precision_tracking::track_manager_color::TrackManagerColor& track_manager,
                const string gt_folder) {
  printf("Tracking objects with the centroid-based Kalman filter baseline. "
         "This method is very fast but not very accurate. Please wait...\n");
  precision_tracking::Params params;
  precision_tracking::Tracker centroid_tracker(&params);
  trackAndEvaluate(&centroid_tracker, track_manager, gt_folder);
}

void testPrecisionTracker(
    const precision_tracking::track_manager_color::TrackManagerColor& track_manager,
    const string gt_folder) {
  printf("\nTracking objects with our precision tracker. "
         "This method is accurate and fairly fast. Please wait...\n");
  precision_tracking::Params params;
  precision_tracking::Tracker precision_tracker(&params);
  precision_tracker.setPrecisionTracker(
      boost::make_shared<precision_tracking::PrecisionTracker>(&params));
  trackAndEvaluate(&precision_tracker, track_manager, gt_folder);
}

void testPrecisionTrackerColor(
    const precision_tracking::track_manager_color::TrackManagerColor& track_manager,
    const string gt_folder) {
  printf("\nTracking objects with our precision tracker using color. "
         "This method is a bit more accurate but much slower. Please wait (will be slow)...\n");
  precision_tracking::Params params;
  params.useColor = true;
  precision_tracking::Tracker precision_tracker_color(&params);
  precision_tracker_color.setPrecisionTracker(
      boost::make_shared<precision_tracking::PrecisionTracker>(&params));
  trackAndEvaluate(&precision_tracker_color, track_manager, gt_folder);
}

int main(int argc, char **argv)
{
  if (argc < 3) {
    printf("Usage: %s tm_file gt_folder\n", argv[0]);
    return (1);
  }

  string color_tm_file = argv[1];
  string gt_folder = argv[2];

  // Load tracks.
  printf("Loading file: %s\n", color_tm_file.c_str());
  precision_tracking::track_manager_color::TrackManagerColor track_manager(color_tm_file);
  printf("Found %zu tracks\n", track_manager.tracks_.size());

  // Track objects and evaluate the accuracy.
  printf("Tracking objects - please wait...\n\n");

  // Testing the centroid-based Kalman filter baseline method - should be
  // very fast but not very accurate.
  testKalman(track_manager, gt_folder);

  // Testing our precision tracker - should be very accurate and quite fast.
  testPrecisionTracker(track_manager, gt_folder);

  // Testing our precision tracker with color - should be even more accurate
  // but slow.
  testPrecisionTrackerColor(track_manager, gt_folder);

  return 0;
}

