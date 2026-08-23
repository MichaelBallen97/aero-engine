#pragma once
// Aero Engine — the audio layer's umbrella header (task 3.7.1), matching <aero/render/render.hpp>
// and <aero/rhi/rhi.hpp>. Include this to get the whole public surface; include the individual
// headers when you want a narrower dependency.
//
// THE ONE RULE A CONSUMER OF THIS LAYER CAN GET WRONG (task 3.7.2): **DECLARE THE DEVICE AFTER THE
// SYSTEM.** A platform::AudioDevice holds an AudioSystem* as its renderUser, and ma_device_uninit
// stops the stream and JOINS the audio thread before it returns -- so ordinary reverse-order
// destruction tears the device down first, EVERY TIME, WITH NO FLAG AND NO HANDSHAKE. Declare them
// the other way round and the symptom is a use-after-free on a thread nobody is looking at.
//
//     auto system = engine::audio::AudioSystem::create();          // FIRST
//     auto device = engine::platform::AudioDevice::open(           // SECOND
//         {.render = &engine::audio::AudioSystem::renderCallback, .renderUser = system.get()});
//
// The rule is written in exactly three places, and this is one of them: here, on AudioSystem itself
// in system.hpp, and beside the two declarations in samples/phase-3-audio/main.cpp.

#include <aero/audio/clip.hpp>     // task 3.7.1
#include <aero/audio/mixer.hpp>    // task 3.7.2
#include <aero/audio/spatial.hpp>  // task 3.7.2
#include <aero/audio/system.hpp>   // task 3.7.2
