/*
**
** Copyright 2017, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include "perfprofd_binder.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <functional>

#include <inttypes.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/test_utils.h>
#include <android-base/unique_fd.h>
#include <android/os/DropBoxManager.h>
#include <binder/BinderService.h>
#include <binder/IResultReceiver.h>
#include <binder/Status.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <utils/String16.h>
#include <utils/String8.h>
#include <utils/Vector.h>

#include "android/os/BnPerfProfd.h"
#include "perfprofd_config.pb.h"
#include "perfprofd_record.pb.h"

#include "config.h"
#include "perfprofdcore.h"

namespace android {
namespace perfprofd {
namespace binder {

using Status = ::android::binder::Status;

class BinderConfig : public Config {
 public:
  bool send_to_dropbox = true;

  bool is_profiling = false;

  void Sleep(size_t seconds) override {
    if (seconds == 0) {
      return;
    }
    std::unique_lock<std::mutex> guard(mutex_);
    using namespace std::chrono_literals;
    cv_.wait_for(guard, seconds * 1s, [&]() { return interrupted_; });
  }
  bool ShouldStopProfiling() override {
    std::unique_lock<std::mutex> guard(mutex_);
    return interrupted_;
  }

  void ResetStopProfiling() {
    std::unique_lock<std::mutex> guard(mutex_);
    interrupted_ = false;
  }
  void StopProfiling() {
    std::unique_lock<std::mutex> guard(mutex_);
    interrupted_ = true;
    cv_.notify_all();
  }

  bool IsProfilingEnabled() const override {
    return true;
  }

  // Operator= to simplify setting the config values. This will retain the
  // original mutex, condition-variable etc.
  BinderConfig& operator=(const BinderConfig& rhs) {
    // Copy base fields.
    *static_cast<Config*>(this) = static_cast<const Config&>(rhs);

    send_to_dropbox = rhs.send_to_dropbox;

    return *this;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool interrupted_ = false;
};

class PerfProfdNativeService : public BinderService<PerfProfdNativeService>,
                               public ::android::os::BnPerfProfd {
 public:
  static status_t start();
  static int Main();

  static char const* getServiceName() { return "perfprofd"; }

  status_t dump(int fd, const Vector<String16> &args) override;

  Status startProfiling(int32_t profilingDuration,
                                int32_t profilingInterval,
                                int32_t iterations) override;
  Status startProfilingProtobuf(const std::vector<uint8_t>& config_proto) override;

  Status stopProfiling() override;

  // Override onTransact so we can handle shellCommand.
  status_t onTransact(uint32_t _aidl_code,
                      const Parcel& _aidl_data,
                      Parcel* _aidl_reply,
                      uint32_t _aidl_flags = 0) override;

 private:
  // Handler for ProfilingLoop.
  bool BinderHandler(android::perfprofd::PerfprofdRecord* encodedProfile,
                     Config* config);
  // Helper for the handler.
  HandlerFn GetBinderHandler();

  status_t shellCommand(int /*in*/, int out, int err, Vector<String16>& args);

  template <typename ConfigFn> Status StartProfiling(ConfigFn fn);
  template <typename ProtoLoaderFn> Status StartProfilingProtobuf(ProtoLoaderFn fn);
  Status StartProfilingProtobufFd(int fd);

  std::mutex lock_;

  BinderConfig cur_config_;

  int seq_ = 0;
};

static Status WriteDropboxFile(android::perfprofd::PerfprofdRecord* encodedProfile,
                               Config* config) {
  TemporaryFile tmp_file(config->destination_directory);
  // Remove the temp file from the file system for security reasons.
  tmp_file.DoNotRemove();
  if (unlink(tmp_file.path) != 0) {
    PLOG(WARNING) << "Could not unlink binder temp file";
  }

  {
    // protobuf-lite doesn't have file streams.
    struct FileCopyingOutputStream : public google::protobuf::io::CopyingOutputStream {
      int fd;
      explicit FileCopyingOutputStream(int fd_in) : fd(fd_in) {
      }
      bool Write(const void * buffer, int size) override {
        return android::base::WriteFully(fd, buffer, size);
      }
    };
    FileCopyingOutputStream fcos(tmp_file.fd);
    google::protobuf::io::CopyingOutputStreamAdaptor cosa(&fcos);
    bool serialized = encodedProfile->SerializeToZeroCopyStream(&cosa);
    if (!serialized) {
      return Status::fromExceptionCode(1, "Failed to serialize proto");
    }
    cosa.Flush();

    // TODO: Is an fsync necessary here?
  }

  // Dropbox takes ownership of the fd, and if it is not readonly,
  // a selinux violation will occur. Get a read-only version.
  android::base::unique_fd read_only;
  {
    char fdpath[64];
    snprintf(fdpath, arraysize(fdpath), "/proc/self/fd/%d", tmp_file.fd);
    read_only.reset(open(fdpath, O_RDONLY | O_CLOEXEC));
    if (read_only.get() < 0) {
      PLOG(ERROR) << "Could not create read-only fd";
      return Status::fromExceptionCode(1, "Could not create read-only fd");
    }
  }

  using DropBoxManager = android::os::DropBoxManager;
  sp<DropBoxManager> dropbox(new DropBoxManager());
  return dropbox->addFile(String16("perfprofd"), read_only.release(), 0);
}

bool PerfProfdNativeService::BinderHandler(
    android::perfprofd::PerfprofdRecord* encodedProfile,
    Config* config) {
  CHECK(config != nullptr);
  if (static_cast<BinderConfig*>(config)->send_to_dropbox) {
    size_t size = encodedProfile->ByteSize();
    Status status;
    if (size < 1024 * 1024) {
      // For a small size, send as a byte buffer directly.
      std::unique_ptr<uint8_t[]> data(new uint8_t[size]);
      encodedProfile->SerializeWithCachedSizesToArray(data.get());

      using DropBoxManager = android::os::DropBoxManager;
      sp<DropBoxManager> dropbox(new DropBoxManager());
      status = dropbox->addData(String16("perfprofd"),
                                data.get(),
                                size,
                                0);
    } else {
      // For larger buffers, we need to go through the filesystem.
      status = WriteDropboxFile(encodedProfile, config);
    }
    if (!status.isOk()) {
      LOG(WARNING) << "Failed dropbox submission: " << status.toString8();
    }
    return status.isOk();
  }

  if (encodedProfile == nullptr) {
    return false;
  }
  std::string data_file_path(config->destination_directory);
  data_file_path += "/perf.data";
  std::string path = android::base::StringPrintf("%s.encoded.%d", data_file_path.c_str(), seq_);
  PROFILE_RESULT result = SerializeProtobuf(encodedProfile, path.c_str());
  if (result != PROFILE_RESULT::OK_PROFILE_COLLECTION) {
    return false;
  }

  seq_++;
  return true;
}

HandlerFn PerfProfdNativeService::GetBinderHandler() {
  return HandlerFn(std::bind(&PerfProfdNativeService::BinderHandler,
                             this,
                             std::placeholders::_1,
                             std::placeholders::_2));
}

status_t PerfProfdNativeService::start() {
  IPCThreadState::self()->disableBackgroundScheduling(true);
  status_t ret = BinderService<PerfProfdNativeService>::publish();
  if (ret != android::OK) {
    return ret;
  }
  sp<ProcessState> ps(ProcessState::self());
  ps->startThreadPool();
  ps->giveThreadPoolName();
  return android::OK;
}

status_t PerfProfdNativeService::dump(int fd, const Vector<String16> &args) {
  auto out = std::fstream(base::StringPrintf("/proc/self/fd/%d", fd));
  out << "Nothing to log, yet!" << std::endl;

  return NO_ERROR;
}

Status PerfProfdNativeService::startProfiling(int32_t profilingDuration,
                                              int32_t profilingInterval,
                                              int32_t iterations) {
  auto config_fn = [&](BinderConfig& config) {
    config = BinderConfig();  // Reset to a default config.

    config.sample_duration_in_s = static_cast<uint32_t>(profilingDuration);
    config.collection_interval_in_s = static_cast<uint32_t>(profilingInterval);
    config.main_loop_iterations = static_cast<uint32_t>(iterations);
  };
  return StartProfiling(config_fn);
}
Status PerfProfdNativeService::startProfilingProtobuf(const std::vector<uint8_t>& config_proto) {
  auto proto_loader_fn = [&config_proto](ProfilingConfig& proto_config) {
    return proto_config.ParseFromArray(config_proto.data(), config_proto.size());
  };
  return StartProfilingProtobuf(proto_loader_fn);
}

template <typename ConfigFn>
Status PerfProfdNativeService::StartProfiling(ConfigFn fn) {
  std::lock_guard<std::mutex> guard(lock_);

  if (cur_config_.is_profiling) {
    // TODO: Define error code?
    return binder::Status::fromServiceSpecificError(1);
  }
  cur_config_.is_profiling = true;
  cur_config_.ResetStopProfiling();

  fn(cur_config_);

  HandlerFn handler = GetBinderHandler();
  auto profile_runner = [handler](PerfProfdNativeService* service) {
    ProfilingLoop(service->cur_config_, handler);

    // This thread is done.
    std::lock_guard<std::mutex> unset_guard(service->lock_);
    service->cur_config_.is_profiling = false;
  };
  std::thread profiling_thread(profile_runner, this);
  profiling_thread.detach();  // Let it go.

  return binder::Status::ok();
}

template <typename ProtoLoaderFn>
Status PerfProfdNativeService::StartProfilingProtobuf(ProtoLoaderFn fn) {
  ProfilingConfig proto_config;
  if (!fn(proto_config)) {
    return binder::Status::fromExceptionCode(2);
  }
  auto config_fn = [&proto_config](BinderConfig& config) {
    config = BinderConfig();  // Reset to a default config.

    // Copy proto values.
#define CHECK_AND_COPY_FROM_PROTO(name)      \
    if (proto_config.has_ ## name ()) {      \
      config. name = proto_config. name ();  \
    }
    CHECK_AND_COPY_FROM_PROTO(collection_interval_in_s)
    CHECK_AND_COPY_FROM_PROTO(use_fixed_seed)
    CHECK_AND_COPY_FROM_PROTO(main_loop_iterations)
    CHECK_AND_COPY_FROM_PROTO(destination_directory)
    CHECK_AND_COPY_FROM_PROTO(config_directory)
    CHECK_AND_COPY_FROM_PROTO(perf_path)
    CHECK_AND_COPY_FROM_PROTO(sampling_period)
    CHECK_AND_COPY_FROM_PROTO(sample_duration_in_s)
    CHECK_AND_COPY_FROM_PROTO(only_debug_build)
    CHECK_AND_COPY_FROM_PROTO(hardwire_cpus)
    CHECK_AND_COPY_FROM_PROTO(hardwire_cpus_max_duration_in_s)
    CHECK_AND_COPY_FROM_PROTO(max_unprocessed_profiles)
    CHECK_AND_COPY_FROM_PROTO(stack_profile)
    CHECK_AND_COPY_FROM_PROTO(collect_cpu_utilization)
    CHECK_AND_COPY_FROM_PROTO(collect_charging_state)
    CHECK_AND_COPY_FROM_PROTO(collect_booting)
    CHECK_AND_COPY_FROM_PROTO(collect_camera_active)
    CHECK_AND_COPY_FROM_PROTO(process)
    CHECK_AND_COPY_FROM_PROTO(use_elf_symbolizer)
    CHECK_AND_COPY_FROM_PROTO(send_to_dropbox)
#undef CHECK_AND_COPY_FROM_PROTO
  };
  return StartProfiling(config_fn);
}

Status PerfProfdNativeService::StartProfilingProtobufFd(int fd) {
  auto proto_loader_fn = [fd](ProfilingConfig& proto_config) {
    struct IstreamCopyingInputStream : public google::protobuf::io::CopyingInputStream {
      IstreamCopyingInputStream(int fd_in)
                : stream(base::StringPrintf("/proc/self/fd/%d", fd_in),
                         std::ios::binary | std::ios::in) {
      }

      int Read(void* buffer, int size) override {
        stream.read(reinterpret_cast<char*>(buffer), size);
        size_t count = stream.gcount();
        if (count > 0) {
          return count;
        }
        return -1;
      }

      std::ifstream stream;
    };
    std::unique_ptr<IstreamCopyingInputStream> is(new IstreamCopyingInputStream(fd));
    std::unique_ptr<google::protobuf::io::CopyingInputStreamAdaptor> is_adaptor(
        new google::protobuf::io::CopyingInputStreamAdaptor(is.get()));
    return proto_config.ParseFromZeroCopyStream(is_adaptor.get());
  };
  return StartProfilingProtobuf(proto_loader_fn);
}

Status PerfProfdNativeService::stopProfiling() {
  std::lock_guard<std::mutex> guard(lock_);
  if (!cur_config_.is_profiling) {
    // TODO: Define error code?
    return Status::fromServiceSpecificError(1);
  }

  cur_config_.StopProfiling();

  return Status::ok();
}

status_t PerfProfdNativeService::shellCommand(int in,
                                              int out,
                                              int err,
                                              Vector<String16>& args) {
  if (android::base::kEnableDChecks) {
    LOG(VERBOSE) << "Perfprofd::shellCommand";

    for (size_t i = 0, n = args.size(); i < n; i++) {
      LOG(VERBOSE) << "  arg[" << i << "]: '" << String8(args[i]).string() << "'";
    }
  }

  if (args.size() >= 1) {
    if (args[0] == String16("dump")) {
      dump(out, args);
      return OK;
    } else if (args[0] == String16("startProfiling")) {
      if (args.size() < 4) {
        return BAD_VALUE;
      }
      // TODO: handle invalid strings.
      int32_t duration = strtol(String8(args[1]).string(), nullptr, 0);
      int32_t interval = strtol(String8(args[2]).string(), nullptr, 0);
      int32_t iterations = strtol(String8(args[3]).string(), nullptr, 0);
      Status status = startProfiling(duration, interval, iterations);
      if (status.isOk()) {
        return OK;
      } else {
        return status.serviceSpecificErrorCode();
      }
    } else if (args[0] == String16("startProfilingProto")) {
      if (args.size() < 2) {
        return BAD_VALUE;
      }
      int fd = -1;
      if (args[1] == String16("-")) {
        fd = in;
      } else {
        // TODO: Implement reading from disk?
      }
      if (fd < 0) {
        return BAD_VALUE;
      }
      binder::Status status = StartProfilingProtobufFd(fd);
      if (status.isOk()) {
        return OK;
      } else {
        return status.serviceSpecificErrorCode();
      }
    } else if (args[0] == String16("stopProfiling")) {
      Status status = stopProfiling();
      if (status.isOk()) {
        return OK;
      } else {
        return status.serviceSpecificErrorCode();
      }
    }
  }
  return BAD_VALUE;
}

status_t PerfProfdNativeService::onTransact(uint32_t _aidl_code,
                                            const Parcel& _aidl_data,
                                            Parcel* _aidl_reply,
                                            uint32_t _aidl_flags) {
  switch (_aidl_code) {
    case IBinder::SHELL_COMMAND_TRANSACTION: {
      int in = _aidl_data.readFileDescriptor();
      int out = _aidl_data.readFileDescriptor();
      int err = _aidl_data.readFileDescriptor();
      int argc = _aidl_data.readInt32();
      Vector<String16> args;
      for (int i = 0; i < argc && _aidl_data.dataAvail() > 0; i++) {
        args.add(_aidl_data.readString16());
      }
      sp<IBinder> unusedCallback;
      sp<IResultReceiver> resultReceiver;
      status_t status;
      if ((status = _aidl_data.readNullableStrongBinder(&unusedCallback)) != OK)
        return status;
      if ((status = _aidl_data.readNullableStrongBinder(&resultReceiver)) != OK)
        return status;
      status = shellCommand(in, out, err, args);
      if (resultReceiver != nullptr) {
        resultReceiver->send(status);
      }
      return OK;
    }

    default:
      return BBinder::onTransact(_aidl_code, _aidl_data, _aidl_reply, _aidl_flags);
  }
}

int Main() {
  android::status_t ret;
  if ((ret = PerfProfdNativeService::start()) != android::OK) {
    LOG(ERROR) << "Unable to start InstalldNativeService: %d" << ret;
    exit(1);
  }

  android::IPCThreadState::self()->joinThreadPool();

  LOG(INFO) << "Exiting perfprofd";
  return 0;
}

}  // namespace binder
}  // namespace perfprofd
}  // namespace android
