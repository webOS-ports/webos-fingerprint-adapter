webos-fingerprint-adapter
=========================

Summary
-------
Bridges the droidian-fpd D-Bus API onto the luna-service2 bus as `com.webos.service.fingerprint`.

Description
-----------
On Halium devices the fingerprint sensor is reached through the Android
biometrics HAL rather than anything the Linux side knows natively, so LuneOS
uses [droidian-fpd](https://github.com/droidian/droidian-fpd) (the Droidian
fork of
[sailfish-fpd-community](https://github.com/sailfishos-open/sailfish-fpd-community),
which reuses the HAL bridge from UBports
[biometryd](https://gitlab.com/ubports/development/core/biometryd)). fpd speaks
D-Bus; webOS apps and the shell speak luna-service2. This daemon sits between
the two.

    fingerprint HAL  ──hybris──▶  droidian-fpd  ──D-Bus──▶  webos-fingerprint-adapter  ──LS2──▶  apps

It watches the fpd bus name, so fpd restarting, or not being installed at all,
is a normal state rather than an error: the service simply reports
`available: false` and re-attaches when fpd comes back.

Luna service API
----------------

### com.webos.service.fingerprint/getStatus

Subscribable. Reports the daemon state and the enrolled fingerprints.

    luna-send -i -n 2 luna://com.webos.service.fingerprint/getStatus '{"subscribe":true}'

```json
{
    "returnValue": true,
    "subscribed": true,
    "available": true,
    "state": "FPSTATE_IDLE",
    "fingerprints": ["right-index"]
}
```

`available` is true once fpd is running and answered its first state query.
`state` is fpd's FPSTATE_* string (`FPSTATE_UNKNOWN` while unavailable).
Subscribers get an update whenever the state, the finger list or the
availability changes.

### com.webos.service.fingerprint/getFingerprints

Returns the enrolled fingerprint names.

    luna-send -n 1 luna://com.webos.service.fingerprint/getFingerprints '{}'

### com.webos.service.fingerprint/enroll

Subscribable; enrollment takes many touches of the sensor, so the progress and
the result come as updates on the subscription.

    luna-send -i luna://com.webos.service.fingerprint/enroll \
        '{"finger":"right-index","subscribe":true}'

The first reply confirms the operation started. Updates then look like:

```json
{"returnValue": true, "progress": 40}
{"returnValue": true, "acquisitionInfo": "FPACQUIRED_PARTIAL"}
{"returnValue": true, "finished": true, "finger": "right-index"}
```

A failed or aborted enrollment ends with
`{"returnValue": false, "finished": true, "errorText": "..."}`.

### com.webos.service.fingerprint/identify

Subscribable. Arms the sensor and reports whether the touch matched an
enrolled fingerprint. fpd cancels an identify on its own after 30 seconds.

    luna-send -i luna://com.webos.service.fingerprint/identify '{"subscribe":true}'

```json
{"returnValue": true, "finished": true, "identified": true, "finger": "right-index"}
```

A non-match ends with `"identified": false` and an `errorText` such as
`FINGER_NOT_RECOGNIZED`.

### com.webos.service.fingerprint/abort

Cancels the running enroll or identify. Aborting while idle succeeds.

### com.webos.service.fingerprint/remove

Removes one enrolled fingerprint by name:

    luna-send -n 1 luna://com.webos.service.fingerprint/remove '{"finger":"right-index"}'

The reply confirms the removal started; the list update reaches getStatus
subscribers once fpd finishes.

### com.webos.service.fingerprint/clear

Removes all enrolled fingerprints.

How to Build on Linux
=====================

## Dependencies

* cmake (version required by openwebos/cmake-modules-webos)
* gcc
* glib-2.0, gio-2.0, gio-unix-2.0, gobject-2.0
* gdbus-codegen (from glib-2.0, used at configure time)
* openwebos/luna-service2
* openwebos/pbnjson_c
* pkg-config

## Building

    $ mkdir BUILD
    $ cd BUILD
    $ cmake ..
    $ make
    $ sudo make install

The directory under which the files are installed defaults to
`/usr/local/webos`. Supply `WEBOS_INSTALL_ROOT` to `cmake` to change that.

# Copyright and License Information

Unless otherwise specified, all content, including all source code files and
documentation files in this repository are:

Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
this file except in compliance with the License. You may obtain a copy of the
License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.

SPDX-License-Identifier: Apache-2.0
