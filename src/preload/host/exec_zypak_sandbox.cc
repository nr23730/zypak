// Copyright 2019 Endless Mobile, Inc.
// Portions copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/stat.h>

#include <cstring>
#include <string_view>
#include <vector>

#include "base/base.h"
#include "base/env.h"
#include "base/file_util.h"
#include "base/str_util.h"
#include "preload/declare_override.h"
#include "preload/host/sandbox_path.h"

// If exec is run, make sure it runs the zypak-provided sandbox binary instead of the normal
// Chrome one.

namespace {

using namespace zypak;

constexpr std::string_view kElectronRelauncherType = "relauncher";
constexpr cstring_view kElectronRelauncherFdMap = "3=3";

std::optional<cstring_view> GetTypeArg(char* const* argv) {
  constexpr cstring_view kTypeArgPrefix = "--type=";

  for (; *argv != nullptr; argv++) {
    cstring_view arg = *argv;
    if (StartsWith(arg, kTypeArgPrefix)) {
      arg.remove_prefix(kTypeArgPrefix.size());
      return arg;
    }
  }

  return {};
}

}  // namespace

DECLARE_OVERRIDE(int, execvp, const char* file, char* const* argv) {
  auto original = LoadOriginal();

  if (*argv == NULL) {
    errno = EINVAL;
    return -1;
  }

  Env::Clear("LD_PRELOAD");

  if (file == SandboxPath::instance()->sandbox_path()) {
    file = "zypak-sandbox";
  } else if (auto type = GetTypeArg(argv); (!type || type == kElectronRelauncherType) &&
                                           PathsPointToSameFile(file, kCurrentExe)) {
    // exec on the host exe, so pass it through the sandbox.
    // "Leaking" calls to 'new' doesn't matter here since we're about to exec anyway.
    std::vector<const char*> c_argv;
    c_argv.push_back("zypak-helper");

    if (Env::Test(Env::kZypakSettingSpawnLatestOnReexec, /*default_value=*/false)) {
      if (auto wrapper = Env::Get("CHROME_WRAPPER")) {
        // Swap out the main binary to the wrapper if one was used, and assume the wrapper
        // will use zypak-wrapper.sh itself (i.e. we don't need to handle it here).
        c_argv.push_back("exec-latest");
        c_argv.push_back(wrapper->data());
        argv++;
      } else {
        c_argv.push_back("host-latest");
      }
    } else {
      c_argv.push_back("host");
      if (type == kElectronRelauncherType) {
        c_argv.push_back(kElectronRelauncherFdMap.c_str());
      }
      c_argv.push_back("-");
    }

    for (; *argv != nullptr; argv++) {
      c_argv.push_back(*argv);
    }

    c_argv.push_back(nullptr);

    return original("zypak-helper", const_cast<char* const*>(c_argv.data()));
  }

  return original(file, argv);
}
