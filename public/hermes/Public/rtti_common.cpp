/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/Public/CrashManager.h"

/// This file provides "key functions" for types in the API that have virtual
/// methods. This ensures that the type information for these types is only
/// generated and exported from one place, to avoid bugs arising from duplicate
/// type information across shared library boundaries.

namespace hermes {
namespace vm {
CrashManager::~CrashManager() {}
NopCrashManager::~NopCrashManager() {}
} // namespace vm
} // namespace hermes
