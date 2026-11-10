// Quasar: one lock for the graphics queue and the swapchain.
//
// Frame generation presents from its own pacing thread (VkSwapchain's pacer), while the
// main thread submits, waits idle and acquires. Vulkan requires the host to synchronise a
// queue (vkQueueSubmit*, vkQueuePresentKHR, vkQueueWaitIdle, vkDeviceWaitIdle: every
// queue of the device) and a swapchain (vkAcquireNextImageKHR, vkQueuePresentKHR), so
// every one of those calls takes this lock. Uncontended (frame generation off) it costs
// a few nanoseconds a call.
#pragma once

#include <mutex>

std::mutex& qlQueueMutex();
