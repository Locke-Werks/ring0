// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class QQmlEngine;

// Installs the async provider behind `image://procicon/<url-encoded full path>`.
void register_icon_provider(QQmlEngine& engine);
