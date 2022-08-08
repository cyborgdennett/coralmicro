/*
 * Copyright 2022 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LIBS_BASE_CONSOLE_M4_H_
#define LIBS_BASE_CONSOLE_M4_H_

#include "libs/base/ipc_message_buffer.h"

namespace coralmicro {

void ConsoleM4EmergencyWrite(const char* fmt, ...);
void ConsoleM4Init();
void ConsoleM4SetBuffer(IpcStreamBuffer* buffer);

}  // namespace coralmicro

#endif  // LIBS_BASE_CONSOLE_M4_H_
