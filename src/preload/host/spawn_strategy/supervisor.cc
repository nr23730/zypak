// Copyright 2020 Endless Mobile, Inc.
// Portions copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "supervisor.h"

#include <sys/signal.h>
#include <sys/wait.h>

#include <unordered_map>

#include "base/singleton.h"
#include "base/socket.h"
#include "base/unique_fd.h"
#include "dbus/bus_readable_message.h"
#include "dbus/flatpak_portal_proxy.h"
#include "preload/host/spawn_strategy/spawn_request.h"

namespace zypak::preload {

using namespace supervisor_internal;

// static
Supervisor* Supervisor::Acquire() {
  static Singleton<Supervisor> instance;
  return instance.get();
}

bool Supervisor::InitAndAttachToBusThread(dbus::Bus* bus) {
  auto request_pair = Socket::OpenSocketPair();
  if (!request_pair) {
    return false;
  }

  auto [supervisor_end, child_end] = std::move(*request_pair);

  if (!Socket::EnableReceivePid(supervisor_end.get())) {
    Log() << "Cannot enable pid receival on supervisor fd";
    return false;
  }

  if (dup2(child_end.get(), kZypakSupervisorFd) == -1) {
    Errno() << "Failed to dup onto supervisor fd";
    return false;
  }

  request_fd_ = std::move(supervisor_end);

  bus->evloop()->Acquire()->AddFd(
      request_fd_.get(), EvLoop::Events::Status::kRead,
      std::bind(&Supervisor::HandleSpawnRequest, this, std::placeholders::_1));
  portal_.AttachToBus(bus);

  portal_.SubscribeToSpawnStarted(
      std::bind(&Supervisor::HandleSpawnStarted, this, std::placeholders::_1));
  portal_.SubscribeToSpawnExited(
      std::bind(&Supervisor::HandleSpawnExited, this, std::placeholders::_1));

  return true;
}

Supervisor::Result Supervisor::GetExitStatus(pid_t stub_pid, int* status) {
  StubPidData* data = nullptr;

  {
    auto stub_pids_data = stub_pids_data_.Acquire(GuardReleaseNotify::kAll);
    data = FindStubPidData(StubPid(stub_pid), *stub_pids_data);
    if (data == nullptr) {
      Debug() << "Could not find stub pid data for " << stub_pid;
      return Result::kNotFound;
    }

    if (!data->exit_status.has_value()) {
      Debug() << "Still running, try later for " << stub_pid;
      return Result::kTryLater;
    }

    stub_pids_data->erase(stub_pid);
  }

  ReapProcess(stub_pid, data, status);
  return Result::kOk;
}

Supervisor::Result Supervisor::WaitForExitStatus(pid_t stub_pid, int* status) {
  StubPidData* data = nullptr;

  {
    auto stub_pids_data =
        stub_pids_data_.AcquireWhen([this, stub_pid, &data](auto* stub_pids_data) {
          data = FindStubPidData(StubPid(stub_pid), *stub_pids_data);
          return data == nullptr || data->exit_status.has_value();
        });

    if (data == nullptr) {
      Debug() << "Could not find stub pid data, assuming dead for " << stub_pid;
      return Result::kNotFound;
    }

    stub_pids_data->erase(stub_pid);
  }

  ReapProcess(stub_pid, data, status);
  return Result::kOk;
}

Supervisor::Result Supervisor::SendSignal(pid_t stub_pid, int signal) {
  auto stub_pids_data = stub_pids_data_.Acquire(GuardReleaseNotify::kNone);
  auto it = stub_pids_data->find(stub_pid);
  if (it == stub_pids_data->end()) {
    return Result::kNotFound;
  }

  if (std::optional<dbus::InvocationError> error = portal_.SpawnSignalBlocking(stub_pid, signal)) {
    Log() << "Failed to call SpawnSignal(" << stub_pid << ',' << signal << "): " << *error;
    return Result::kFailed;
  }

  return Result::kOk;
}

Supervisor::Result Supervisor::FindInternalPidBlocking(pid_t stub_pid, pid_t* internal_pid) {
  StubPidData* data = nullptr;
  auto stub_pids_data = stub_pids_data_.AcquireWhen([this, stub_pid, &data](auto* stub_pids_data) {
    data = FindStubPidData(StubPid(stub_pid), *stub_pids_data);
    return data == nullptr || data->internal.pid != -1;
  });

  if (data == nullptr) {
    return Result::kNotFound;
  }

  ZYPAK_ASSERT(data->internal.pid != -1);
  *internal_pid = data->internal.pid;
  return Result::kOk;
}

Supervisor::StubPidData*
Supervisor::FindStubPidData(ExternalPid external,
                            const std::unordered_map<StubPid, StubPidData>& stub_pids_data) {
  auto it = external_to_stub_pids_.find(external);
  if (it == external_to_stub_pids_.end()) {
    Log() << "External pid " << external.pid << " has no associated stub PID";
    return nullptr;
  }

  StubPidData* data = FindStubPidData(it->second, stub_pids_data);
  ZYPAK_ASSERT(data != nullptr ? data->external.pid == external.pid : true);
  return data;
}

Supervisor::StubPidData*
Supervisor::FindStubPidData(StubPid stub,
                            const std::unordered_map<StubPid, StubPidData>& stub_pids_data) {
  auto it = stub_pids_data.find(stub);
  if (it == stub_pids_data.end()) {
    Debug() << "Can't find stub pid data " << stub.pid;
    return nullptr;
  }

  return const_cast<StubPidData*>(&it->second);
}

void Supervisor::ReapProcess(StubPid stub, StubPidData* data, int* status) {
  Debug() << "Reaping " << stub.pid;

  ZYPAK_ASSERT(data->exit_status.has_value());
  *status = data->exit_status.value();

  if (!Socket::Write(data->notify_exit.get(), kZypakSupervisorExitReply)) {
    Errno() << "Failed to let stub process " << stub.pid << " know of exit";
    if (kill(stub.pid, SIGKILL) == -1) {
      Errno() << "Failed to emergency kill " << stub.pid;
    }
  }

  if (HANDLE_EINTR(waitpid(stub.pid, nullptr, 0)) == -1) {
    Errno() << "Failed to wait for stub process " << stub.pid;
  }
}

void Supervisor::HandleSpawnStarted(dbus::FlatpakPortalProxy::SpawnStartedMessage message) {
  auto stub_pids_data = stub_pids_data_.Acquire(GuardReleaseNotify::kAll);
  StubPidData* data = FindStubPidData(ExternalPid(message.external_pid), *stub_pids_data);
  if (data == nullptr) {
    Log() << "SpawnStarted handler could not find stub pid data";
    return;
  }

  Debug() << "Marking as started: " << message.external_pid << ' ' << message.internal_pid;
  data->internal = message.internal_pid;
}

void Supervisor::HandleSpawnExited(dbus::FlatpakPortalProxy::SpawnExitedMessage message) {
  auto stub_pids_data = stub_pids_data_.Acquire(GuardReleaseNotify::kAll);
  StubPidData* data = FindStubPidData(ExternalPid(message.external_pid), *stub_pids_data);
  if (data == nullptr) {
    Log() << "SpawnExited handler could not find stub pid data";
    return;
  }

  if (!Socket::Write(data->notify_exit.get(), kZypakSupervisorExitReply)) {
    Errno() << "Failed to let stub process know of exit";
    return;
  }

  Debug() << "Marking as dead: " << message.external_pid;
  data->exit_status = message.exit_status;
}

void Supervisor::HandleSpawnRequest(EvLoop::SourceRef source) {
  // Include the null terminator.
  std::array<std::byte, kZypakSupervisorSpawnRequest.size() + 1> buffer;
  std::vector<unique_fd> fds;
  pid_t stub_pid;

  Debug() << "Ready to read spawn request";

  Socket::ReadOptions options;
  options.fds = &fds;
  options.pid = &stub_pid;
  if (ssize_t bytes_read = Socket::Read(request_fd_.get(), &buffer, options); bytes_read == -1) {
    Errno() << "Failed to read spawn request";
    return;
  }

  Debug() << "Read spawn request";

  if (kZypakSupervisorSpawnRequest != reinterpret_cast<const char*>(buffer.data())) {
    Log() << "Invalid supervisor spawn request data";
    return;
  }

  if (fds.size() != 1) {
    Log() << "Expected one of from supervisor client, got " << fds.size();
    return;
  }

  unique_fd communication_fd = std::move(fds[0]);

  if (!FulfillSpawnRequest(
          &portal_, communication_fd,
          std::bind(&Supervisor::HandleSpawnReply, this, stub_pid, std::placeholders::_1))) {
    Log() << "Failed to fulfill spawn request";
    return;
  }

  Debug() << "Starting as " << stub_pid;
  auto stub_pids_data = stub_pids_data_.Acquire(GuardReleaseNotify::kNone);
  auto it = stub_pids_data->emplace(stub_pid, StubPidData{}).first;
  it->second.notify_exit = std::move(communication_fd);
}

void Supervisor::HandleSpawnReply(pid_t stub_pid, dbus::FlatpakPortalProxy::SpawnReply reply) {
  Debug() << "Got bus reply for " << stub_pid;

  auto stub_pids_data = stub_pids_data_.Acquire(GuardReleaseNotify::kNone);
  auto it = stub_pids_data->find(stub_pid);

  if (it == stub_pids_data->end()) {
    Log() << "Stub PID " << stub_pid << " has no data entry";
    return;
  }

  if (dbus::InvocationError* error = std::get_if<dbus::InvocationError>(&reply)) {
    Log() << "Failed to call Spawn: " << *error;
    stub_pids_data->erase(it);
    return;
  }

  pid_t external_pid = std::get<std::uint32_t>(reply);

  Debug() << "Initially spawned " << external_pid << " as " << stub_pid;

  it->second.external = external_pid;
  external_to_stub_pids_.emplace(external_pid, stub_pid);
}

}  // namespace zypak::preload
