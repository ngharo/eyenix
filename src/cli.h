#pragma once

// `eyenix dump`: prints live gaze to the terminal, talking to the tracker
// directly. Needs no display and no running app.
int runDump(int argc, char *argv[]);

// True for the commands that drive the running app: overlay, align,
// calibrate, profile, status, quit and plugin.
bool isControlCommand(const char *name);

// Runs one of those. They need no display either; the ones that have
// something to show start the app in the background if it is not running.
int runControl(int argc, char *argv[]);
