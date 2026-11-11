// Quiet updates from GitHub (updater.cpp).
#pragma once

// Starts the once-a-boot check on a thread of its own; call it once
// SceShell's UI is running and has settled.
void startUpdateCheck();
