/*
 * Copyright 2014 Google Inc. All Rights Reserved.
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

#include <sstream>

#include <tango-gl/conversions.h>

#include "tango-area-learning/area_learning_app.h"

namespace {
const int kVersionStringLength = 128;

// This function routes onPoseAvailable callbacks to the application object for
// handling.
//
// @param context, context will be a pointer to a AreaLearningApp
//        instance on which to call callbacks.
// @param pose, pose data to route to onPoseAvailable function.
void onPoseAvailableRouter(void* context, const TangoPoseData* pose) {
  using namespace tango_area_learning;
  AreaLearningApp* app = static_cast<AreaLearningApp*>(context);
  app->onPoseAvailable(pose);
}

// This function routes onTangoEvent callbacks to the application object for
// handling.
//
// @param context, context will be a pointer to a AreaLearningApp
//        instance on which to call callbacks.
// @param event, TangoEvent to route to onTangoEventAvailable function.
void onTangoEventAvailableRouter(void* context, const TangoEvent* event) {
  using namespace tango_area_learning;
  AreaLearningApp* app = static_cast<AreaLearningApp*>(context);
  app->onTangoEventAvailable(event);
}
}  // namespace

namespace tango_area_learning {
void AreaLearningApp::onPoseAvailable(const TangoPoseData* pose) {
  std::lock_guard<std::mutex> lock(pose_mutex_);
  pose_data_.UpdatePose(*pose);
}

void AreaLearningApp::onTangoEventAvailable(const TangoEvent* event) {
  std::lock_guard<std::mutex> lock(tango_event_mutex_);
  tango_event_data_.UpdateTangoEvent(event);

  if (event->type == TangoEventType::TANGO_EVENT_AREA_LEARNING &&
      !strcmp(event->event_key, "AreaDescriptionSaveProgress")) {
    OnAdfSavingProgressChanged(std::atof(event->event_value) * 100);
  }
}

AreaLearningApp::AreaLearningApp() {
  tango_core_version_string_ = "N/A";
  loaded_adf_string_ = "Loaded ADF: N/A";
}

AreaLearningApp::~AreaLearningApp() {
  TangoConfig_free(tango_config_);
  JNIEnv* env;
  java_vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  env->DeleteGlobalRef(calling_activity_obj_);
}

int AreaLearningApp::TangoInitialize(JNIEnv* env, jobject caller_activity) {
  // The first thing we need to do for any Tango enabled application is to
  // initialize the service. We'll do that here, passing on the JNI environment
  // and jobject corresponding to the Android activity that is calling us.
  int ret = TangoService_initialize(env, caller_activity);

  jclass cls = env->GetObjectClass(caller_activity);
  on_saving_adf_progress_updated_ =
      env->GetMethodID(cls, "updateSavingAdfProgress", "(I)V");

  calling_activity_obj_ =
      reinterpret_cast<jobject>(env->NewGlobalRef(caller_activity));
  return ret;
}

int AreaLearningApp::TangoSetupConfig(bool is_area_learning_enabled,
                                      bool is_loading_adf) {
  // Here, we'll configure the service to run in the way we'd want. For this
  // application, we'll start from the default configuration
  // (TANGO_CONFIG_DEFAULT). This enables basic motion tracking capabilities.
  tango_config_ = TangoService_getConfig(TANGO_CONFIG_DEFAULT);
  if (tango_config_ == nullptr) {
    LOGE("AreaLearningApp: Failed to get default config form");
    return TANGO_ERROR;
  }

  int ret = TangoConfig_setBool(tango_config_, "config_enable_learning_mode",
                                is_area_learning_enabled);
  if (ret != TANGO_SUCCESS) {
    LOGE(
        "AreaLearningApp: config_enable_learning_mode failed with error"
        "code: %d",
        ret);
    return ret;
  }

  // If load ADF, load the most recent saved ADF.
  if (is_loading_adf) {
    std::vector<std::string> adf_list;
    GetAdfUuids(&adf_list);
    if (!adf_list.empty()) {
      std::string adf_uuid = adf_list.back();
      std::ostringstream adf_str_stream;
      adf_str_stream << "Number of ADFs:" << adf_list.size()
                     << ", Loaded ADF: " << adf_uuid;
      loaded_adf_string_ = adf_str_stream.str();
      ret = TangoConfig_setString(
          tango_config_, "config_load_area_description_UUID", adf_uuid.c_str());
      if (ret != TANGO_SUCCESS) {
        LOGE("AreaLearningApp: get ADF UUID failed with error code: %d", ret);
      }
    }
  }

  tango_core_version_string_ = GetTangoServiceVersion();

  return ret;
}

int AreaLearningApp::TangoConnectCallbacks() {
  // Setting up the frame pair for the onPoseAvailable callback.
  TangoCoordinateFramePair pairs[3] = {
      {TANGO_COORDINATE_FRAME_START_OF_SERVICE, TANGO_COORDINATE_FRAME_DEVICE},
      {TANGO_COORDINATE_FRAME_AREA_DESCRIPTION, TANGO_COORDINATE_FRAME_DEVICE},
      {TANGO_COORDINATE_FRAME_AREA_DESCRIPTION,
       TANGO_COORDINATE_FRAME_START_OF_SERVICE}};

  // Attach onPoseAvailable callback.
  // The callback will be called after the service is connected.
  int ret =
      TangoService_connectOnPoseAvailable(3, pairs, onPoseAvailableRouter);
  if (ret != TANGO_SUCCESS) {
    LOGE("AreaLearningApp: Failed to connect to pose callback with error"
         "code: %d", ret);
    return ret;
  }

  // Attach onEventAvailable callback.
  // The callback will be called after the service is connected.
  ret = TangoService_connectOnTangoEvent(onTangoEventAvailableRouter);
  if (ret != TANGO_SUCCESS) {
    LOGE("AreaLearningApp: Failed to connect to event callback with error"
         "code: %d", ret);
    return ret;
  }
  return ret;
}

// Connect to Tango Service, service will start running, and
// pose can be queried.
bool AreaLearningApp::TangoConnect() {
  TangoErrorType ret = TangoService_connect(this, tango_config_);
  bool is_connected = (ret == TANGO_SUCCESS);
  if (!is_connected) {
    LOGE("AreaLearningApp: Failed to connect to the Tango service with"
         "error code: %d", ret);
  }
  return is_connected;
}

void AreaLearningApp::TangoDisconnect() {
  // When disconnecting from the Tango Service, it is important to make sure to
  // free your configuration object. Note that disconnecting from the service,
  // resets all configuration, and disconnects all callbacks. If an application
  // resumes after disconnecting, it must re-register configuration and
  // callbacks with the service.
  TangoConfig_free(tango_config_);
  tango_config_ = nullptr;
  TangoService_disconnect();
}

void AreaLearningApp::TangoResetMotionTracking() {
  TangoService_resetMotionTracking();
}

std::string AreaLearningApp::SaveAdf() {
  std::string adf_uuid_string;
  if(!pose_data_.IsRelocalized()) {
    return adf_uuid_string;
  }
  TangoUUID uuid;
  int ret = TangoService_saveAreaDescription(&uuid);
  if (ret == TANGO_SUCCESS) {
    LOGI("AreaLearningApp: Successfully saved ADF with UUID: %s", uuid);
    adf_uuid_string = std::string(uuid);
  } else {
    // Note: uuid is set to a nullptr in this case, so don't always try to
    // construct a string from it!
    LOGE("AreaLearningApp: Failed to save ADF with error code: %d", ret);
  }
  return adf_uuid_string;
}

std::string AreaLearningApp::GetAdfMetadataValue(const std::string& uuid,
                                                 const std::string& key) {
  size_t size = 0;
  char* output;
  TangoAreaDescriptionMetadata metadata;

  // Get the metadata object from the Tango Service.
  int ret = TangoService_getAreaDescriptionMetadata(uuid.c_str(), &metadata);
  if (ret != TANGO_SUCCESS) {
    LOGE("AreaLearningApp: Failed to get ADF metadata with error code: %d",
         ret);
  }

  // Query specific key-value from the metadata object.
  ret = TangoAreaDescriptionMetadata_get(metadata, key.c_str(), &size, &output);
  if (ret != TANGO_SUCCESS) {
    LOGE(
        "AreaLearningApp: Failed to get ADF metadata value with error "
        "code: %d",
        ret);
  }
  return std::string(output);
}

void AreaLearningApp::SetAdfMetadataValue(const std::string& uuid,
                                          const std::string& key,
                                          const std::string& value) {
  TangoAreaDescriptionMetadata metadata;
  int ret = TangoService_getAreaDescriptionMetadata(uuid.c_str(), &metadata);
  if (ret != TANGO_SUCCESS) {
    LOGE("AreaLearningApp: Failed to get ADF metadata with error code: %d",
         ret);
  }
  ret = TangoAreaDescriptionMetadata_set(metadata, key.c_str(), value.size(),
                                         value.c_str());
  if (ret != TANGO_SUCCESS) {
    LOGE("AreaLearningApp: Failed to set ADF metadata with error code: %d",
         ret);
  }
  ret = TangoService_saveAreaDescriptionMetadata(uuid.c_str(), metadata);
  if (ret != TANGO_SUCCESS) {
    LOGE("AreaLearningApp: Failed to save ADF metadata with error code: %d",
         ret);
  }
}

std::string AreaLearningApp::GetAllAdfUuids() {
  TangoConfig tango_config_ = TangoService_getConfig(TANGO_CONFIG_DEFAULT);
  if (tango_config_ == nullptr) {
    LOGE("AreaLearningApp: Failed to get default config form");
  }

  // Get all ADF's uuids.
  char* uuid_list = nullptr;
  int ret = TangoService_getAreaDescriptionUUIDList(&uuid_list);
  // uuid_list will contain a comma separated list of UUIDs.
  if (ret != TANGO_SUCCESS) {
    LOGE("AreaLearningApp: get ADF UUID failed with error code: %d", ret);
  }
  return std::string(uuid_list);
}

void AreaLearningApp::DeleteAdf(std::string uuid) {
  TangoService_deleteAreaDescription(uuid.c_str());
}

void AreaLearningApp::InitializeGLContent() { main_scene_.InitGLContent(); }

void AreaLearningApp::SetViewPort(int width, int height) {
  main_scene_.SetupViewPort(width, height);
}

void AreaLearningApp::Render() {
  // Query current pose data.
  TangoPoseData cur_pose;
  {
    std::lock_guard<std::mutex> lock(pose_mutex_);
    cur_pose = pose_data_.GetCurrentPoseData();
  }
  main_scene_.Render(cur_pose, pose_data_.IsRelocalized());
}

void AreaLearningApp::FreeContent() {
  pose_data_.ResetPoseData();
  main_scene_.FreeGLContent();
}

bool AreaLearningApp::IsRelocalized() {
  std::lock_guard<std::mutex> lock(pose_mutex_);
  return pose_data_.IsRelocalized();
}

std::string AreaLearningApp::GetStartServiceTDeviceString() {
  std::lock_guard<std::mutex> lock(pose_mutex_);
  return pose_data_.GetStartServiceTDeviceString();
}

std::string AreaLearningApp::GetAdfTDeviceString() {
  std::lock_guard<std::mutex> lock(pose_mutex_);
  return pose_data_.GetAdfTDeviceString();
}

std::string AreaLearningApp::GetAdfTStartServiceString() {
  std::lock_guard<std::mutex> lock(pose_mutex_);
  return pose_data_.GetAdfTStartServiceString();
}

std::string AreaLearningApp::GetEventString() {
  std::lock_guard<std::mutex> lock(tango_event_mutex_);
  return tango_event_data_.GetTangoEventString().c_str();
}

std::string AreaLearningApp::GetVersionString() {
  return tango_core_version_string_.c_str();
}

void AreaLearningApp::SetCameraType(
    tango_gl::GestureCamera::CameraType camera_type) {
  main_scene_.SetCameraType(camera_type);
}

void AreaLearningApp::OnTouchEvent(int touch_count,
                                      tango_gl::GestureCamera::TouchEvent event,
                                      float x0, float y0, float x1, float y1) {
  main_scene_.OnTouchEvent(touch_count, event, x0, y0, x1, y1);
}

void AreaLearningApp::GetAdfUuids(std::vector<std::string>* adf_list) {
  // Get all ADF's uuids.
  char* uuid_list = nullptr;
  int ret = TangoService_getAreaDescriptionUUIDList(&uuid_list);
  // uuid_list will contain a comma separated list of UUIDs.
  if (ret != TANGO_SUCCESS) {
    LOGE("AreaLearningApp: get ADF UUID failed with error code: %d", ret);
  }

  // Parse the uuid_list to get the individual uuids.
  if (uuid_list != nullptr) {
    if (uuid_list[0] != '\0') {
      char* parsing_char;
      char* saved_ptr;
      parsing_char = strtok_r(uuid_list, ",", &saved_ptr);
      while (parsing_char != nullptr) {
        std::string str = std::string(parsing_char);
        adf_list->push_back(str);
        parsing_char = strtok_r(nullptr, ",", &saved_ptr);
      }
    }
  }
}

std::string AreaLearningApp::GetTangoServiceVersion() {
  char version_buffer[kVersionStringLength];
  // Get TangoCore version string from service.
  int ret = TangoConfig_getString(
      tango_config_, "tango_service_library_version",
      version_buffer, sizeof(version_buffer));
  if (ret != TANGO_SUCCESS) {
    LOGE("AreaLearningApp: get tango core version failed with error code: %d",
         ret);
  }
  return std::string(version_buffer);
}

void AreaLearningApp::OnAdfSavingProgressChanged(int progress) {
  // Here, we notify the Java activity that we'd like it to update the saving
  // Adf progress.
  JNIEnv* env;
  java_vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  env->CallVoidMethod(calling_activity_obj_, on_saving_adf_progress_updated_,
                      progress);
}

}  // namespace tango_area_learning
