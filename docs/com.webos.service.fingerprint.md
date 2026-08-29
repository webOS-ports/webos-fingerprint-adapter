# com.webos.service.fingerprint

API reference for the luna-service2 (LS2) API exposed by
`webos-fingerprint-adapter`.

## API Summary

Provides fingerprint enrollment, identification and management on LuneOS by
bridging the [droidian-fpd](https://github.com/droidian/droidian-fpd) D-Bus
daemon onto the LS2 bus. On Halium devices the fingerprint sensor is only
reachable through the Android biometrics HAL; fpd wraps that HAL and this
service translates its D-Bus API into a webOS-style service API.

    fingerprint HAL ──hybris──▶ droidian-fpd ──D-Bus──▶ webos-fingerprint-adapter ──LS2──▶ apps

| | |
|---|---|
| Service name | `com.webos.service.fingerprint` |
| Category | `/` (root) |
| Executable | `@WEBOS_INSTALL_SBINDIR@/webos-fingerprint-adapter` |
| Trust level | `oem`, `privileged` |
| Upstream daemon | `org.droidian.fingerprint` on the D-Bus system bus |

### Methods

| Method | Description | Subscribable |
|---|---|---|
| [`getStatus`](#getstatus) | Daemon availability, operation state and enrolled fingerprints | Yes |
| [`getFingerprints`](#getfingerprints) | The enrolled fingerprint names | No |
| [`enroll`](#enroll) | Enroll a new fingerprint | Yes (progress/result) |
| [`identify`](#identify) | Arm the sensor and match a touch against the enrolled fingerprints | Yes (progress/result) |
| [`abort`](#abort) | Cancel the running enroll or identify | No |
| [`remove`](#remove) | Remove one enrolled fingerprint | No |
| [`rename`](#rename) | Rename an enrolled fingerprint | No |
| [`clear`](#clear) | Remove all enrolled fingerprints | No |

### Access Control Groups

| ACG | Methods |
|---|---|
| `fingerprint.operation` | all methods |
| `fingerprint.query` | `getStatus`, `getFingerprints` |

`com.webos.surfacemanager*` (the compositor, for the lockscreen) is granted
both groups out of the box; other clients need the matching ACG in their
manifest.

## General Behavior

### Availability

The adapter watches the `org.droidian.fingerprint` bus name. fpd restarting,
or not being installed at all, is a normal state rather than an error:
`getStatus` reports `"available": false` and the service re-attaches
automatically when fpd (re)appears. `available` becomes `true` once fpd is on
the bus **and** has answered the adapter's first state query.

While fpd is unavailable, operation methods (`enroll`, `identify`, `abort`,
`remove`, `rename`, `clear`) fail with
`"errorText": "Fingerprint daemon not available"`.

### Reply conventions

Every reply carries `returnValue` (boolean). Failures set
`"returnValue": false` and describe the problem in `errorText` (string).
This service does not use numeric `errorCode` values. See the
[error reference](#error-reference).

### Subscription semantics

`getStatus`, `enroll` and `identify` accept `"subscribe": true`. Subscription
updates are posted per **method**, not per caller: every subscriber of a
method receives its updates, including updates for an operation started by
another client. Only one enroll or identify can run at a time anyway (fpd
rejects a second one with `ALREADY_BUSY`), so in practice the updates belong
to the single running operation.

For `enroll` and `identify`, updates that end the operation carry
`"finished": true`; treat the subscription as done (and the sensor as
disarmed) only when that flag arrives. Updates without it — enroll progress,
acquisition feedback, per-touch identify misses — are informational and the
operation continues.

## Methods

### getStatus

Reports the daemon state and the enrolled fingerprints. Subscribable:
subscribers get an update whenever the availability, the operation state or
the fingerprint list changes.

#### Parameters

| Name | Required | Type | Description |
|---|---|---|---|
| `subscribe` | Optional | Boolean | Subscribe to status updates. Default: `false`. |

#### Call Returns

| Name | Type | Description |
|---|---|---|
| `returnValue` | Boolean | Always `true`. |
| `subscribed` | Boolean | Whether the subscription was accepted. |
| `available` | Boolean | `true` once fpd is running and answered its first state query. |
| `state` | String | fpd's `FPSTATE_*` string; `"FPSTATE_UNKNOWN"` while unavailable. See [FPSTATE values](#fpstate-values). |
| `fingerprints` | String array | The enrolled fingerprint names. Empty while unavailable. |

#### Subscription Returns

Same fields as the call return, without `subscribed`.

#### Example

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

---

### getFingerprints

Returns the enrolled fingerprint names. One-shot convenience for clients
that don't need the full status subscription.

#### Parameters

None.

#### Call Returns

| Name | Type | Description |
|---|---|---|
| `returnValue` | Boolean | Always `true`. |
| `fingerprints` | String array | The enrolled fingerprint names. Empty while fpd is unavailable. |

#### Example

    luna-send -n 1 luna://com.webos.service.fingerprint/getFingerprints '{}'

```json
{"returnValue": true, "fingerprints": ["right-index", "left-thumb"]}
```

---

### enroll

Enrolls a new fingerprint under the given name. Enrollment takes many touches
of the sensor, so call this with `"subscribe": true`: the progress and the
result arrive as updates on the subscription. The first reply only confirms
that the operation started.

#### Parameters

| Name | Required | Type | Description |
|---|---|---|---|
| `finger` | Required | String | Name to enroll under (e.g. `"right-index"`). Must not already exist. |
| `subscribe` | Optional | Boolean | Subscribe to progress and result updates. Strongly recommended — without it only the started/failed-to-start reply is delivered. |

#### Call Returns

| Name | Type | Description |
|---|---|---|
| `returnValue` | Boolean | `true` if enrollment started. |
| `subscribed` | Boolean | Whether the subscription was accepted. |
| `errorText` | String | On failure to start. See [error reference](#error-reference). |

#### Subscription Returns

Each update contains `returnValue` plus **one** of the following:

| Name | Type | Description |
|---|---|---|
| `progress` | Number | Enrollment progress, 0–100. |
| `acquisitionInfo` | String | Per-touch sensor feedback, an `FPACQUIRED_*` string. Informational; show it to the user (e.g. "press harder", "sensor dirty"). |
| `finished` + `finger` | Boolean + String | `"finished": true` with the enrolled name: enrollment succeeded and the fingerprint was stored. |
| `finished` + `errorText` | Boolean + String | With `"returnValue": false`: enrollment ended without storing anything. `"Aborted"` for a cancel, otherwise fpd's failure reason (an `FPERROR_*` string) or `"Enrollment failed"`. |

#### Example

    luna-send -i luna://com.webos.service.fingerprint/enroll \
        '{"finger":"right-index","subscribe":true}'

```json
{"returnValue": true, "subscribed": true}
{"returnValue": true, "acquisitionInfo": "FPACQUIRED_GOOD"}
{"returnValue": true, "progress": 20}
{"returnValue": true, "acquisitionInfo": "FPACQUIRED_PARTIAL"}
{"returnValue": true, "progress": 40}
{"returnValue": true, "progress": 100}
{"returnValue": true, "finished": true, "finger": "right-index"}
```

A failed or aborted enrollment ends with:

```json
{"returnValue": false, "finished": true, "errorText": "Aborted"}
```

---

### identify

Arms the sensor and reports whether a touch matched an enrolled fingerprint.
Subscribable; call with `"subscribe": true` to receive the result. fpd
cancels an identify on its own after 30 seconds without a match — re-issue
`identify` to keep the sensor armed longer (e.g. on a lockscreen).

A non-matching touch does **not** end the operation: fpd keeps the sensor
armed and the adapter posts a non-terminal miss update, so the client can show
per-touch feedback (and count attempts) without re-arming.

#### Parameters

| Name | Required | Type | Description |
|---|---|---|---|
| `subscribe` | Optional | Boolean | Subscribe to the result. Strongly recommended. |

#### Call Returns

| Name | Type | Description |
|---|---|---|
| `returnValue` | Boolean | `true` if the sensor was armed. |
| `subscribed` | Boolean | Whether the subscription was accepted. |
| `errorText` | String | On failure to start, e.g. `"No fingerprints are enrolled"`. |

#### Subscription Returns

| Name | Type | Description |
|---|---|---|
| `finished` | Boolean | `true` on the update that ends the operation. |
| `identified` | Boolean | Whether the touch matched an enrolled fingerprint. |
| `finger` | String | On a match: the name of the matched fingerprint. |
| `acquisitionInfo` | String | Per-touch sensor feedback, an `FPACQUIRED_*` string. |
| `errorText` | String | `"FINGER_NOT_RECOGNIZED"` on a per-touch miss (`"finished": false` — sensor stays armed), `"Aborted"` when the operation was cancelled or timed out. |

The three shapes a client should handle:

```json
{"returnValue": true,  "finished": true,  "identified": true,  "finger": "right-index"}
{"returnValue": true,  "finished": false, "identified": false, "errorText": "FINGER_NOT_RECOGNIZED"}
{"returnValue": false, "finished": true,  "identified": false, "errorText": "Aborted"}
```

#### Example

    luna-send -i luna://com.webos.service.fingerprint/identify '{"subscribe":true}'

---

### abort

Cancels the running enroll or identify. The cancelled operation's subscribers
receive a terminal `"errorText": "Aborted"` update on their own subscription.
Aborting while nothing is running succeeds as well.

#### Parameters

None.

#### Call Returns

| Name | Type | Description |
|---|---|---|
| `returnValue` | Boolean | `true` when the abort was issued (or nothing was running). |
| `errorText` | String | On failure. |

#### Example

    luna-send -n 1 luna://com.webos.service.fingerprint/abort '{}'

---

### remove

Removes one enrolled fingerprint by name. The reply confirms the removal
started; the updated list reaches `getStatus` subscribers once fpd finishes.

#### Parameters

| Name | Required | Type | Description |
|---|---|---|---|
| `finger` | Required | String | Name of the fingerprint to remove. |

#### Call Returns

| Name | Type | Description |
|---|---|---|
| `returnValue` | Boolean | `true` if the removal started. |
| `errorText` | String | On failure, e.g. `"No fingerprint with that name exists"`. |

#### Example

    luna-send -n 1 luna://com.webos.service.fingerprint/remove '{"finger":"right-index"}'

---

### rename

Renames an enrolled fingerprint. The updated list reaches `getStatus`
subscribers once fpd finishes.

#### Parameters

| Name | Required | Type | Description |
|---|---|---|---|
| `finger` | Required | String | Current name of the fingerprint. |
| `newName` | Required | String | New name. Must not already exist. |

#### Call Returns

| Name | Type | Description |
|---|---|---|
| `returnValue` | Boolean | `true` if the rename started. |
| `errorText` | String | On failure, e.g. `"A fingerprint with that name already exists"`. |

#### Example

    luna-send -n 1 luna://com.webos.service.fingerprint/rename \
        '{"finger":"right-index","newName":"right-thumb"}'

---

### clear

Removes all enrolled fingerprints. fpd's `Clear` call carries no reply value
on the wire, so `"returnValue": true` confirms delivery to fpd; the emptied
list reaches `getStatus` subscribers once fpd finishes.

#### Parameters

None.

#### Call Returns

| Name | Type | Description |
|---|---|---|
| `returnValue` | Boolean | `true` when the request reached fpd. |
| `errorText` | String | On failure (fpd unavailable or the D-Bus call failed). |

#### Example

    luna-send -n 1 luna://com.webos.service.fingerprint/clear '{}'

## Error Reference

Errors are reported as `"returnValue": false` with an `errorText`; there are
no numeric error codes.

### Adapter errors

| errorText | Cause |
|---|---|
| `Malformed json.` | The payload was not valid JSON. |
| `Invalid parameters.` | A required parameter (`finger`, `newName`) is missing, empty or not a string. |
| `Fingerprint daemon not available` | fpd is not on the D-Bus system bus. |

### fpd reply errors

Translations of the `fpreply` codes fpd returns when refusing an operation:

| errorText | fpd code | Cause |
|---|---|---|
| `Operation failed` | `FAILED` | Generic failure starting the operation. |
| `No operation in progress` | `ALREADY_IDLE` | (Not surfaced by `abort`, which treats idle as success.) |
| `Another fingerprint operation is in progress` | `ALREADY_BUSY` | An enroll or identify is already running; `abort` it first. |
| `Operation denied` | `DENIED` | fpd refused the operation. |
| `A fingerprint with that name already exists` | `KEY_ALREADY_EXISTS` | `enroll`/`rename` target name is taken. |
| `No fingerprint with that name exists` | `KEY_DOES_NOT_EXIST` | `remove`/`rename` source name is unknown. |
| `No fingerprints are enrolled` | `NO_KEYS_AVAILABLE` | `identify` with an empty fingerprint store. |
| `Invalid fingerprint name` | `KEY_IS_INVALID` | fpd rejected the name. |

If the D-Bus call itself fails (fpd crashed mid-call, request timed out), the
GLib error message is passed through as `errorText` verbatim.

### Terminal subscription errors

| errorText | Meaning |
|---|---|
| `Aborted` | The operation was cancelled via `abort` or timed out (identify auto-cancels after 30 s). Never counts as a failed read. |
| `FINGER_NOT_RECOGNIZED` | identify only, with `"finished": false` — a single non-matching touch; the sensor stays armed. |
| `FPERROR_*` / `Enrollment failed` | fpd reported the enrollment failed; the `FPERROR_*` string is fpd's reason when it sent one. |

## Enumerations

These string values originate in droidian-fpd (which maps them from the
Android biometrics HAL); the adapter passes them through unmodified, so the
exact set depends on the fpd version and the device.

### FPSTATE values

Reported in `getStatus` as `state`.

| Value | Meaning |
|---|---|
| `FPSTATE_UNKNOWN` | fpd is unavailable (adapter-generated, always paired with `"available": false`). |
| `FPSTATE_IDLE` | No operation running. |
| `FPSTATE_ENROLLING` | An enrollment is in progress. |
| `FPSTATE_IDENTIFYING` | The sensor is armed for identification. |

fpd may report further transient `FPSTATE_*` values (e.g. while enumerating
or removing); clients should treat unknown states as busy.

### FPACQUIRED values

Reported in `acquisitionInfo` updates; per-touch feedback from the sensor,
e.g. `FPACQUIRED_GOOD`, `FPACQUIRED_PARTIAL`, `FPACQUIRED_INSUFFICIENT`,
`FPACQUIRED_IMAGER_DIRTY`, `FPACQUIRED_TOO_SLOW`, `FPACQUIRED_TOO_FAST`.
Informational only — map them to user-facing hints and ignore values you
don't recognize.

## Appendix: the upstream D-Bus API

For reference, the D-Bus interface the adapter consumes (see
`files/xml/fpd.xml` for the full introspection XML):

| | |
|---|---|
| Bus | system |
| Name | `org.droidian.fingerprint` |
| Object path | `/org/droidian/fingerprint` |
| Interface | `org.droidian.fingerprint` |

Methods: `Enroll(s)→i`, `Identify()→i`, `Verify()→i`, `Abort()→i`,
`Remove(s)→i`, `Rename(s,s)→i`, `Clear()`, `GetState()→s`, `GetAll()→as`.
Signals: `Added(s)`, `Removed(s)`, `Identified(s)`, `Aborted`, `Failed`,
`Verified`, `StateChanged(s)`, `EnrollProgressChanged(i)`,
`AcquisitionInfo(s)`, `ErrorInfo(s)`, `ListChanged`.

How LS2 traffic maps onto it:

| LS2 | D-Bus |
|---|---|
| `getStatus` | `GetState` + `GetAll`, updated from `StateChanged` and `ListChanged` |
| `getFingerprints` | cached `GetAll` result |
| `enroll` | `Enroll`; updates from `EnrollProgressChanged`, `AcquisitionInfo`; terminal from `Added`/`Failed`/`Aborted` |
| `identify` | `Identify`; misses from `ErrorInfo("FINGER_NOT_RECOGNIZED")`; terminal from `Identified`/`Failed`/`Aborted` |
| `abort` | `Abort` (`ALREADY_IDLE` treated as success) |
| `remove` / `rename` / `clear` | `Remove` / `Rename` / `Clear` |

The interface is the community reimplementation of Sailfish's
`org.sailfishos.fingerprint1` API plus the `Clear()` addition, renamed to
`org.droidian.fingerprint`. `Verify`/`Verified` are present in the interface
but not exposed over LS2.
