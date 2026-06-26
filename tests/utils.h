/**
 * @file utils.h
 * @brief Reusable functions for tests.
 */
#pragma once

// standard includes
#include <string>

int setEnv(const std::string &name, const std::string &value);

bool isGitHubActions();

void dismissNativeNotifications();

void waitForNativeNotificationTimeout();
