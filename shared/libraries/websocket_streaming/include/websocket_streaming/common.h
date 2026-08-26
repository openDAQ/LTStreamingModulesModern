/*
 * Copyright 2022-2025 openDAQ d.o.o.
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

#pragma once

#if DAQMODULES_LT_STREAMING_ENABLE_TLS && !WS_STREAMING_ENABLE_TLS
#error "DAQMODULES_LT_STREAMING_ENABLE_TLS is on, but ws-streaming was built without TLS support. \
Rebuild ws-streaming with WS_STREAMING_ENABLE_TLS=ON, or configure this project with \
DAQMODULES_LT_STREAMING_ENABLE_TLS=OFF."
#endif

#define BEGIN_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING namespace daq::websocket_streaming {
#define END_NAMESPACE_OPENDAQ_WEBSOCKET_STREAMING }
