#pragma once

// Single-instance handoff plumbing (Windows-only). When enabled, a
// second launch of BananaPlayer.exe reads any path/URL args and forwards them
// to the running instance via a named pipe, then exits without opening
// a new window.
//
// The primary process holds a named mutex + runs a background thread
// listening on the named pipe. Both are set up by activate() and torn
// down by deactivate(). SDL user events are pushed by the listener so
// the main thread can loadPath the forwarded arg from within its own
// event loop.
//
// This lives in its own translation unit (rather than static globals
// inside main.cpp) so UIController can flip the mode on / off mid-
// session in response to the Settings toggle.

#include <SDL3/SDL.h>

#include <string>

namespace SingleInstance {

// Try to make this process the primary instance: acquire the named
// mutex and start the pipe listener. Returns true on success. Idempotent
// (a second activate() on an already-primary process just returns true).
// Returns false when another instance already holds the mutex - the
// caller should probably fall through to the forward-and-exit path.
bool activate();

// Release the mutex and stop the pipe listener. Idempotent. Safe to
// call even if activate() failed / was never called.
void deactivate();

// True when THIS process currently holds the primary-instance mutex.
bool isPrimary();

// True when another process holds the primary-instance mutex.
// activate() returned false because of this. Independent of whether
// THIS process is primary.
bool anotherInstanceRunning();

// Register the SDL user-event type used by the pipe listener to hand
// forwarded paths to the main thread. Call once after SDL_Init.
// Returns the registered event id (or (Uint32)-1 on failure).
Uint32 registerSdlEvent();

// The SDL user-event id set up by registerSdlEvent(). data1 is a
// heap-allocated std::string* the receiver takes ownership of.
Uint32 sdlEventType();

// Read banana.config for the persisted single_instance setting. Runs
// standalone (no UIController state) so main.cpp can decide policy
// BEFORE SDL init.
bool peekPolicyFromConfig();

// Forward one path (or URL) to the currently-running primary. Returns
// true on write success. Best-effort - a false return means the caller
// should fall back to opening a fresh window.
bool forwardPathToPrimary(const std::string& path);

}   // namespace SingleInstance
