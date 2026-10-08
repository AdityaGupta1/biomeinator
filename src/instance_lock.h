// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

namespace InstanceLock
{

// Blocks until this instance may run: an exclusive instance waits for every other instance to exit, and any
// instance waits while an exclusive one is running. Held until the process exits.
void acquire(bool exclusive);

} // namespace InstanceLock
